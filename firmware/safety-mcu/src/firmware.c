/* Keel R3 safety MCU (U12, STM32C011F4P6) firmware. The hardware characterisation
 * still open is listed in docs/qualification.md.
 * No debug UART, host configuration protocol, heap, secret, or ISR watchdog feed.
 */
#include "stm32c011xx.h"
#include "safety_core.h"

extern void SystemCoreClockUpdate(void);
extern uint32_t SystemCoreClock;
extern uint8_t __image_end;
__attribute__((section(".safety_crc"),used)) volatile const uint32_t expected_image_crc=0xffffffffu;
volatile uint32_t tick_ms,reset_cause,pwm_activity,fault_edge;
volatile uint32_t enable_edge_count,hb_fall_count;
volatile uint32_t measured_safe_mv,firmware_diagnostic;
/* Read-only bring-up evidence. Successful stages: 1=GPIO/timebase, 2=IWDG,
 * 3=option check, 4=CRC, 5=ADC ready. Bit31 marks the failed stage.
 * This does not participate in permissions or watchdog service decisions. */
volatile uint32_t startup_diagnostic;
static safety_state controller;
/* Sensor strike record: two copies in SRAM that startup never initializes, so a
 * pin/IWDG reset keeps them. slot[0] is written first, then slot[1]; an NRST
 * edge that tears one store leaves the other valid. Never read after PWRRSTF or
 * while the SRAM parity check option is enabled. */
__attribute__((section(".noinit"))) static volatile sensor_guard guard_slot[2];
static sensor_guard guard; /* this boot's working copy */
static uint32_t guard_at;
#define MOTOR_PIN (1u<<11) /* PA11 pin16 MCU_MOTOR_PERMIT_RAW */
#define SERVO_PIN (1u<<7) /* PA7 pin14 MCU_SERVO_PERMIT_RAW */
#define SENSOR_PIN (1u<<12) /* PA12 pin17 SENSOR_POWER_EN */
#define READY_PIN (1u<<4) /* PA4 pin11 MCU_READY */
#define WDI_PIN (1u<<5) /* PA5 pin12 MCU_WD_SERVICE */
#define PWM_EXTI ((1u<<0)|(1u<<1)|(1u<<14)|(1u<<15)) /* PA0 LN, PA1 LP, PC14 RP, PC15 RN */
#define FAULT_EXTI (1u<<6) /* PB6 pin20 SENSE_MOTOR_FAULT */
#define CONTROL_EXTI ((1u<<3)|(1u<<7)) /* PA3 SENSE_MOTOR_EN, PB7 SENSE_HOST_HEARTBEAT */
#define OUTPUT_PINS (MOTOR_PIN|SERVO_PIN|SENSOR_PIN|READY_PIN|WDI_PIN) /* all GPIOA */
/* firmware_diagnostic (SWD-readable): bits 0-10 controller.last_faults, bits 16-17
 * sensor strikes, bit 31 sensor strike lockout. 0x48415244 marks a HardFault. */
#define DIAG_STRIKES_POS 16u
#define DIAG_SENSOR_LOCKOUT (1u<<31)
/* RCC_CSR2 reset flags (names/bits per stm32c011xx.h); RMVF (bit 23) clears them all. */
#define RESET_FLAGS (RCC_CSR2_OBLRSTF|RCC_CSR2_PINRSTF|RCC_CSR2_PWRRSTF|RCC_CSR2_SFTRSTF|\
 RCC_CSR2_IWDGRSTF|RCC_CSR2_WWDGRSTF|RCC_CSR2_LPWRRSTF)

void SysTick_Handler(void) { tick_ms++; } /* timebase only; never feeds WDI/IWDG */
static void exti_events(void) {
 uint32_t rising=EXTI->RPR1&(PWM_EXTI|FAULT_EXTI|CONTROL_EXTI);
 uint32_t falling=EXTI->FPR1&(PWM_EXTI|FAULT_EXTI|CONTROL_EXTI);
 if((rising|falling)&(1u<<3))enable_edge_count+=((rising>>3)&1u)+((falling>>3)&1u); /* PA3 */
 if(falling&(1u<<7))hb_fall_count++; /* PB7 */
 pwm_activity|=(rising|falling)&PWM_EXTI;
 if(falling&FAULT_EXTI) {
  fault_edge=1;
  GPIOA->BSRR=(MOTOR_PIN|SERVO_PIN|SENSOR_PIN|READY_PIN)<<16;
 }
 EXTI->RPR1=rising;EXTI->FPR1=falling;
}
void EXTI0_1_IRQHandler(void){exti_events();}
void EXTI2_3_IRQHandler(void){exti_events();}
void EXTI4_15_IRQHandler(void){exti_events();}
void HardFault_Handler(void) {
 GPIOA->BSRR=OUTPUT_PINS<<16;firmware_diagnostic=0x48415244u;
 for(;;){} /* independent external watchdog resets; no feed */
}
static void pin_mode(GPIO_TypeDef*p,unsigned n,unsigned mode) {
 p->MODER=(p->MODER&~(3u<<(2*n)))|(mode<<(2*n));
 p->PUPDR&=~(3u<<(2*n));
}
static bool wait_clear(volatile uint32_t *reg,uint32_t mask,uint32_t timeout) {
 uint32_t start=tick_ms;
 while(*reg&mask)if((uint32_t)(tick_ms-start)>timeout)return false;
 return true;
}
static bool wait_set(volatile uint32_t *reg,uint32_t mask,uint32_t timeout) {
 uint32_t start=tick_ms;
 while(!(*reg&mask))if((uint32_t)(tick_ms-start)>timeout)return false;
 return true;
}
static uint32_t image_crc(void) {
 const uint8_t*p=(const uint8_t*)0x08000000u;uint32_t crc=0xffffffffu;
 while(p<&__image_end) {
  crc^=*p++;
  for(unsigned j=0;j<8;j++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
 }
 return crc^0xffffffffu;
}
static void gpio_init(void) {
 RCC->IOPENR|=RCC_IOPENR_GPIOAEN|RCC_IOPENR_GPIOBEN|RCC_IOPENR_GPIOCEN;
 (void)RCC->IOPENR;
 GPIOA->BSRR=OUTPUT_PINS<<16;GPIOA->OTYPER&=~OUTPUT_PINS;
 for(unsigned n=0;n<=12;n++)pin_mode(GPIOA,n,0);
 pin_mode(GPIOB,6,0);pin_mode(GPIOB,7,0);pin_mode(GPIOC,14,0);pin_mode(GPIOC,15,0);
 for(unsigned n=0;n<=12;n++)if(OUTPUT_PINS&(1u<<n))pin_mode(GPIOA,n,1);
 /* PA13/PA14 remain SWD. PF2/NRST is never repurposed. */
 EXTI->EXTICR[0]&=~((7u<<0)|(7u<<8)|(7u<<24)); /* PA0/PA1/PA3 */
 EXTI->EXTICR[1]=(EXTI->EXTICR[1]&~((7u<<16)|(7u<<24)))|(1u<<16)|(1u<<24); /* PB6,PB7 (PA6/PA7 not routed) */
 EXTI->EXTICR[3]=(EXTI->EXTICR[3]&~((7u<<16)|(7u<<24)))|(2u<<16)|(2u<<24); /* PC14,PC15 */
 EXTI->RTSR1|=PWM_EXTI|(1u<<3);EXTI->FTSR1|=PWM_EXTI|FAULT_EXTI|CONTROL_EXTI;
 EXTI->RPR1=PWM_EXTI|FAULT_EXTI|CONTROL_EXTI;EXTI->FPR1=PWM_EXTI|FAULT_EXTI|CONTROL_EXTI;
 EXTI->IMR1|=PWM_EXTI|FAULT_EXTI|CONTROL_EXTI;
 NVIC_SetPriority(EXTI0_1_IRQn,0);NVIC_SetPriority(EXTI4_15_IRQn,0);
 NVIC_SetPriority(EXTI2_3_IRQn,0);
 NVIC_EnableIRQ(EXTI0_1_IRQn);NVIC_EnableIRQ(EXTI2_3_IRQn);NVIC_EnableIRQ(EXTI4_15_IRQn);
}
static bool adc_init(void) {
 RCC->APBENR2|=RCC_APBENR2_ADCEN;(void)RCC->APBENR2;
 ADC1->CR|=ADC_CR_ADVREGEN;
 uint32_t t=tick_ms;while((uint32_t)(tick_ms-t)<1u){} /* >20us regulator startup */
 ADC1->CFGR2=ADC_CFGR2_CKMODE_0; /* synchronous PCLK/2 */
 ADC1->SMPR=ADC_SMPR_SMP1; /* longest acquisition, selected channel group1 */
 ADC1->CR|=ADC_CR_ADCAL;if(!wait_clear(&ADC1->CR,ADC_CR_ADCAL,2))return false;
 for(volatile unsigned n=0;n<64;n++){} /* >=4 ADC clocks */
 ADC1_COMMON->CCR|=ADC_CCR_VREFEN;
 t=tick_ms;while((uint32_t)(tick_ms-t)<1u){} /* internal reference settle */
 /* RM0490 requires the channel-configuration handshake before any ADC start.
  * Configure the one fixed channel once; do not restart this handshake in
  * every foreground sample. Clear the W1C flag before selecting the channel. */
 ADC1->ISR=ADC_ISR_CCRDY;
 ADC1->CHSELR=(1u<<10); /* C011 VREFINT internal channel10 */
 if(!wait_set(&ADC1->ISR,ADC_ISR_CCRDY,2))return false;
 ADC1->ISR=ADC_ISR_ADRDY;ADC1->CR|=ADC_CR_ADEN;
 return wait_set(&ADC1->ISR,ADC_ISR_ADRDY,2);
}
static bool sample_safe_supply(void) {
 const uint16_t cal=*(const uint16_t*)0x1fff756au; /* ST factory VREFINT at3000mV */
 if(cal<100u||cal>4095u)return false;
 ADC1->ISR=ADC_ISR_EOC|ADC_ISR_EOS|ADC_ISR_OVR;ADC1->CR|=ADC_CR_ADSTART;
 if(!wait_set(&ADC1->ISR,ADC_ISR_EOC,2))return false;
 uint32_t raw=ADC1->DR;
 if(raw<100u||raw>4095u)return false;
 measured_safe_mv=((uint32_t)cal*3000u)/raw;
 return true;
}
static safety_input snapshot(bool supply_ok,bool internal_ok) {
 safety_input i={0};__disable_irq();
 uint32_t a=GPIOA->IDR,b=GPIOB->IDR,c=GPIOC->IDR;
 i.now_ms=tick_ms;
 i.pwm_edge_seen=(pwm_activity|EXTI->RPR1|EXTI->FPR1)&PWM_EXTI;
 pwm_activity=0;
 i.hardware_fault_edge=fault_edge || (EXTI->FPR1&FAULT_EXTI);
 fault_edge=0; /* do not clear hardware pending here; ISR handles it after unmask */
 i.enable_edges=(uint8_t)(enable_edge_count>2u?2u:enable_edge_count);
 i.heartbeat_falls=(uint8_t)(hb_fall_count>2u?2u:hb_fall_count);
 i.heartbeat_event_mode=true;
 enable_edge_count=0;hb_fall_count=0;
 __enable_irq();
 i.enable=(a&(1u<<3))!=0;i.heartbeat=(b&(1u<<7))!=0;i.servo_request=(a&(1u<<2))!=0; /* PA3,PB7,PA2 */
 i.hardware_fault_high=(b&(1u<<6))!=0; /* PB6 */
 i.pwm_levels=((c>>15)&1u)|(((c>>14)&1u)<<1)|(((a>>0)&1u)<<2)|(((a>>1)&1u)<<3); /* RN PC15,RP PC14,LN PA0,LP PA1 */
 i.motor_feedback=(a&(1u<<8))!=0;i.servo_feedback=(a&(1u<<6))!=0; /* PA8,PA6 */
 i.host_rail_ok=supply_ok; /* core ABI name retained; this revision monitors SAFE rail only */
 i.internal_ok=internal_ok;return i;
}
static sensor_guard guard_load(unsigned n) {
 volatile sensor_guard *v=&guard_slot[n];
 return (sensor_guard){v->magic,v->rise_age_ms,v->strikes,v->sensor_high,v->check};
}
static void guard_store(const sensor_guard *g) {
 for(unsigned n=0;n<2u;n++) {
  volatile sensor_guard *v=&guard_slot[n]; /* check word last: a torn copy fails it */
  v->magic=g->magic;v->rise_age_ms=g->rise_age_ms;v->strikes=g->strikes;
  v->sensor_high=g->sensor_high;v->check=g->check;
 }
}
static void guard_restore(uint32_t csr2) {
 /* Internal resets also pulse NRST and may set PINRSTF, so SR_PIN means PINRSTF alone. */
 enum sensor_reset cause=(csr2&RCC_CSR2_PWRRSTF)?SR_POWER:
  ((csr2&RESET_FLAGS)==RCC_CSR2_PINRSTF)?SR_PIN:SR_OTHER;
 /* Option bit RAM_PARITY_CHECK=1 means SRAM parity checking is disabled (the required
  * setting). If it is enabled, a slot not written since power-up may read as a parity
  * error, so the record is not read and the boot is treated as a power-on. */
 if(!(FLASH->OPTR&FLASH_OPTR_RAM_PARITY_CHECK)) cause=SR_POWER;
 sensor_guard a={0},b={0};
 if(cause!=SR_POWER){a=guard_load(0u);b=guard_load(1u);} /* no uninitialized-SRAM read after power-on */
 guard=sensor_guard_boot(&a,&b,cause);guard_store(&guard);
}
static void apply(safety_output o) {
 __disable_irq();
 if(fault_edge || !(GPIOB->IDR&(1u<<6)))o.motor_permit=o.servo_permit=o.sensor_permit=o.ready=false; /* PB6 fault bus */
 uint32_t set=(o.motor_permit?MOTOR_PIN:0)|(o.servo_permit?SERVO_PIN:0)|
  (o.sensor_permit?SENSOR_PIN:0)|(o.ready?READY_PIN:0)|(o.watchdog_level?WDI_PIN:0);
 /* One atomic BSRR write prevents sequential motor/servo transition overlap. */
 GPIOA->BSRR=((OUTPUT_PINS&~set)<<16)|set;
 __enable_irq();
}
int main(void) {
 SystemCoreClockUpdate();
 gpio_init();reset_cause=RCC->CSR2;
 guard_restore(reset_cause);
 /* Clear only after this boot's record is stored: a reset before this point
  * still reports PWRRSTF/other flags and cannot be mistaken for a pin reset. */
 RCC->CSR2|=RCC_CSR2_RMVF;
 if(SysTick_Config(SystemCoreClock/1000u))for(;;){}
 NVIC_SetPriority(SysTick_IRQn,1);
 startup_diagnostic=1u;
 DBG->APBFZ1&=~DBG_APB_FZ1_DBG_IWDG_STOP;
 /* Timeout 4*(2047+1) = 8192 LSI ticks. Register synchronization takes up to
  * five prescaled LSI periods: /4 keeps it below 1 ms at the nominal 32 kHz,
  * where /256 would need about 40 ms and exceed the 10 ms startup bound. The
  * first external-watchdog service deadline is unaffected. */
 IWDG->KR=0xccccu;IWDG->KR=0x5555u;IWDG->PR=0u;IWDG->RLR=2047u;
 bool internal=wait_clear(&IWDG->SR,7u,10);
 if(internal) {
  IWDG->KR=0xaaaau; /* load the confirmed bound once at successful startup */
  startup_diagnostic=2u;
 } else startup_diagnostic=0x80000002u;
 if(internal) {
  internal=(FLASH->OPTR&FLASH_OPTR_NRST_MODE)==FLASH_OPTR_NRST_MODE;
  startup_diagnostic=internal?3u:0x80000003u;
 }
 if(internal) {
  internal=image_crc()==expected_image_crc;
  startup_diagnostic=internal?4u:0x80000004u;
 }
 if(internal) {
  internal=adc_init();
  startup_diagnostic=internal?5u:0x80000005u;
 }
 safety_init(&controller,tick_ms);uint32_t last=tick_ms;bool old_wdi=false;
 bool wd_kicked=false; /* this boot has driven a WDI falling edge (external watchdog kick) */
 guard_at=last;
 for(;;) {
  if(tick_ms==last){__WFI();continue;}
  last=tick_ms;
  bool adc_ok=internal && sample_safe_supply();
  bool supply_ok=adc_ok && measured_safe_mv>=3100u && measured_safe_mv<=3500u;
  safety_input i=snapshot(supply_ok,internal&&adc_ok);
  i.sensor_strike_lockout=sensor_guard_lockout(&guard); /* re-read every pass */
  const enum safety_stage stage_before=controller.stage;
  safety_output o=safety_step(&controller,&i);
  /* The core already applies the lockout. PA12 also waits for this boot's first WDI
   * kick, so an external-watchdog reset left over from before this boot cannot
   * land inside the strike window of a switch-on. */
  o.sensor_permit=o.sensor_permit && !i.sensor_strike_lockout && wd_kicked;
  /* Record PA12 and any new F_EXTERNAL/F_RAIL latch before driving PA12 (apply can
   * only lower it): a reset caused by this switch-on always finds it in the record. */
  sensor_guard_tick(&guard,o.sensor_permit,i.now_ms-guard_at,
   sensor_guard_trip(stage_before,&controller));
  guard_at=i.now_ms;
  guard_store(&guard);
  apply(o);
  firmware_diagnostic=controller.last_faults|((guard.strikes&3u)<<DIAG_STRIKES_POS)|
   (sensor_guard_lockout(&guard)?DIAG_SENSOR_LOCKOUT:0u);
  if(old_wdi&&!o.watchdog_level) {
   wd_kicked=true;
   if(i.internal_ok)IWDG->KR=0xaaaau;
  }
  old_wdi=o.watchdog_level;
 }
}
