#include "safety_core.h"
volatile uint32_t test_total,test_passed,test_failed,test_done;
volatile uint32_t failed_ids[64];
volatile uint32_t cov_sensor_on,cov_sensor_on_en_low,cov_actuator_on; /* second adversarial run */
volatile uint32_t cov_lockout_steps,cov_short_events,cov_guard_random_rejected; /* strike limiter */
volatile uint32_t cov_in_boot_strikes,cov_trip_events; /* in-boot (no reset) strike counting */
static void check(uint32_t id,bool pass) {
 test_total++;if(pass)test_passed++;else {failed_ids[test_failed%64]=id;test_failed++;}
}
static bool ack_enable(uint32_t t,uint32_t start) {
 if(t<start)return false;
 if(t<start+100)return true;
 if(t<start+200)return false;
 return true;
}
static safety_input normal(uint32_t t,safety_output old) {
 safety_input i={0};i.now_ms=t;i.enable=ack_enable(t,2100);
 i.heartbeat=(t%50)<25;i.hardware_fault_high=true;
 i.host_rail_ok=true;i.internal_ok=true;
 i.motor_feedback=old.motor_permit && i.enable;
 i.servo_feedback=old.servo_permit && i.enable;
 return i;
}
/* Reference sensor policy: fresh heartbeat (HB_MAX_MS=80), good fault/rail/internal
 * inputs, no latched fault and no strike lockout. GPIO26/enable and arming stage are
 * deliberately absent. */
static bool sensor_expected(const safety_state *s,const safety_input *i) {
 bool hb=s->have_hb && s->valid_hb_edges>=2u && (uint32_t)(i->now_ms-s->last_hb_fall)<=80u;
 return hb && i->hardware_fault_high && i->host_rail_ok && i->internal_ok && s->stage!=S_LATCHED &&
  !i->sensor_strike_lockout;
}
/* Everything except the sensor output must be independent of the strike lockout. */
static bool same_actuation(const safety_state *a,const safety_state *b,safety_output oa,safety_output ob) {
 return oa.motor_permit==ob.motor_permit && oa.servo_permit==ob.servo_permit && oa.ready==ob.ready &&
  oa.watchdog_level==ob.watchdog_level && a->stage==b->stage && a->faults==b->faults &&
  a->last_faults==b->last_faults && a->last_step==b->last_step && a->stage_since==b->stage_since &&
  a->low_since==b->low_since && a->last_hb_fall==b->last_hb_fall && a->servo_since==b->servo_since &&
  a->servo_not_before==b->servo_not_before && a->wd_last_fall==b->wd_last_fall &&
  a->wd_high_since==b->wd_high_since && a->changed_at==b->changed_at && a->boot_at==b->boot_at &&
  a->valid_hb_edges==b->valid_hb_edges && a->last_hb==b->last_hb && a->last_request==b->last_request &&
  a->low_timing==b->low_timing && a->have_hb==b->have_hb && a->started==b->started &&
  a->watchdog_high==b->watchdog_high && a->output.motor_permit==b->output.motor_permit &&
  a->output.servo_permit==b->output.servo_permit && a->output.ready==b->output.ready &&
  a->output.watchdog_level==b->output.watchdog_level;
}
static uint32_t grng=0x2545f491u;
static uint32_t rnd32(void){grng^=grng<<13;grng^=grng>>17;grng^=grng<<5;return grng;}
static sensor_guard guard_with(uint32_t strikes,uint32_t rise_age,bool high) {
 /* A sealed record built only through the public functions. */
 sensor_guard z={0},g=sensor_guard_boot(&z,&z,SR_POWER);
 sensor_guard_tick(&g,high,0,false);g.strikes=strikes;g.rise_age_ms=rise_age;
 sensor_guard_tick(&g,high,0,false); /* reseal; high equals the recorded state, so no new rise */
 return g;
}
/* Device model mirroring firmware.c around the pure functions: boot from the
 * two retained slots, then per 1 ms pass: lockout input (re-read every pass) ->
 * safety_step -> PA12 = core permit AND this boot has driven a WDI kick -> record
 * PA12 and any new F_EXTERNAL/F_RAIL latch -> drive PA12. A shorted sensor branch
 * faults short_ms after PA12 rises (or after the short appears at short_from, if
 * later). DK_RESET: the U30 FAULT pulls NRST (pin reset) and releases PA12.
 * DK_EXTI: EXTI6 catches the FAULT and drops PA12 before NRST acts; the MCU keeps
 * running and the next pass sees hardware_fault_edge. DK_RAIL: the switch-on
 * sags SAFE3 and the next pass sees host_rail_ok false. DK_EXTI_THEN_RESET: as
 * DK_EXTI, but the same FAULT also resets the MCU right after the pass that
 * latched it. The Pi heartbeat keeps running across MCU resets. A reference core
 * that never sees a lockout runs in lock-step to prove motor/servo/ready/WDI are
 * unaffected. */
enum { DK_RESET, DK_EXTI, DK_RAIL, DK_EXTI_THEN_RESET };
#define NO_SHORT 0xffffffffu
typedef struct { sensor_guard slot[2]; uint32_t t,short_from,short_events,sensor_on_ms,motor_on_ms,max_on_run,rises,trips,
 locked_rises; } device;
static bool device_run_k(device *d,enum sensor_reset cause,uint32_t run_ms,uint32_t short_ms,unsigned kind,bool ack,
 uint32_t ack_period,sensor_guard *out) {
 sensor_guard g=sensor_guard_boot(&d->slot[0],&d->slot[1],cause);d->slot[0]=d->slot[1]=g;
 safety_state s,r;safety_output o={0},ro={0};uint32_t boot=d->t,at=boot,rise=0,run=0;
 bool high=false,kicked=false,old_wd=false,edge=false,sag=false;
 safety_init(&s,boot);safety_init(&r,boot);
 for(uint32_t k=0;k<run_ms;k++) {
  uint32_t now=++d->t,rel=now-boot;
  safety_input i={0};i.now_ms=now;i.heartbeat=(now%50)<25;i.hardware_fault_high=true;
  i.host_rail_ok=!sag;i.internal_ok=true;i.hardware_fault_edge=edge;edge=sag=false;
  i.enable=ack && ack_enable(ack_period?rel%ack_period:rel,300);
  i.motor_feedback=o.motor_permit && i.enable;i.servo_feedback=o.servo_permit && i.enable;
  i.sensor_strike_lockout=sensor_guard_lockout(&g);
  safety_input ri=i;ri.sensor_strike_lockout=false;
  enum safety_stage before=s.stage;
  o=safety_step(&s,&i);ro=safety_step(&r,&ri);
  check(330,same_actuation(&s,&r,o,ro));
  check(331,!i.sensor_strike_lockout || !o.sensor_permit);
  check(332,i.sensor_strike_lockout || o.sensor_permit==ro.sensor_permit);
  bool trip=sensor_guard_trip(before,&s);
  check(333,trip==(before!=S_LATCHED && (!i.hardware_fault_high || i.hardware_fault_edge || !i.host_rail_ok)));
  bool on=o.sensor_permit && !i.sensor_strike_lockout && kicked;
  sensor_guard_tick(&g,on,now-at,trip);at=now;d->slot[0]=d->slot[1]=g;
  if(trip)d->trips++;
  if(on && !high){rise=now;run=0;d->rises++;if(i.sensor_strike_lockout)d->locked_rises++;}
  high=on;if(on){d->sensor_on_ms++;run++;if(run>d->max_on_run)d->max_on_run=run;}
  if(o.motor_permit)d->motor_on_ms++;
  if(old_wd && !o.watchdog_level)kicked=true;
  old_wd=o.watchdog_level;
  if(kind==DK_EXTI_THEN_RESET && trip){if(out)*out=g;return true;} /* NRST follows the EXTI-caught FAULT */
  uint32_t start=rise>d->short_from?rise:d->short_from;
  if(on && short_ms!=NO_SHORT && now>=d->short_from && now-start>=short_ms){
   d->short_events++;
   if(kind==DK_RESET){if(out)*out=g;return true;}
   if(kind==DK_RAIL)sag=true; else edge=true; /* ISR drops PA12; switch unlatches; next pass sees it */
   high=false; /* PA12 physically low from here; the record still says HIGH until the next pass */
  }
 }
 if(out)*out=g;
 return false;
}
static bool device_run(device *d,enum sensor_reset cause,uint32_t run_ms,uint32_t short_ms,bool ack,sensor_guard *out) {
 return device_run_k(d,cause,run_ms,short_ms?short_ms:NO_SHORT,DK_RESET,ack,0u,out);
}
int main(void) {
 safety_state s;safety_output o={0};uint32_t last;
 safety_init(&s,0);last=0;bool prev=false;uint32_t pulses=0;
 for(uint32_t t=1;t<=5000;t++) {
  safety_input i=normal(t,o);i.enable=false;i.heartbeat=false;
  o=safety_step(&s,&i);
  check(100,!o.motor_permit&&!o.servo_permit);
  check(190,!o.sensor_permit);
  if(prev&&!o.watchdog_level){check(101,t-last<=51);last=t;pulses++;}
  prev=o.watchdog_level;
 }check(102,pulses>=95);
 safety_init(&s,0);o=(safety_output){0};
 for(uint32_t t=1;t<=5000;t++){safety_input i=normal(t,o);i.enable=true;o=safety_step(&s,&i);check(103,!o.motor_permit&&!o.servo_permit);check(191,o.sensor_permit==(t>=75));}
 safety_init(&s,0);o=(safety_output){0};
 for(uint32_t t=1;t<=2450;t++){safety_input i=normal(t,o);o=safety_step(&s,&i);if(t<2400)check(104,!o.motor_permit);check(192,o.sensor_permit==(t>=75));}
 check(105,o.motor_permit && s.faults==0);
 for(unsigned mode=0;mode<3;mode++) {
  safety_init(&s,0);o=(safety_output){0};
  for(uint32_t t=1;t<=2600;t++) {
   safety_input i=normal(t,o);
   if(t==2150 && mode==0)i.pwm_edge_seen=true;
   if(t==2150 && mode==1)i.pwm_levels=1;
   if(mode==2 && t>=2000)i.servo_request=true;
   o=safety_step(&s,&i);
  }check(110+mode,!o.motor_permit&&!o.servo_permit);
 }
 for(unsigned mode=0;mode<4;mode++) {
  safety_init(&s,0);o=(safety_output){0};
  for(uint32_t t=1;t<=3000;t++) {
   safety_input i=normal(t,o);
   if(t>=2600) {
    if(mode==0)i.heartbeat=true;
    if(mode==1)i.heartbeat=false;
    if(mode==2)i.heartbeat=(t%120)<60;
    if(mode==3)i.heartbeat=(t%10)<5;
   }
   o=safety_step(&s,&i);
   if(t>=2700)check(120+mode,!o.motor_permit&&!o.servo_permit);
  }check(124+mode,(s.last_faults&F_HEARTBEAT)!=0);
 }
 for(unsigned mode=0;mode<4;mode++) {
  safety_init(&s,0);o=(safety_output){0};uint32_t on=0;
  for(uint32_t t=1;t<=3500;t++) {
   safety_input i=normal(t,o);
   i.servo_request=t>=2500;
   if(mode==0 && t>=2800)i.servo_request=false;
   if(mode==2 && t>=2510)i.servo_request=(t%20)<10;
   if(mode==3 && t==2520)i.pwm_edge_seen=true;
   o=safety_step(&s,&i);if(o.servo_permit)on++;
   check(130+mode,!(o.motor_permit&&o.servo_permit));
   if(t>=3000)check(134+mode,!o.motor_permit&&!o.servo_permit);
   if(mode==0 && t>=75)check(193,o.sensor_permit);
   if(mode==1 && t>=3000)check(194,!o.sensor_permit);
   if(mode==3 && t>=2520)check(195,!o.sensor_permit);
  }
  check(138+mode,on>0 && on<=500);
  if(mode==0)check(142,on==300);
  if(mode==1)check(143,(s.last_faults&F_SERVO_TIMEOUT)!=0);
 }
 safety_init(&s,0);o=(safety_output){0};
 for(uint32_t t=1;t<=3500;t++) {
  safety_input i=normal(t,o);i.servo_request=t>=2500&&t<2800;
  if(t>=2800)i.enable=ack_enable(t,3000);
  if(t>=3400)i.servo_request=true;
  o=safety_step(&s,&i);
 }check(150,(s.last_faults&F_COOLDOWN)!=0 && !o.servo_permit);
 safety_init(&s,2600);o=(safety_output){0};
 for(uint32_t t=2601;t<=3500;t++) {
  safety_input i=normal(t,o);i.enable=ack_enable(t,3000);
  i.servo_request=t>=3400;o=safety_step(&s,&i);
 }check(151,(s.last_faults&F_COOLDOWN)!=0&&!o.servo_permit);
 for(unsigned mode=0;mode<5;mode++) {
  safety_init(&s,0);o=(safety_output){0};
  for(uint32_t t=1;t<=3100;t++) {
   safety_input i=normal(t,o);
   if(t>=2600) {
    if(mode==0)i.hardware_fault_high=false;
    if(mode==1)i.host_rail_ok=false;
    if(mode==2)i.motor_feedback=false;
    if(mode==3)i.internal_ok=false;
    if(mode==4){i.enable=false;i.servo_request=true;}
   }
   o=safety_step(&s,&i);
   if(t>=2604)check(160+mode,!o.motor_permit&&!o.servo_permit);
   if(t>=2604)check(196,mode==4 ? o.sensor_permit : !o.sensor_permit);
  }
 }
 /* A returned stalled main loop must not revive the prior command. */
 safety_init(&s,0);o=(safety_output){0};
 for(uint32_t t=1;t<2600;t++){safety_input i=normal(t,o);o=safety_step(&s,&i);}
 safety_input lag=normal(3000,o);o=safety_step(&s,&lag);
 check(170,!o.motor_permit && (s.last_faults&F_LOOP));
 /* Wrap-safe scheduling with same relative handshake/heartbeat. */
 uint32_t base=0xfffff800u;safety_init(&s,base);o=(safety_output){0};
 for(uint32_t dt=1;dt<=2450;dt++){safety_input i=normal(dt,o);i.now_ms=base+dt;o=safety_step(&s,&i);}
 check(171,o.motor_permit);
 safety_init(&s,0);o=(safety_output){0};
 for(uint32_t t=1;t<=2450;t++){
  safety_input i=normal(t,o);i.heartbeat_event_mode=true;
  /* Deliver EXTI edge one tick after physical level changed; no double count. */
  i.heartbeat_falls=(t%50)==26;o=safety_step(&s,&i);
 }check(175,o.motor_permit && !(s.faults&F_HEARTBEAT));
 /* Sub-poll EN excursion, short hardware-fault event, and excess heartbeat edges. */
 for(unsigned mode=0;mode<3;mode++) {
  safety_init(&s,0);o=(safety_output){0};
  for(uint32_t t=1;t<=2750;t++) {
   safety_input i=normal(t,o);
   if(t==2600){
    if(mode==0)i.enable_edges=2;
    if(mode==1)i.hardware_fault_edge=true;
    if(mode==2)i.heartbeat_falls=2;
   }
   o=safety_step(&s,&i);
   if(t>=2600)check(172+mode,!o.motor_permit&&!o.servo_permit&&!o.sensor_permit);
  }
 }
 /* Repeatable adversarial state/edge sequences. */
 uint32_t rng=0x6817c011u;safety_init(&s,0);o=(safety_output){0};
 for(uint32_t t=1;t<=100000;t++) {
  rng=rng*1664525u+1013904223u;safety_input i=normal(t,o);
  i.enable=(rng&32)!=0;i.servo_request=(rng&128)!=0;
  i.pwm_levels=(uint8_t)((rng>>10)&15);i.pwm_edge_seen=(rng&256)!=0;
  i.hardware_fault_high=(rng&512)!=0;i.host_rail_ok=(rng&1024)!=0;
  o=safety_step(&s,&i);
  check(180,!(o.motor_permit&&o.servo_permit));
  check(183,(!o.sensor_permit)||(i.hardware_fault_high&&i.host_rail_ok&&s.stage!=S_LATCHED));
  check(184,o.sensor_permit==sensor_expected(&s,&i));
  check(181,(i.enable&&i.hardware_fault_high&&i.host_rail_ok) || (!o.motor_permit&&!o.servo_permit));
 }
 /* F-09: disarmed sensor power, heartbeat loss/recovery, WDI unaffected. */
 safety_init(&s,0);o=(safety_output){0};prev=false;last=0;pulses=0;
 for(uint32_t t=1;t<=3000;t++) {
  safety_input i=normal(t,o);i.enable=false;
  if(t>=1000&&t<1500)i.heartbeat=false;
  o=safety_step(&s,&i);
  check(197,!o.motor_permit&&!o.servo_permit&&o.sensor_permit==sensor_expected(&s,&i));
  if(t<75)check(198,!o.sensor_permit);
  if(t>=75&&t<=1055)check(199,o.sensor_permit);
  if(t>=1056&&t<1675)check(200,!o.sensor_permit);
  if(t>=1675)check(201,o.sensor_permit);
  if(prev&&!o.watchdog_level){check(202,t-last<=51);last=t;pulses++;}
  prev=o.watchdog_level;
 }check(203,pulses>=55 && (s.last_faults&F_HEARTBEAT)!=0);
 /* Latched fault holds sensor off while EN is HIGH; EN LOW + 100 ms quiet re-qualifies. */
 for(unsigned mode=0;mode<2;mode++) {
  safety_init(&s,0);o=(safety_output){0};
  for(uint32_t t=1;t<=2000;t++) {
   safety_input i=normal(t,o);i.enable=mode==1 && t>=500 && t<1200;
   if(t==500)i.pwm_edge_seen=true;
   o=safety_step(&s,&i);
   uint32_t back=mode==0?601u:1300u;
   if(t>=500&&t<back)check(204+mode,!o.sensor_permit && s.stage==S_LATCHED);
   if(t>=back)check(206+mode,o.sensor_permit && !o.motor_permit);
  }check(208+mode,(s.last_faults&F_PWM)!=0);
 }
 /* Second adversarial run: xorshift32, rare faults/activity, event heartbeat with
  * rare out-of-window intervals, random EN/request so ACK/ARMED/SERVO are visited. */
 uint32_t x=0x5e4502c0u,next_fall=25,sensor_on=0,sensor_on_en_low=0,armed=0;bool en=false,req=false;
 safety_init(&s,0);o=(safety_output){0};
 for(uint32_t t=1;t<=100000;t++) {
  safety_input i=normal(t,o);
#define RND() (x^=x<<13,x^=x>>17,x^=x<<5,x)
  i.heartbeat_event_mode=true;i.heartbeat_falls=t==next_fall;
  if(t==next_fall)next_fall=t+((RND()&127u)==0u?((RND()&1u)?5u:120u):20u+RND()%61u);
  if((RND()&255u)==0u)en=!en;
  if((RND()&1023u)==0u)req=!req;
  i.enable=en;i.servo_request=req;
  i.pwm_edge_seen=(RND()&2047u)==0u;
  i.pwm_levels=(RND()&4095u)==0u?(uint8_t)(RND()&15u):0u;
  i.hardware_fault_high=(RND()&16383u)!=0u;i.host_rail_ok=(RND()&16383u)!=0u;
  i.hardware_fault_edge=(RND()&32767u)==0u;
#undef RND
  i.motor_feedback=o.motor_permit&&i.enable;i.servo_feedback=o.servo_permit&&i.enable;
  o=safety_step(&s,&i);
  check(180,!(o.motor_permit&&o.servo_permit));
  check(181,(i.enable&&i.hardware_fault_high&&i.host_rail_ok) || (!o.motor_permit&&!o.servo_permit));
  check(183,(!o.sensor_permit)||(i.hardware_fault_high&&i.host_rail_ok&&s.stage!=S_LATCHED));
  check(184,o.sensor_permit==sensor_expected(&s,&i));
  if(o.sensor_permit){sensor_on++;if(!i.enable)sensor_on_en_low++;}
  if(o.motor_permit||o.servo_permit)armed++;
 }check(185,sensor_on>10000u && sensor_on_en_low>1000u && armed>0u);
 cov_sensor_on=sensor_on;cov_sensor_on_en_low=sensor_on_en_low;cov_actuator_on=armed;
 /* ---------------- Sensor strike limiter ---------------- */
 {
  /* S1 power-on SRAM: random slots are never accepted; boot starts clean. */
  uint32_t rejected=0;
  for(uint32_t n=0;n<200000u;n++) {
   sensor_guard a,b;
   a.magic=rnd32();a.rise_age_ms=rnd32();a.strikes=rnd32();a.sensor_high=rnd32();a.check=rnd32();
   b.magic=(n&1u)?SENSOR_GUARD_MAGIC:rnd32();b.rise_age_ms=rnd32()&63u;b.strikes=rnd32()%4u;
   b.sensor_high=rnd32()&1u;b.check=rnd32(); /* plausible fields, random check */
   bool va=sensor_guard_valid(&a),vb=sensor_guard_valid(&b);
   check(300,!va && !vb);if(!va&&!vb)rejected++;
   sensor_guard g=sensor_guard_boot(&a,&b,SR_PIN);
   check(301,sensor_guard_valid(&g) && g.strikes==0u && !sensor_guard_lockout(&g) &&
    g.sensor_high==0u && g.rise_age_ms==SENSOR_AGE_NEVER);
  }
  cov_guard_random_rejected=rejected;
  sensor_guard z0={0},z1;z1.magic=z1.rise_age_ms=z1.strikes=z1.sensor_high=z1.check=0xffffffffu;
  check(302,!sensor_guard_valid(&z0) && !sensor_guard_valid(&z1));
  check(303,sensor_guard_lockout(&z0)); /* a corrupt working copy fails safe (sensor off) */
  /* S2 every single-bit corruption of a sealed record is rejected. */
  sensor_guard base=guard_with(2u,7u,true);
  check(304,sensor_guard_valid(&base) && base.strikes==2u && base.rise_age_ms==7u);
  for(unsigned w=0;w<5u;w++)for(unsigned bit=0;bit<32u;bit++) {
   sensor_guard c=base;uint32_t *word=&c.magic;
   if(w==1u)word=&c.rise_age_ms; else if(w==2u)word=&c.strikes; else if(w==3u)word=&c.sensor_high; else if(w==4u)word=&c.check;
   *word^=1u<<bit;check(305,!sensor_guard_valid(&c));
  }
  /* S3 strike attribution: only a pin reset within the window after a switch-on counts. */
  sensor_guard g,prev;
  for(uint32_t age=0;age<=120u;age++) {
   prev=guard_with(1u,age,true);
   g=sensor_guard_boot(&prev,&prev,SR_PIN);
   check(310,g.strikes==(age<=SENSOR_STRIKE_WINDOW_MS?2u:1u) && g.rise_age_ms==SENSOR_AGE_NEVER && g.sensor_high==0u);
   g=sensor_guard_boot(&prev,&prev,SR_OTHER);check(311,g.strikes==1u); /* IWDG/software: kept, no strike */
   g=sensor_guard_boot(&prev,&prev,SR_POWER);check(312,g.strikes==0u); /* power-on: fresh */
   prev=guard_with(1u,age,false); /* switched on, then off again before the reset */
   g=sensor_guard_boot(&prev,&prev,SR_PIN);check(313,g.strikes==(age<=SENSOR_STRIKE_WINDOW_MS?2u:1u));
  }
  prev=guard_with(1u,SENSOR_AGE_NEVER,false);g=sensor_guard_boot(&prev,&prev,SR_PIN);check(314,g.strikes==1u);
  prev=guard_with(2u,3u,true);g=sensor_guard_boot(&prev,&prev,SR_PIN);check(315,g.strikes==3u && sensor_guard_lockout(&g));
  /* S4 lockout persists through pin/other resets (any age) and saturates; power-on clears it. */
  prev=guard_with(3u,0u,true);
  for(unsigned n=0;n<50u;n++) {
   g=sensor_guard_boot(&prev,&prev,(n&1u)?SR_PIN:SR_OTHER);
   check(316,g.strikes==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&g));
   prev=(n%3u)?g:guard_with(g.strikes,n,true); /* also a (forced) recent switch-on: saturates at the limit */
  }
  g=sensor_guard_boot(&prev,&prev,SR_POWER);check(317,g.strikes==0u && !sensor_guard_lockout(&g));
  /* S5 slot choice: slot A (written first) wins; torn A falls back to B; both bad => fresh. */
  sensor_guard A=guard_with(2u,5u,true),Bs=guard_with(1u,5u,true),Ab=A;
  Ab.strikes=1u; /* torn store: field rewritten, check word not yet */
  g=sensor_guard_boot(&A,&Bs,SR_PIN);check(318,g.strikes==3u);
  g=sensor_guard_boot(&Ab,&Bs,SR_PIN);check(319,g.strikes==2u);
  Bs.check^=1u;g=sensor_guard_boot(&Ab,&Bs,SR_PIN);check(320,g.strikes==0u);
  /* S6 tick: rise edge, age accumulation/saturation, healthy clear at exactly 2 s, lockout never cleared. */
  g=guard_with(2u,SENSOR_AGE_NEVER,false);
  sensor_guard_tick(&g,false,5u,false);check(321,g.rise_age_ms==SENSOR_AGE_NEVER && g.strikes==2u);
  sensor_guard_tick(&g,true,1u,false);check(322,g.rise_age_ms==0u && g.sensor_high==1u && sensor_guard_valid(&g));
  for(uint32_t n=1;n<SENSOR_HEALTHY_MS;n++)sensor_guard_tick(&g,true,1u,false);
  check(323,g.rise_age_ms==SENSOR_HEALTHY_MS-1u && g.strikes==2u);
  sensor_guard_tick(&g,true,1u,false);check(324,g.rise_age_ms==SENSOR_HEALTHY_MS && g.strikes==0u && sensor_guard_valid(&g));
  g=guard_with(2u,0u,true);sensor_guard_tick(&g,true,SENSOR_HEALTHY_MS-1u,false);sensor_guard_tick(&g,false,5u,false);
  sensor_guard_tick(&g,true,1u,false);check(325,g.strikes==2u && g.rise_age_ms==0u); /* interrupted on-time does not clear */
  g=guard_with(1u,0xfffffff0u,false);sensor_guard_tick(&g,false,0x100u,false);check(326,g.rise_age_ms==SENSOR_AGE_NEVER);
  sensor_guard_tick(&g,false,0xffffffffu,false);check(327,g.rise_age_ms==SENSOR_AGE_NEVER && sensor_guard_valid(&g));
  g=guard_with(3u,0u,true);for(unsigned n=0;n<5000u;n++)sensor_guard_tick(&g,true,1u,false);
  check(328,g.strikes==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&g));
  /* S7 the core: lockout forces sensor off in every reachable stage and changes nothing else. */
  safety_state sa,sb;safety_output oa={0},ob={0};uint32_t xs=0x0badc0deu,locked=0,locked_armed=0;
  safety_init(&sa,0);safety_init(&sb,0);bool lk=false;
  for(uint32_t t=1;t<=60000u;t++) {
#define XR() (xs^=xs<<13,xs^=xs>>17,xs^=xs<<5,xs)
   safety_input i=normal(t,oa);
   if((XR()&511u)==0u)lk=!lk;
   if(t>20000u && t<40000u){i.enable=ack_enable(t%5000u,1000u);i.servo_request=(t%5000u)>=3500u && (t%5000u)<3700u;}
   else {i.enable=(XR()&7u)!=0u;i.servo_request=(XR()&4095u)==0u;}
   i.pwm_edge_seen=(XR()&8191u)==0u;i.hardware_fault_high=(XR()&16383u)!=0u;
#undef XR
   i.motor_feedback=oa.motor_permit&&i.enable;i.servo_feedback=oa.servo_permit&&i.enable;
   safety_input j=i;i.sensor_strike_lockout=lk;j.sensor_strike_lockout=false;
   oa=safety_step(&sa,&i);ob=safety_step(&sb,&j);
   check(333,same_actuation(&sa,&sb,oa,ob));
   check(334,lk ? !oa.sensor_permit : oa.sensor_permit==ob.sensor_permit);
   check(335,oa.sensor_permit==sensor_expected(&sa,&i));
   if(lk){locked++;if(oa.motor_permit||oa.servo_permit)locked_armed++;}
  }
  cov_lockout_steps=locked;check(336,locked>10000u && locked_armed>1000u);
  /* S8 device model: sustained short, disarmed. Exactly SENSOR_STRIKE_LIMIT short events per power-on. */
  device d={0};d.t=1000;
  d.slot[0].magic=rnd32();d.slot[1].magic=rnd32(); /* power-on SRAM (not read after PWRRSTF anyway) */
  enum sensor_reset cause=SR_POWER;sensor_guard last={0};unsigned boots=0;
  while(device_run(&d,cause,3000u,8u,false,&last) && boots<20u){cause=SR_PIN;boots++;}
  check(340,d.short_events==SENSOR_STRIKE_LIMIT && boots==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&last));
  check(341,d.max_on_run==9u); /* FAULT 8 ms after the rise: PA12 HIGH on 9 consecutive 1 ms passes */
  uint32_t on_before=d.sensor_on_ms;
  check(342,!device_run(&d,SR_PIN,60000u,8u,false,0) && d.sensor_on_ms==on_before); /* locked, no cycling */
  check(343,!device_run(&d,SR_OTHER,5000u,8u,false,0) && d.sensor_on_ms==on_before);
  /* A complete fresh ACK arms the motors normally but does NOT clear the lockout (chosen policy). */
  uint32_t motor_before=d.motor_on_ms;
  check(344,!device_run(&d,SR_PIN,5000u,8u,true,&last) && d.sensor_on_ms==on_before && d.motor_on_ms>motor_before+4000u);
  check(345,sensor_guard_lockout(&last));
  /* Power cycle (PWRRSTF) with the old record still intact in SRAM: fresh record, sensor retried. */
  d.short_events=0;
  check(346,device_run(&d,SR_POWER,3000u,8u,false,0) && d.short_events==1u);
  /* Fault removed after that retry: next boot (strike 1) powers the sensor; 2 s on clears strikes. */
  on_before=d.sensor_on_ms;
  check(347,!device_run(&d,SR_PIN,3000u,0u,false,&last) && d.sensor_on_ms>on_before+2800u &&
   last.strikes==0u && !sensor_guard_lockout(&last));
  /* S9 intermittent short: two strikes, then >=2 s healthy clears them; repeated pairs never lock. */
  device e={0};e.t=77777u;cause=SR_POWER;
  for(unsigned pair=0;pair<4u;pair++) {
   check(350,device_run(&e,cause,3000u,5u,false,0));cause=SR_PIN;
   check(351,device_run(&e,cause,3000u,5u,false,&last) && last.strikes==1u);
   check(352,!device_run(&e,cause,3000u,0u,false,&last) && last.strikes==0u && !sensor_guard_lockout(&last));
  }
  check(353,e.short_events==8u);
  /* S10 a FAULT later than the window is not attributed (documented limit): no lockout... */
  device f={0};f.t=5000u;cause=SR_POWER;unsigned slow=0;
  while(slow<6u && device_run(&f,cause,3000u,SENSOR_STRIKE_WINDOW_MS+1u,false,&last)){cause=SR_PIN;slow++;}
  check(354,slow==6u && f.short_events==6u && !sensor_guard_lockout(&last));
  /* ...while a FAULT exactly at the window edge is attributed. */
  device h={0};h.t=9000u;cause=SR_POWER;unsigned edge=0;
  while(edge<6u && device_run(&h,cause,3000u,SENSOR_STRIKE_WINDOW_MS,false,&last)){cause=SR_PIN;edge++;}
  check(355,edge==SENSOR_STRIKE_LIMIT && h.short_events==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&last));
  /* S11 armed and driving when the short develops (sensor already on for >50 ms): that first
   * reset is not a strike; the following switch-ons are. Motors match the reference core in every
   * boot (checked inside device_run); 1+SENSOR_STRIKE_LIMIT short events in total, then locked. */
  device m={0};m.t=123u;m.short_from=m.t+2000u;cause=SR_POWER;unsigned mb=0;
  check(356,device_run(&m,cause,4000u,8u,true,&last) && m.motor_on_ms>1300u && last.strikes==0u);
  cause=SR_PIN;
  while(mb<10u && device_run(&m,cause,4000u,8u,true,&last))mb++;
  check(357,mb==SENSOR_STRIKE_LIMIT && m.short_events==1u+SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&last));
  motor_before=m.motor_on_ms;
  check(358,!device_run(&m,SR_PIN,4000u,8u,true,0) && m.motor_on_ms>motor_before+3000u);
  /* ---------------- In-boot strikes: faults that do not reset the MCU ---------------- */
  /* S12 tick with trip: a new F_EXTERNAL/F_RAIL latch within the window after a switch-on is
   * one strike and charges that switch-on (age NEVER), so a pin reset from the same event and
   * later trips without a new switch-on cannot count again. */
  for(uint32_t age=1;age<=120u;age++) {
   g=guard_with(1u,SENSOR_AGE_NEVER,false);sensor_guard_tick(&g,true,1u,false); /* switch-on */
   for(uint32_t n=1;n<age;n++)sensor_guard_tick(&g,true,1u,false);
   sensor_guard_tick(&g,false,1u,true); /* latched on the pass age ms after the switch-on */
   bool hit=age<=SENSOR_STRIKE_WINDOW_MS;
   check(360,sensor_guard_valid(&g) && g.strikes==(hit?2u:1u) && g.sensor_high==0u &&
    g.rise_age_ms==(hit?SENSOR_AGE_NEVER:age));
   prev=g;sensor_guard_tick(&g,false,1u,true); /* a second latch, no new switch-on */
   check(361,g.strikes==(hit?2u:1u));
   g=sensor_guard_boot(&prev,&prev,SR_PIN); /* NRST from the same event after the pass */
   check(362,g.strikes==(hit?2u:1u)); /* hit: not charged twice; miss: age > window */
  }
  g=guard_with(1u,SENSOR_AGE_NEVER,false);sensor_guard_tick(&g,false,1u,true);
  check(363,g.strikes==1u && g.rise_age_ms==SENSOR_AGE_NEVER); /* no switch-on since boot */
  g=guard_with(0u,SENSOR_AGE_NEVER,false);sensor_guard_tick(&g,true,1u,true);
  check(364,g.strikes==0u && g.rise_age_ms==0u && g.sensor_high==1u); /* rise in the trip pass is not blamed */
  g=guard_with(1u,10u,false);sensor_guard_tick(&g,true,1u,true);
  check(365,g.strikes==2u && g.rise_age_ms==0u); /* earlier switch-on charged, new rise starts timing */
  g=guard_with(2u,4u,true);sensor_guard_tick(&g,false,1u,true);
  check(366,g.strikes==3u && sensor_guard_lockout(&g) && sensor_guard_valid(&g));
  g=guard_with(3u,4u,true);sensor_guard_tick(&g,false,1u,true);
  check(367,g.strikes==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&g)); /* saturates */
  g=guard_with(2u,SENSOR_AGE_NEVER,true);for(unsigned n=0;n<3000u;n++)sensor_guard_tick(&g,true,1u,false);
  check(368,g.strikes==2u); /* a charged switch-on never counts as healthy on-time */
  g=guard_with(2u,SENSOR_STRIKE_WINDOW_MS-1u,false);sensor_guard_tick(&g,false,1u,true);
  check(369,g.strikes==3u); /* age accumulates while PA12 is already LOW again */
  /* S13 sensor_guard_trip reads the core exactly: new latch AND (fault input low, fault edge or rail bad). */
  {
   safety_state c;safety_output co={0};uint32_t xt=0x13579bdfu,trips=0,latch_other=0;
   safety_init(&c,0);
   for(uint32_t t=1;t<=60000u;t++) {
#define XT() (xt^=xt<<13,xt^=xt>>17,xt^=xt<<5,xt)
    safety_input i=normal(t,co);i.enable=(t%3000u)>=2100u?ack_enable(t%3000u,2100u):false;
    i.hardware_fault_edge=(XT()&1023u)==0u;i.hardware_fault_high=(XT()&2047u)!=0u;
    i.host_rail_ok=(XT()&2047u)!=0u;i.pwm_edge_seen=(XT()&1023u)==0u;
    if((XT()&4095u)==0u)i.heartbeat=!i.heartbeat;
    i.sensor_strike_lockout=(XT()&1u)!=0u;
#undef XT
    enum safety_stage before=c.stage;co=safety_step(&c,&i);
    bool tr=sensor_guard_trip(before,&c);
    check(370,tr==(before!=S_LATCHED && (!i.hardware_fault_high || i.hardware_fault_edge || !i.host_rail_ok)));
    if(tr)trips++;
    if(before!=S_LATCHED && c.stage==S_LATCHED && !tr)latch_other++; /* F_PWM/F_ACK/F_HEARTBEAT... */
   }
   check(371,trips>50u && latch_other>30u);
   cov_trip_events=trips;
  }
  /* S14 no-reset probe: Pi idle and disarmed (GPIO26 LOW, zero PWM), U30 FAULT caught by EXTI6
   * 7 ms after every switch-on and no MCU reset. Without the limiter: ~560 trips in 60 s. With it: three,
   * then PA12 stays LOW for the rest of the minute; the lock-step reference core shows identical outputs. */
  device q={0};q.t=40000u;
  check(372,!device_run_k(&q,SR_POWER,60000u,7u,DK_EXTI,false,0u,&last));
  check(373,q.short_events==SENSOR_STRIKE_LIMIT && q.rises==SENSOR_STRIKE_LIMIT && q.trips==SENSOR_STRIKE_LIMIT &&
   q.locked_rises==0u && last.strikes==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&last));
  check(374,q.max_on_run==8u && q.sensor_on_ms==8u*SENSOR_STRIKE_LIMIT);
  on_before=q.sensor_on_ms;
  check(375,!device_run_k(&q,SR_PIN,5000u,7u,DK_EXTI,false,0u,0) && q.sensor_on_ms==on_before); /* lockout kept */
  check(376,!device_run_k(&q,SR_OTHER,5000u,7u,DK_EXTI,false,0u,0) && q.sensor_on_ms==on_before);
  q.short_events=0;
  check(377,!device_run_k(&q,SR_POWER,60000u,7u,DK_EXTI,false,0u,&last) && q.short_events==SENSOR_STRIKE_LIMIT &&
   sensor_guard_lockout(&last)); /* power cycle: three more tries, then locked again */
  /* S15 SAFE3 sag on switch-on (F_RAIL on the next sample), no reset: three, then locked. */
  device v={0};v.t=900u;
  check(378,!device_run_k(&v,SR_POWER,30000u,0u,DK_RAIL,false,0u,&last) && v.short_events==SENSOR_STRIKE_LIMIT &&
   v.trips==SENSOR_STRIKE_LIMIT && v.max_on_run==1u && sensor_guard_lockout(&last) && v.locked_rises==0u);
  /* S16 mixed: two in-boot strikes, a pin reset whose switch-on was already charged, then a pin-reset strike. */
  device w={0};w.t=3333u;
  check(379,!device_run_k(&w,SR_POWER,250u,7u,DK_EXTI,false,0u,&last) && w.short_events==2u && last.strikes==2u &&
   !sensor_guard_lockout(&last));
  check(380,device_run_k(&w,SR_PIN,3000u,7u,DK_RESET,false,0u,&last) && last.strikes==2u); /* charged: no boot strike */
  check(381,!device_run_k(&w,SR_PIN,5000u,7u,DK_EXTI,false,0u,&last) && last.strikes==SENSOR_STRIKE_LIMIT &&
   sensor_guard_lockout(&last) && w.short_events==3u && w.locked_rises==0u);
  /* S17 the EXTI-caught FAULT also resets the MCU right after the latching pass: one event, one strike. */
  device x={0};x.t=777u;cause=SR_POWER;unsigned xe=0;
  while(xe<10u && device_run_k(&x,cause,3000u,7u,DK_EXTI_THEN_RESET,false,0u,&last)){cause=SR_PIN;xe++;}
  check(382,xe==SENSOR_STRIKE_LIMIT && x.short_events==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&last));
  /* S18 armed: a Pi that re-ACKs every second. The short develops while the sensor has been on for
   * a long time (first latch not attributed); every ACK start then re-enables it within the window.
   * 1+3 events, then locked; motors keep arming on later ACKs (reference core identical throughout). */
  device y={0};y.t=50000u;y.short_from=y.t+2500u;
  check(383,!device_run_k(&y,SR_POWER,20000u,7u,DK_EXTI,true,1000u,&last));
  check(384,y.short_events==1u+SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&last) && y.locked_rises==0u &&
   y.motor_on_ms>6500u); /* ~400 ms armed per 1 s ACK cycle, less the latched cycles */
  /* S19 window edge in-boot: latched 50 ms after the switch-on counts, 51 ms does not (documented
   * limit: such slower trips retry at the re-qualification rate and are not bounded). */
  device z={0};z.t=100u;
  check(385,!device_run_k(&z,SR_POWER,20000u,SENSOR_STRIKE_WINDOW_MS-1u,DK_EXTI,false,0u,&last) &&
   z.short_events==SENSOR_STRIKE_LIMIT && sensor_guard_lockout(&last));
  device u={0};u.t=100u;
  check(386,!device_run_k(&u,SR_POWER,20000u,SENSOR_STRIKE_WINDOW_MS,DK_EXTI,false,0u,&last) &&
   u.short_events>100u && !sensor_guard_lockout(&last) && last.strikes==0u);
  cov_in_boot_strikes=q.trips+v.trips+w.trips+x.trips+y.trips+z.trips;
  cov_short_events=d.short_events+e.short_events+f.short_events+h.short_events+m.short_events+
   q.short_events+v.short_events+w.short_events+x.short_events+y.short_events+z.short_events+u.short_events;
 }
 test_done=0x600dc0deu;__asm volatile("bkpt #0");for(;;){}
}
