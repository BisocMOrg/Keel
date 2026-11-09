#include "safety_core.h"

/* Fixed policy: no host protocol can alter these constants. */
#define HB_MIN_MS 20u
#define HB_MAX_MS 80u
#define ACK_MS 100u
#define SERVO_MAX_MS 500u
#define SERVO_COOLDOWN_MS 2000u
#define LOOP_MAX_MS 3u
#define WD_FALL_PERIOD_MS 50u

static uint32_t elapsed(uint32_t now, uint32_t then) { return now-then; }
static bool before(uint32_t a,uint32_t b) { return (int32_t)(a-b)<0; }
static void trip(safety_state *s,uint32_t reason,uint32_t now) {
 s->faults|=reason; s->last_faults=s->faults; s->stage=S_LATCHED;
 s->stage_since=now; s->low_timing=false;
 if(s->output.servo_permit) s->servo_not_before=now+SERVO_COOLDOWN_MS;
}
void safety_init(safety_state *s,uint32_t now) {
 *s=(safety_state){0};s->stage=S_DISARMED;s->faults=F_BOOT;
 s->boot_at=now;s->last_step=now;s->wd_last_fall=now;
 s->servo_not_before=now+SERVO_COOLDOWN_MS;
}
safety_output safety_step(safety_state *s,const safety_input *i) {
 const uint32_t now=i->now_ms;
 bool loop_good=true;
 if(s->started && elapsed(now,s->last_step)>LOOP_MAX_MS) {
  trip(s,F_LOOP,now);loop_good=false;
 }
 if(s->started && now==s->last_step) return s->output;
 s->last_step=now;s->started=true;
 if((unsigned)s->stage>(unsigned)S_LATCHED || !i->internal_ok) {
  trip(s,F_INTERNAL,now);loop_good=false;
 }
 const bool falling=i->heartbeat_event_mode ? i->heartbeat_falls>0u :
  ((s->last_hb && !i->heartbeat) || i->heartbeat_falls>0u);
 if(i->heartbeat_falls>1u)trip(s,F_HEARTBEAT,now);
 s->last_hb=i->heartbeat;
 if(falling) {
  if(!s->have_hb) {s->have_hb=true;s->valid_hb_edges=1;}
  else {
   uint32_t interval=elapsed(now,s->last_hb_fall);
   if(interval<HB_MIN_MS || interval>HB_MAX_MS) {
    s->valid_hb_edges=1;trip(s,F_HEARTBEAT,now);
   } else if(s->valid_hb_edges<65535u) s->valid_hb_edges++;
  }
  s->last_hb_fall=now;
 }
 bool hb_good=s->have_hb && s->valid_hb_edges>=2u &&
  elapsed(now,s->last_hb_fall)<=HB_MAX_MS;
 if(!hb_good && (s->stage>=S_WAIT_H1 && s->stage<=S_SERVO))
  trip(s,F_HEARTBEAT,now);
 if(!i->hardware_fault_high || i->hardware_fault_edge) trip(s,F_EXTERNAL,now);
 if(!i->host_rail_ok) trip(s,F_RAIL,now);
 bool levels_zero=i->pwm_levels==0u;
 bool no_activity=levels_zero && !i->pwm_edge_seen;
 const bool request_rise=!s->last_request && i->servo_request;
 s->last_request=i->servo_request;
 bool input_good=hb_good && i->hardware_fault_high && i->host_rail_ok && i->internal_ok && loop_good;
 if(i->enable_edges>1u)trip(s,F_ACK,now);

 /* Raw GPIO26 LOW also hard-disables both external gates. */
 if(!i->enable && (s->stage==S_ARMED || s->stage==S_SERVO)) {
  if(s->stage==S_SERVO) s->servo_not_before=now+SERVO_COOLDOWN_MS;
  s->stage=S_DISARMED;s->low_timing=false;
 }
 if(s->stage==S_DISARMED || s->stage==S_LATCHED) {
  if(input_good && !i->enable && no_activity && !i->servo_request) {
   if(!s->low_timing) {s->low_timing=true;s->low_since=now;}
   if(elapsed(now,s->low_since)>=ACK_MS) {
    s->faults=0;s->stage=S_WAIT_H1;s->stage_since=now;s->low_timing=false;
   }
  } else s->low_timing=false;
 }
 bool ack_stage=s->stage>=S_WAIT_H1 && s->stage<=S_HIGH2;
 if(ack_stage && (!input_good || !no_activity || i->servo_request))
  trip(s,(!no_activity?F_PWM:F_ACK),now);
 switch(s->stage) {
 case S_WAIT_H1:
  if(i->enable) {s->stage=S_HIGH1;s->stage_since=now;} break;
 case S_HIGH1:
  if(elapsed(now,s->stage_since)>=ACK_MS) {
   s->stage=i->enable?S_WAIT_L2:S_LOW2;s->stage_since=now;
  } else if(!i->enable) trip(s,F_ACK,now);
  break;
 case S_WAIT_L2:
  if(!i->enable) {s->stage=S_LOW2;s->stage_since=now;} break;
 case S_LOW2:
  if(elapsed(now,s->stage_since)>=ACK_MS) {
   s->stage=i->enable?S_HIGH2:S_WAIT_H2;s->stage_since=now;
  } else if(i->enable) trip(s,F_ACK,now);
  break;
 case S_WAIT_H2:
  if(i->enable) {s->stage=S_HIGH2;s->stage_since=now;} break;
 case S_HIGH2:
  if(!i->enable) trip(s,F_ACK,now);
  else if(elapsed(now,s->stage_since)>=ACK_MS) s->stage=S_ARMED;
  break;
 case S_ARMED:
  if(request_rise) {
   if(!no_activity) trip(s,F_PWM,now);
   else if(before(now,s->servo_not_before)) trip(s,F_COOLDOWN,now);
   else {s->stage=S_SERVO;s->servo_since=now;}
  } break;
 case S_SERVO:
  if(!no_activity) trip(s,F_PWM,now);
  else if(elapsed(now,s->servo_since)>=SERVO_MAX_MS) trip(s,F_SERVO_TIMEOUT,now);
  else if(!i->servo_request) {
   s->servo_not_before=now+SERVO_COOLDOWN_MS;
   s->stage=S_DISARMED;s->low_timing=false;
  } break;
 default: break;
 }
 safety_output o={0};
 o.motor_permit=s->stage==S_ARMED && input_good && i->enable;
 o.servo_permit=s->stage==S_SERVO && input_good && i->enable;
 /* Sensor: fresh Pi health, no latched fault and no reset-persistent strike
  * lockout; independent of GPIO26/arming. Nothing else reads the lockout. */
 o.sensor_permit=input_good && s->stage!=S_LATCHED && !i->sensor_strike_lockout;
 o.ready=input_good && s->faults==0u;
 if(o.motor_permit && o.servo_permit) {
  trip(s,F_INTERNAL,now);o.motor_permit=o.servo_permit=o.sensor_permit=o.ready=false;loop_good=false;
 }
 bool changed=o.motor_permit!=s->output.motor_permit || o.servo_permit!=s->output.servo_permit;
 if(changed) s->changed_at=now;
 /* Feedback comparison allows two sample periods for ordinary propagation. */
 if(!changed && elapsed(now,s->changed_at)>2u &&
    ((i->motor_feedback!=o.motor_permit) || (i->servo_feedback!=o.servo_permit))) {
  trip(s,F_FEEDBACK,now);o.motor_permit=o.servo_permit=o.sensor_permit=o.ready=false;loop_good=false;
 }
 /* Foreground health only. Safely-off boot/recovery need not wait for the Pi. */
 bool proven_off=!o.motor_permit && !o.servo_permit &&
  !i->motor_feedback && !i->servo_feedback;
 bool may_feed=loop_good && i->internal_ok && i->hardware_fault_high &&
  ((hb_good && i->host_rail_ok) || proven_off);
 if(may_feed) {
  if(!s->watchdog_high && elapsed(now,s->wd_last_fall)>=WD_FALL_PERIOD_MS-1u) {
   s->watchdog_high=true;s->wd_high_since=now;
  } else if(s->watchdog_high && elapsed(now,s->wd_high_since)>=1u) {
   s->watchdog_high=false;s->wd_last_fall=now;
  }
 }
 /* On failed health, leave the line static: never manufacture a falling kick. */
 o.watchdog_level=s->watchdog_high;s->output=o;return o;
}

/* Sensor strike limiter (see safety_core.h). Independent of safety_state. */
static uint32_t rotl7(uint32_t v) { return (v<<7)|(v>>25); }
static uint32_t guard_check(const sensor_guard *g) {
 uint32_t h=rotl7(g->magic^0xa5c3e1f0u)^g->rise_age_ms;
 h=rotl7(h)^g->strikes;h=rotl7(h)^g->sensor_high;
 return ~h;
}
static void guard_seal(sensor_guard *g) { g->magic=SENSOR_GUARD_MAGIC;g->check=guard_check(g); }
bool sensor_guard_valid(const sensor_guard *g) {
 return g->magic==SENSOR_GUARD_MAGIC && g->strikes<=SENSOR_STRIKE_LIMIT &&
  g->sensor_high<=1u && g->check==guard_check(g);
}
sensor_guard sensor_guard_boot(const sensor_guard *a,const sensor_guard *b,enum sensor_reset cause) {
 sensor_guard g={0};const sensor_guard *prev=0;
 if(cause!=SR_POWER) prev=sensor_guard_valid(a)?a:(sensor_guard_valid(b)?b:0);
 if(prev) {
  g.strikes=prev->strikes;
  /* External NRST soon after PA12 rose: attribute it to the sensor switch FAULT. */
  if(cause==SR_PIN && prev->rise_age_ms<=SENSOR_STRIKE_WINDOW_MS &&
     g.strikes<SENSOR_STRIKE_LIMIT) g.strikes++;
 }
 /* Reset released PA12 (10k pulldown): no switch-on yet in this boot. */
 g.rise_age_ms=SENSOR_AGE_NEVER;g.sensor_high=0u;guard_seal(&g);return g;
}
bool sensor_guard_trip(enum safety_stage before,const safety_state *after) {
 /* Outside S_LATCHED, faults holds at most F_BOOT: every trip() also latches. */
 return before!=S_LATCHED && after->stage==S_LATCHED && (after->faults&SENSOR_TRIP_FAULTS)!=0u;
}
void sensor_guard_tick(sensor_guard *g,bool high,uint32_t dt,bool trip) {
 uint32_t age=(g->rise_age_ms>SENSOR_AGE_NEVER-dt)?SENSOR_AGE_NEVER:g->rise_age_ms+dt;
 /* In-boot trip (no reset) soon after a switch-on: one strike, and that switch-on is
  * then charged (age NEVER) so a pin reset from the same event cannot count again. */
 if(trip && age<=SENSOR_STRIKE_WINDOW_MS) {
  if(g->strikes<SENSOR_STRIKE_LIMIT) g->strikes++;
  age=SENSOR_AGE_NEVER;
 }
 if(high && !g->sensor_high) age=0u; /* a rise in this pass is judged by later passes/boots */
 g->rise_age_ms=age;g->sensor_high=high?1u:0u;
 /* Sustained healthy sensor power forgives earlier strikes; a lockout is never forgiven here. */
 if(high && age>=SENSOR_HEALTHY_MS && age!=SENSOR_AGE_NEVER && g->strikes<SENSOR_STRIKE_LIMIT) g->strikes=0u;
 guard_seal(g);
}
bool sensor_guard_lockout(const sensor_guard *g) {
 return !sensor_guard_valid(g) || g->strikes>=SENSOR_STRIKE_LIMIT;
}
