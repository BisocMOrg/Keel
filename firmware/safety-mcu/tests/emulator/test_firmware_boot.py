"""Boot-path and main-loop check of the built image build/safety_controller.elf (run tools/build_firmware.py first).

Unicorn executes the real Reset_Handler/startup/main. Peripheral registers are
plain memory, with a small Python model behind the ones firmware.c polls:
RCC_CSR2 reset flags (RMVF clears them), FLASH_OPTR (NRST_MODE, RAM_PARITY_CHECK),
GPIO IDR/BSRR, EXTI write-1-to-clear pending bits, ADC CAL/READY/EOC and data,
IWDG SR. The .noinit strike-record slots are preloaded to model SRAM kept
through a reset.
Boot cases stop at the RMVF write that clears the reset flags and check RCC_CSR2
decoding (PINRSTF alone = pin reset; PWRRSTF = power-on; other flags = keep),
the SRAM-parity option guard, the resulting strike count, no slot read after
PWRRSTF or with parity checking enabled, startup not touching the slots, and
both slots written before RMVF. One boot case tears the boot's own record store
after each of its word writes (a second NRST, PINRSTF still set) and checks that
the next boot still counts the switch-on once.
Main-loop cases continue into the foreground loop. Time advances only at the
WFI instruction: there the harness updates the modelled inputs, runs the real
EXTI handlers for heartbeat/GPIO26/PWM/fault edges and SysTick_Handler, then
resumes. After every EXTI6 fault edge the handler itself must already have
driven PA12, READY, motor and servo LOW. Every pass is checked for: both slots
rewritten (slot 0 then slot 1, equal and valid), then exactly one BSRR write
whose PA12 bit equals the record, so a PA12 rise is always recorded
(sensor_high=1, rise_age=0) before the BSRR write that drives it. The only
exception is a pass whose FAULT edge is injected between snapshot() and apply()
(at the safety_step call): there the record may say 1 but PA12 must stay LOW.
Scenarios: locked record (PA12 never set; READY, motor arming after an ACK and a
servo burst unaffected), unlocked record (PA12 after two heartbeat falls and the
first WDI kick), a U30 FAULT caught by EXTI6 or a SAFE3 sag without reset (three
strikes, then lockout), a FAULT edge between snapshot() and apply(), F_HEARTBEAT
and F_PWM latches soon after a switch-on (no strike), a fault bus LOW read only by
apply() (edge arriving while its interrupts are masked: that pass's output write must
already drop PA12, READY, motor and servo), a FAULT 51 ms after each
switch-on (the documented uncounted limit), and an in-boot strike followed by a
pin reset of the same event, including one that tears the strike pass's record
store (counted once).
U12 pin map (hardware/kicad/Keel.kicad_sch, part U12; table PIN_MAP below): the EXTI
pending bits above are injected directly, so two cases check the wiring itself. A boot
case starts GPIO/EXTI registers from their reset values and checks, at the first
main-loop WFI, every U12 pin's mode, output type and pull, each edge input alone on
its EXTI line with EXTICR routed to its port, the edges and mask, no other line
unmasked, the EXTI IRQ groups enabled, SWD/GPIOF untouched and outputs LOW. A
main-loop case drives each edge line and each level input from the first ACK stage
and checks that the core latches the matching fault (GPIO26 double edge F_ACK, a
PWM edge or level on each of PA0/PA1/PC14/PC15 F_PWM, fault bus LOW on PB6
F_EXTERNAL) with READY dropped, and that nothing latches without a stimulus.
This runner builds nothing; --elf names another built image to check.
Results go to build/firmware_boot_tests.json (next to the image given with --elf),
or to the file named by --out.
This is instruction emulation with memory-backed registers, NOT a peripheral or
silicon simulation. It does not rebuild the image; MCU_RUNTIME may hold
unicorn/pyelftools.
"""
from pathlib import Path
import sys,os,json,hashlib,struct,argparse
sys.dont_write_bytecode=True
P=Path(__file__).resolve().parents[2];B=P/'build' # firmware/safety-mcu/build
if os.environ.get('MCU_RUNTIME'):sys.path.insert(0,os.environ['MCU_RUNTIME'])
import unicorn
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS,UC_HOOK_INTR,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE,UC_HOOK_CODE
from unicorn import arm_const as A
from elftools.elf.elffile import ELFFile
ap=argparse.ArgumentParser()
ap.add_argument('--elf',default=str(B/'safety_controller.elf'))
ap.add_argument('--out')
args=ap.parse_args() if __name__=="__main__" else ap.parse_args([])
PACKAGE_ELF=Path(args.elf).resolve()==(B/'safety_controller.elf').resolve()
# Register addresses and bits as in the retained ST stm32c011xx.h
RCC_CSR2=0x40021000+0x60;FLASH_OPTR=0x40022000+0x20
EXTI_RPR1=0x40021800+0x0C;EXTI_FPR1=0x40021800+0x10
ADC_ISR=0x40012400;ADC_CR=0x40012408;ADC_CHSELR=0x40012428;ADC_DR=0x40012440;IWDG_KR=0x40003000
IWDG_PR=0x40003004;IWDG_RLR=0x40003008;IWDG_SR=0x4000300c
GPIOA_IDR=0x50000010;GPIOA_BSRR=0x50000018;GPIOB_IDR=0x50000410;GPIOC_IDR=0x50000810
LSION=1<<0;RMVF=1<<23;OBLRSTF=1<<25;PINRSTF=1<<26;PWRRSTF=1<<27;SFTRSTF=1<<28;IWDGRSTF=1<<29;WWDGRSTF=1<<30;LPWRRSTF=1<<31
RAM_PARITY_CHECK=1<<22;NRST_MODE=3<<27
OPTR_OK=NRST_MODE|RAM_PARITY_CHECK|0xAA   # parity checking disabled (bit=1), bidirectional NRST, RDP level 0
OPTR_PARITY_ON=OPTR_OK&~RAM_PARITY_CHECK
# U12 outputs (all GPIOA): PA4 READY, PA5 WDI, PA7 servo permit, PA11 motor permit, PA12 sensor enable
PA4,PA5,PA7,PA11,PA12=1<<4,1<<5,1<<7,1<<11,1<<12
# U12 pin map (hardware/kicad/Keel.kicad_sch, part U12): (pin, port, bit, net, kind). kind: out = push-pull
# output; level = input without EXTI; fall/both = input alone on EXTI line <bit>, falling / both edges.
PIN_MAP=[(1,'B',7,'SENSE_HOST_HEARTBEAT','fall'),(2,'C',14,'SENSE_MOTOR_RP','both'),(3,'C',15,'SENSE_MOTOR_RN','both'),
 (7,'A',0,'SENSE_MOTOR_LN','both'),(8,'A',1,'SENSE_MOTOR_LP','both'),(9,'A',2,'SENSE_SERVO_REQUEST','level'),
 (10,'A',3,'SENSE_MOTOR_EN','both'),(11,'A',4,'MCU_READY','out'),(12,'A',5,'MCU_WD_SERVICE','out'),
 (13,'A',6,'SERVO_POWER_EN_FB','level'),(14,'A',7,'MCU_SERVO_PERMIT_RAW','out'),(15,'A',8,'MOTOR_EN_SAFE_FB','level'),
 (16,'A',11,'MCU_MOTOR_PERMIT_RAW','out'),(17,'A',12,'SENSOR_POWER_EN','out'),(20,'B',6,'SENSE_MOTOR_FAULT','fall')]
GPIO_BASE={'A':0x50000000,'B':0x50000400,'C':0x50000800,'F':0x50001400}
GPIO_RESET={'A':(0xEBFFFFFF,0x24000000),'B':(0xFFFFFFFF,0),'C':(0xFFFFFFFF,0),'F':(0xFFFFFFFF,0)} # MODER, PUPDR reset values (RM0490)
EXTI_BASE=0x40021800;EXTI_PORT={'A':0,'B':1,'C':2,'F':5};NVIC_ISER=0xE000E100
def exti_irq(line):return 5 if line<2 else 6 if line<4 else 7 # EXTI0_1 / EXTI2_3 / EXTI4_15 IRQ number
F_HEARTBEAT,F_EXTERNAL,F_PWM,F_RAIL,F_INTERNAL=2,4,16,128,512
DIAG_LOCKOUT=1<<31
MAGIC=0x53e7c011;NEVER=0xffffffff;WINDOW=50
RET=0x0800ff00 # return address for handler calls (outside the image)
def rotl7(v):return ((v<<7)|(v>>25))&0xffffffff
def seal(rise,strikes,high):
 h=rotl7(MAGIC^0xa5c3e1f0)^rise;h=rotl7(h)^strikes;h=rotl7(h)^high
 return [MAGIC,rise,strikes,high,(~h)&0xffffffff]
def valid(w):return len(w)==5 and w==seal(w[1],w[2],w[3]) and w[2]<=3 and w[3]<=1

class Image:
 def __init__(self,path):
  self.path=Path(path)
  with self.path.open('rb') as f:
   e=ELFFile(f);tab=e.get_section_by_name('.symtab')
   self.syms={s.name:s['st_value'] for s in tab.iter_symbols()}
   sizes={s.name:s['st_size'] for s in tab.iter_symbols()}
   self.segs=[(s['p_paddr'],s.data()) for s in e.iter_segments() if s['p_type']=='PT_LOAD' and s['p_filesz']]
   ni=e.get_section_by_name('.noinit');self.ni_addr,self.ni_size=ni['sh_addr'],ni['sh_size']
  self.slot=self.syms['guard_slot']
  assert self.slot==self.ni_addr and self.ni_size==40,(hex(self.slot),hex(self.ni_addr),self.ni_size)
  assert self.slot>=self.syms['_ebss'],'slots must lie outside the zeroed .bss'
  main=self.syms['main']&~1;code=b''.join(d[main-a:main-a+sizes['main']] for a,d in self.segs if a<=main<a+len(d))
  wfi=[main+k for k in range(0,len(code)-1,2) if code[k:k+2]==b'\x30\xbf']
  assert len(wfi)==1,'expected one WFI in main: %s'%wfi
  self.wfi=wfi[0];self.step_fn=self.syms['safety_step']&~1 # called once per pass, after snapshot(), before apply()
  self.sha=hashlib.sha256(self.path.read_bytes()).hexdigest()

class Mcu:
 """One MCU boot: built image, memory-backed registers and a small input model."""
 def __init__(self,img,csr2,slot0,slot1,optr=OPTR_OK):
  self.img=img;s=img.syms;u=self.u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
  for base,size in [(0x08000000,0x10000),(0x1fff0000,0x10000),(0x20000000,0x10000),(0x40000000,0x30000),(0x50000000,0x2000),(0xe0000000,0x100000)]:u.mem_map(base,size)
  for a,d in img.segs:u.mem_write(a,d)
  u.mem_write(0x1fff756a,struct.pack('<H',1600)) # VREFINT calibration word
  u.mem_write(img.slot,struct.pack('<10I',*slot0,*slot1))
  self.csr2=csr2;self.optr=optr;self.rpr=0;self.fpr=0;self.odr=0;self.adc_raw=1455 # 1600*3000/1455 = 3298 mV
  self.idr_a=0;self.idr_b=1<<6;self.idr_c=0;self.log=[];self.phase='boot';self.pass_no=0
  # IWDG and ADC startup model. IWDG status sync uses five prescaled LSI
  # periods; the model rounds to the next millisecond.
  self.iwdg_pr=0;self.iwdg_rlr=4095;self.iwdg_ready_at=0;self.iwdg_stuck=False
  self.iwdg_reloads=[];self.lsi_hz=32000
  self.adc_ready_at=None;self.adc_never_ready=False;self.adc_conversion=False
  self.adc_ignored_starts=0;self.adc_channel_writes=0

  self.in_loop=False;self.tick_reads=0;self.stop_at_rmvf=False;self.at_wfi=False;self.rmvf=False
  self.isr_errors=[];self.midpass_passes=set();self.midpass_armed=False;self.midpass_stop=False
  self.apply_fault_pass=None;self.idr_b_reads=0 # pass whose second GPIOB IDR read (apply()) sees PB6 LOW
  slot=img.slot;tick=s['tick_ms']
  def w32(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
  def r32(a):return struct.unpack('<I',bytes(u.mem_read(a,4)))[0]
  self.w32,self.r32=w32,r32
  def on_read(uc,acc,addr,size,val,ud):
   if slot<=addr<slot+40:self.log.append(('R',addr-slot,self.phase,self.pass_no))
   elif addr==RCC_CSR2:w32(addr,self.csr2)
   elif addr==FLASH_OPTR:w32(addr,self.optr);self.log.append(('OPTR',self.optr,self.phase,self.pass_no))
   elif addr==GPIOA_IDR:w32(addr,self.idr_a)
   elif addr==GPIOB_IDR:
    self.idr_b_reads+=1
    w32(addr,self.idr_b&~(1<<6) if self.apply_fault_pass==self.pass_no and self.idr_b_reads>=2 else self.idr_b)
   elif addr==GPIOC_IDR:w32(addr,self.idr_c)
   elif addr==EXTI_RPR1:w32(addr,self.rpr)
   elif addr==EXTI_FPR1:w32(addr,self.fpr)
   elif addr==ADC_CR:w32(addr,r32(addr)&0x7fffffff) # calibration completes at once
   elif addr==IWDG_SR:w32(addr,3 if self.iwdg_stuck or r32(tick)<self.iwdg_ready_at else 0)
   elif addr==ADC_ISR:
    configured=self.adc_ready_at is not None and r32(tick)>=self.adc_ready_at and not self.adc_never_ready
    w32(addr,(1 if r32(ADC_CR)&1 else 0)|(0x2000 if configured else 0)|(0xC if self.adc_conversion else 0))
   elif addr==ADC_DR:w32(addr,self.adc_raw)
   elif addr==tick and not self.in_loop:
    self.tick_reads+=1
    if self.tick_reads%16==0:w32(tick,r32(tick)+1) # time passes in start-up busy waits
  def on_write(uc,acc,addr,size,val,ud):
   val&=0xffffffff
   if slot<=addr<slot+40:self.log.append(('W',addr-slot,val,self.phase,self.pass_no))
   elif addr==GPIOA_BSRR:
    self.log.append(('BSRR',val,self.phase,self.pass_no))
    self.odr=(self.odr&~(val>>16))|(val&0xffff)
   elif addr==RCC_CSR2:
    self.log.append(('CSR2',val,self.phase,self.pass_no))
    self.csr2=(val&0x007fffff)|(0 if val&RMVF else self.csr2&0xfe000000)
    if val&RMVF:
     self.rmvf=True
     if self.stop_at_rmvf:uc.emu_stop()
   elif addr==EXTI_RPR1:self.rpr&=~val
   elif addr==EXTI_FPR1:self.fpr&=~val
   elif addr in (IWDG_PR,IWDG_RLR):
    if addr==IWDG_PR:self.iwdg_pr=val&7
    else:self.iwdg_rlr=val&4095
    divider=4<<min(self.iwdg_pr,6)
    self.iwdg_ready_at=r32(tick)+(5*divider*1000+self.lsi_hz-1)//self.lsi_hz
   elif addr==IWDG_KR:
    self.log.append(('KR',val,self.phase,self.pass_no))
    if val==0xaaaa:self.iwdg_reloads.append((r32(tick),(4<<min(self.iwdg_pr,6))*(self.iwdg_rlr+1)))
   elif addr==ADC_CHSELR:
    self.adc_ready_at=r32(tick)+1;self.adc_channel_writes+=1
   elif addr==ADC_ISR:
    if val&0x2000:self.adc_ready_at=None
    if val&0xC:self.adc_conversion=False
   elif addr==ADC_CR and val&4:
    self.adc_conversion=self.adc_ready_at is not None and r32(tick)>=self.adc_ready_at and not self.adc_never_ready
    if not self.adc_conversion:self.adc_ignored_starts+=1
  for lo,hi in [(slot,slot+39),(tick,tick+3),(RCC_CSR2,RCC_CSR2+3),(FLASH_OPTR,FLASH_OPTR+3),(EXTI_RPR1,EXTI_FPR1+3),
                (ADC_ISR,ADC_DR+3),(IWDG_SR,IWDG_SR+3),(GPIOA_IDR,GPIOA_IDR+3),(GPIOB_IDR,GPIOB_IDR+3),(GPIOC_IDR,GPIOC_IDR+3)]:
   u.hook_add(UC_HOOK_MEM_READ,on_read,begin=lo,end=hi)
  for lo,hi in [(slot,slot+39),(RCC_CSR2,RCC_CSR2+3),(EXTI_RPR1,EXTI_FPR1+3),(GPIOA_BSRR,GPIOA_BSRR+3),(IWDG_KR,IWDG_RLR+3),(ADC_ISR,ADC_CHSELR+3)]:
   u.hook_add(UC_HOOK_MEM_WRITE,on_write,begin=lo,end=hi)
  def on_wfi(uc,addr,size,ud):self.at_wfi=True;uc.emu_stop()
  u.hook_add(UC_HOOK_CODE,on_wfi,begin=img.wfi,end=img.wfi)
  def on_step_fn(uc,addr,size,ud): # stops before the hooked instruction executes
   if self.midpass_armed:self.midpass_armed=False;self.midpass_stop=True;uc.emu_stop()
  u.hook_add(UC_HOOK_CODE,on_step_fn,begin=img.step_fn,end=img.step_fn)
  u.hook_add(UC_HOOK_INTR,lambda uc,n,d:uc.emu_stop())
  u.reg_write(A.UC_ARM_REG_SP,0x20001800);u.reg_write(A.UC_ARM_REG_XPSR,0x01000000)
 def slots(self):
  w=struct.unpack('<10I',bytes(self.u.mem_read(self.img.slot,40)));return list(w[:5]),list(w[5:])
 def var(self,name):return self.r32(self.img.syms[name])
 def boot_to_rmvf(self):
  self.stop_at_rmvf=True
  self.u.emu_start(self.img.syms['Reset_Handler']|1,0,count=5_000_000)
  return self.rmvf
 def start(self):
  """Reset to the first WFI of the foreground loop."""
  self.u.emu_start(self.img.syms['Reset_Handler']|1,0,count=50_000_000)
  if not self.at_wfi:raise AssertionError('did not reach the main-loop WFI')
  self.in_loop=True;self.phase='loop';self.t=0
 def call(self,name):
  """Run an interrupt handler as the NVIC would at the WFI (context saved/restored)."""
  u=self.u;regs=[A.UC_ARM_REG_R0+k for k in range(13)]+[A.UC_ARM_REG_SP,A.UC_ARM_REG_LR,A.UC_ARM_REG_PC,A.UC_ARM_REG_XPSR]
  saved=[u.reg_read(r) for r in regs];phase=self.phase;self.phase='isr'
  u.reg_write(A.UC_ARM_REG_SP,(saved[13]-0x40)&~7);u.reg_write(A.UC_ARM_REG_LR,RET|1);u.reg_write(A.UC_ARM_REG_XPSR,0x01000000)
  u.emu_start(self.img.syms[name]|1,RET,count=200000)
  for r,v in zip(regs,saved):u.reg_write(r,v)
  self.phase=phase
 def fault_isr(self):
  """EXTI6 FAULT edge: the real handler must itself drive PA12, READY, motor and servo LOW."""
  self.call('EXTI4_15_IRQHandler')
  if self.odr&(PA12|PA4|PA11|PA7):self.isr_errors.append('t=%d: EXTI6 handler left outputs %#x HIGH'%(self.t,self.odr&(PA12|PA4|PA11|PA7)))
 def step(self,world):
  """At WFI: advance 1 ms (inputs, EXTI handlers, SysTick), then run the next pass to WFI.
  If the world asks for it, a FAULT edge is delivered inside the pass, at the safety_step call
  (snapshot() already taken, apply() not yet run)."""
  self.t+=1;events=world.inputs(self,self.t)
  if events&((1<<0)|(1<<1)):self.call('EXTI0_1_IRQHandler') # PA0/PA1 PWM
  if events&((1<<2)|(1<<3)):self.call('EXTI2_3_IRQHandler') # PA3 GPIO26 enable
  if events&((1<<7)|(1<<14)|(1<<15)):self.call('EXTI4_15_IRQHandler') # PB7 heartbeat, PC14/PC15 PWM
  if events&(1<<6):self.fault_isr() # PB6 fault
  self.call('SysTick_Handler')
  self.pass_no+=1;self.at_wfi=False;before=self.odr;self.idr_b_reads=0
  self.midpass_armed=world.midpass_due(self,self.t);self.midpass_stop=False
  self.u.emu_start((self.img.wfi+2)|1,0,count=2_000_000)
  if self.midpass_stop:
   self.midpass_stop=False;pc=self.u.reg_read(A.UC_ARM_REG_PC)
   world.midpass(self,self.t);self.midpass_passes.add(self.pass_no);self.fault_isr()
   self.u.emu_start(pc|1,0,count=2_000_000)
  self.midpass_armed=False
  if not self.at_wfi:raise AssertionError('pass %d did not return to WFI'%self.pass_no)
  world.after_pass(self,self.t,before,self.odr)

class World:
 """Pi and board model. hb: heartbeat falling-edge period/phase (ms). en(t): GPIO26.
 srv(t): servo request (PA2). fault: None, 'u30' (FAULT caught by EXTI6 fault_ms after
 each PA12 rise, no reset; with midpass=True the edge arrives inside that pass, between
 snapshot() and apply()) or 'sag' (SAFE3 reads 3.0 V on the pass after each PA12 rise).
 rise_events: one (kind, delay_ms) per PA12 rise, in order: 'hb' = an extra heartbeat
 fall (too early: F_HEARTBEAT), 'pwm' = a motor PWM edge on PA1 (F_PWM in an ACK stage)."""
 def __init__(self,hb_period=50,hb_phase=10,en=lambda t:False,srv=lambda t:False,fault=None,fault_ms=7,
              midpass=False,rise_events=()):
  self.hb_period=hb_period;self.hb_phase=hb_phase;self.en=en;self.srv=srv;self.fault=fault;self.fault_ms=fault_ms
  self.fault_midpass=midpass;self.rise_events=list(rise_events);self.sched={};self.fired=[];self.diag_at={}
  self.last_en=False;self.rises=[];self.falls=[];self.fault_at=None;self.faults=[];self.sag_at=None;self.kicks=[]
  self.odr_log=[]
 def inputs(self,m,t):
  ev=0;en=bool(self.en(t))
  if en!=self.last_en:
   if en:m.rpr|=1<<3
   else:m.fpr|=1<<3
   ev|=1<<3;self.last_en=en # PA3 = EXTI3
  hb_level=(t%self.hb_period)==(self.hb_phase-1)%self.hb_period
  if t%self.hb_period==self.hb_phase%self.hb_period:m.fpr|=1<<7;ev|=1<<7;self.falls.append(t) # PB7 = EXTI7
  kind=self.sched.pop(t,None)
  if kind=='hb':m.fpr|=1<<7;ev|=1<<7;self.fired.append(t)
  elif kind=='pwm':m.rpr|=1<<1;m.fpr|=1<<1;ev|=1<<1;self.fired.append(t) # short pulse: PA1 back LOW by the pass
  if self.fault=='u30' and not self.fault_midpass and self.fault_at==t and m.odr&PA12:
   m.fpr|=1<<6;ev|=1<<6;self.faults.append(t) # short FAULT pulse: EXTI6 latched, bus back HIGH before the next pass
  m.adc_raw=1600 if (self.fault=='sag' and self.sag_at==t) else 1455
  if self.fault=='sag' and self.sag_at==t:self.faults.append(t)
  fb_ok=en and bool(m.idr_b&(1<<6)) # PB6 fault bus
  # PA3 enable, PA2 servo request, PA8 MOTOR_EN_SAFE, PA6 SERVO_POWER_EN; PB6 fault bus HIGH, PB7 heartbeat
  m.idr_a=(int(en)<<3)|(int(bool(self.srv(t)))<<2)|(int(fb_ok and bool(m.odr&PA11))<<8)|(int(fb_ok and bool(m.odr&PA7))<<6)
  m.idr_b=(1<<6)|(int(hb_level)<<7);m.idr_c=0
  return ev
 def midpass_due(self,m,t):return self.fault=='u30' and self.fault_midpass and self.fault_at==t and bool(m.odr&PA12)
 def midpass(self,m,t):m.fpr|=1<<6;self.faults.append(t) # same short FAULT pulse, arriving inside the pass
 def after_pass(self,m,t,before,after):
  self.odr_log.append((t,after))
  if t in self.fired:self.diag_at[t]=m.var('firmware_diagnostic')
  if after&PA12 and not before&PA12:
   self.rises.append(t);self.fault_at=t+self.fault_ms;self.sag_at=t+1
   if self.rise_events:kind,delay=self.rise_events.pop(0);self.sched[t+delay]=kind
  if before&PA5 and not after&PA5:self.kicks.append(t)

def passes(m):
 """Group loop-phase log entries by pass number."""
 out={}
 for e in m.log:
  if e[-2]=='loop':out.setdefault(e[-1],[]).append(e)
 return out
def pass_invariants(m):
 """Per pass: slot0 then slot1 fully rewritten (valid, equal), then one BSRR write whose PA12
 bit equals the stored sensor_high (on a pass with a FAULT edge injected before apply(), PA12
 must be LOW whatever the record says). Returns (errors, number of passes checked)."""
 err=[];n=0
 for k,ev in sorted(passes(m).items()):
  if k==0:continue
  n+=1;ws=[e for e in ev if e[0]=='W'];bs=[j for j,e in enumerate(ev) if e[0]=='BSRR']
  if [e[1] for e in ws]!=list(range(0,40,4)):err.append('pass %d slot writes %s'%(k,[e[1] for e in ws]));continue
  s0=[e[2] for e in ws[:5]];s1=[e[2] for e in ws[5:]]
  if s0!=s1 or not valid(s0):err.append('pass %d slots %s %s'%(k,s0,s1));continue
  if len(bs)!=1:err.append('pass %d has %d BSRR writes'%(k,len(bs)));continue
  last_w=max(j for j,e in enumerate(ev) if e[0]=='W')
  if bs[0]<last_w:err.append('pass %d BSRR before the record store'%k)
  pa12=(ev[bs[0]][1]>>12)&1
  if k in m.midpass_passes:
   if pa12:err.append('pass %d: apply() drove PA12 HIGH after a FAULT edge'%k)
  elif pa12!=s0[3]:err.append('pass %d PA12=%d but record sensor_high=%d'%(k,pa12,s0[3]))
  if len(err)>5:break
 return err,n
def rise_records(m):
 """For each pass whose BSRR write turns PA12 on: the record stored before it in that pass."""
 out=[];prev=0
 for k,ev in sorted(passes(m).items()):
  for j,e in enumerate(ev):
   if e[0]=='BSRR':
    now=(prev&~(e[1]>>16))|(e[1]&0xffff)
    if now&PA12 and not prev&PA12:
     ws=[x for x in ev[:j] if x[0]=='W']
     out.append((k,[x[2] for x in ws[:5]],[x[2] for x in ws[5:10]]))
    prev=now
 return out
def run(img,csr2,slots,ms,world,optr=OPTR_OK):
 m=Mcu(img,csr2,slots[0],slots[1],optr);m.start()
 for _ in range(ms):m.step(world)
 return m

rng=0x1b873593
def rnd():
 global rng;rng^=(rng<<13)&0xffffffff;rng^=rng>>17;rng^=(rng<<5)&0xffffffff;return rng
garbage=lambda:[rnd() for _ in range(5)]
BOOT_CASES=[
 # name, RCC_CSR2 at reset, slot0, slot1, strikes expected after boot, slots may be read, FLASH_OPTR
 ('power_on_valid_record_ignored',PWRRSTF|PINRSTF|LSION,seal(5,2,1),seal(5,2,1),0,False,OPTR_OK),
 ('power_on_locked_record_cleared',PWRRSTF|PINRSTF,seal(NEVER,3,0),seal(NEVER,3,0),0,False,OPTR_OK),
 ('pin_reset_8ms_after_switch_on_strikes',PINRSTF|LSION,seal(8,2,1),seal(7,2,1),3,True,OPTR_OK),
 ('pin_reset_window_edge_50ms',PINRSTF,seal(50,0,1),seal(49,0,1),1,True,OPTR_OK),
 ('pin_reset_51ms_not_attributed',PINRSTF,seal(51,1,1),seal(50,1,1),1,True,OPTR_OK),
 ('pin_reset_sensor_never_on',PINRSTF,seal(NEVER,2,0),seal(NEVER,2,0),2,True,OPTR_OK),
 ('pin_reset_lockout_kept',PINRSTF,seal(NEVER,3,0),seal(NEVER,3,0),3,True,OPTR_OK),
 ('iwdg_reset_keeps_no_strike',IWDGRSTF|PINRSTF,seal(8,2,1),seal(7,2,1),2,True,OPTR_OK),
 ('wwdg_reset_keeps_no_strike',WWDGRSTF|PINRSTF,seal(8,1,1),seal(7,1,1),1,True,OPTR_OK),
 ('software_reset_keeps_no_strike',SFTRSTF|PINRSTF,seal(8,1,1),seal(7,1,1),1,True,OPTR_OK),
 ('option_byte_reset_keeps_no_strike',OBLRSTF|PINRSTF,seal(8,1,1),seal(7,1,1),1,True,OPTR_OK),
 ('low_power_reset_keeps_no_strike',LPWRRSTF|PINRSTF,seal(8,1,1),seal(7,1,1),1,True,OPTR_OK),
 ('no_flags_keeps_no_strike',0,seal(8,1,1),seal(7,1,1),1,True,OPTR_OK),
 ('torn_slot0_uses_slot1',PINRSTF,seal(9,2,1)[:4]+[0],seal(8,1,1),2,True,OPTR_OK),
 ('both_slots_garbage_fresh',PINRSTF,garbage(),garbage(),0,True,OPTR_OK),
 ('lockout_kept_through_iwdg',IWDGRSTF|PINRSTF,seal(NEVER,3,0),seal(NEVER,3,0),3,True,OPTR_OK),
 ('charged_switch_on_no_second_strike',PINRSTF,seal(NEVER,1,0),seal(NEVER,1,0),1,True,OPTR_OK),
 ('parity_check_enabled_locked_record_not_read',PINRSTF,seal(NEVER,3,0),seal(NEVER,3,0),0,False,OPTR_PARITY_ON),
 ('parity_check_enabled_recent_switch_on_not_read',PINRSTF|LSION,seal(8,2,1),seal(7,2,1),0,False,OPTR_PARITY_ON),
]
def boot_case(img,name,csr2,s0,s1,strikes,may_read,optr):
 m=Mcu(img,csr2,s0,s1,optr);stopped=m.boot_to_rmvf();err=[]
 slot0,slot1=m.slots();exp=seal(NEVER,strikes,0);log=m.log
 csr2_write=next((e[1] for e in log if e[0]=='CSR2'),None)
 if not stopped:err.append('did not reach the RMVF write')
 if m.var('reset_cause')!=csr2:err.append('reset_cause %#x'%m.var('reset_cause'))
 if slot0!=exp or slot1!=exp:err.append('slots %s %s expected %s'%(slot0,slot1,exp))
 if csr2_write!=(csr2|RMVF):err.append('CSR2 write %#x'%(csr2_write or 0))
 reads=[k for k,e in enumerate(log) if e[0]=='R'];writes=[k for k,e in enumerate(log) if e[0]=='W']
 optr_reads=[k for k,e in enumerate(log) if e[0]=='OPTR']
 if not may_read and reads:err.append('slot read after PWRRSTF or with SRAM parity checking enabled')
 if reads and (not optr_reads or optr_reads[0]>reads[0]):err.append('slot read before the RAM_PARITY_CHECK option was read')
 if not (csr2&PWRRSTF) and not optr_reads:err.append('FLASH_OPTR not read before the record was restored')
 if reads and writes and max(reads)>writes[0]:err.append('slot read after first slot write')
 csr_at=next(k for k,e in enumerate(log) if e[0]=='CSR2')
 if [log[k][1] for k in writes]!=list(range(0,40,4)) or not writes or csr_at<max(writes):
  err.append('expected one ordered store of slot0 then slot1 before RMVF: %s'%[log[k][1] for k in writes])
 return dict(name=name,kind='boot',status='FAIL' if err else 'PASS',rcc_csr2=hex(csr2),flash_optr=hex(optr),
  strikes_after_boot=strikes,slot_reads=len(reads),errors=err)
def torn(old0,old1,writes,k):
 """SRAM after the first k word writes of a record store (slot 0 then slot 1) landed."""
 s=list(old0)+list(old1)
 for off,val in writes[:k]:s[off//4]=val
 return s[:5],s[5:]
def reboot_strikes(img,old0,old1,writes):
 """A second NRST after k = 0..all of the store's word writes, reset flags not yet cleared
 (PINRSTF): the strike count the next boot stores, for each k."""
 out=[]
 for k in range(len(writes)+1):
  s0,s1=torn(old0,old1,writes,k);m=Mcu(img,PINRSTF,s0,s1);m.boot_to_rmvf();out.append(m.slots()[0][2])
 return out
def boot_second_reset_during_store(img):
 name='second_pin_reset_during_boot_store_counted_once'
 old0,old1=seal(8,1,1),seal(7,1,1);m=Mcu(img,PINRSTF,old0,old1);m.boot_to_rmvf();err=[]
 writes=[(e[1],e[2]) for e in m.log if e[0]=='W']
 if m.slots()[0]!=seal(NEVER,2,0):err.append('first boot stored %s'%m.slots()[0])
 found=reboot_strikes(img,old0,old1,writes)
 if len(writes)!=10 or any(x!=2 for x in found):err.append('strikes after a second NRST at each tear point: %s'%found)
 return dict(name=name,kind='boot',status='FAIL' if err else 'PASS',rcc_csr2=hex(PINRSTF),
  strikes_before=1,strikes_after_first_boot=m.slots()[0][2],strikes_after_second_boot_by_tear_point=found,errors=err)
def boot_pin_configuration(img):
 """U12 pin map at the first main-loop WFI, GPIO/EXTI registers starting from their reset values."""
 name='gpio_exti_configuration_matches_u12_pin_map'
 m=Mcu(img,PINRSTF,seal(NEVER,0,0),seal(NEVER,0,0));err=[]
 for port,base in GPIO_BASE.items():m.w32(base,GPIO_RESET[port][0]);m.w32(base+0x0C,GPIO_RESET[port][1])
 iser=[0] # write-1-to-set: accumulate every NVIC_EnableIRQ write
 def on_iser(uc,acc,addr,size,val,ud):iser[0]|=val&0xffffffff
 m.u.hook_add(UC_HOOK_MEM_WRITE,on_iser,begin=NVIC_ISER,end=NVIC_ISER+3)
 m.start()
 moder={k:m.r32(b) for k,b in GPIO_BASE.items()};otyper={k:m.r32(b+4) for k,b in GPIO_BASE.items()};pupdr={k:m.r32(b+0x0C) for k,b in GPIO_BASE.items()}
 rtsr,ftsr,imr=m.r32(EXTI_BASE),m.r32(EXTI_BASE+4),m.r32(EXTI_BASE+0x80);cr=[m.r32(EXTI_BASE+0x60+4*k) for k in range(4)]
 lines=[n for _,port,n,_,kind in PIN_MAP if kind in ('fall','both')]
 if len(set(lines))!=len(lines):err.append('EXTI line shared by two edge inputs: %s'%lines)
 for pin,port,n,net,kind in PIN_MAP:
  tag='pin %d P%s%d %s'%(pin,port,n,net);mode=(moder[port]>>2*n)&3;pull=(pupdr[port]>>2*n)&3
  if mode!=(1 if kind=='out' else 0):err.append('%s: MODER %d, expected %s'%(tag,mode,'output' if kind=='out' else 'input'))
  if pull:err.append('%s: internal pull %d configured'%(tag,pull))
  if kind=='out' and otyper[port]>>n&1:err.append('%s: open-drain output'%tag)
  if kind in ('fall','both'):
   route=(cr[n//4]>>8*(n%4))&0xff
   if route!=EXTI_PORT[port]:err.append('%s: EXTICR routes line %d to port code %d'%(tag,n,route))
   if not imr>>n&1 or not ftsr>>n&1 or bool(rtsr>>n&1)!=(kind=='both'):err.append('%s: EXTI%d IMR/RTSR/FTSR %d/%d/%d'%(tag,n,imr>>n&1,rtsr>>n&1,ftsr>>n&1))
   if not iser[0]>>exti_irq(n)&1:err.append('%s: EXTI IRQ %d not enabled'%(tag,exti_irq(n)))
 extra=imr&~sum(1<<n for n in lines)
 if extra:err.append('EXTI lines unmasked without an edge input: %#x'%extra)
 for n in (13,14):
  if (moder['A']>>2*n)&3!=2 or (pupdr['A']>>2*n)&3!=(GPIO_RESET['A'][1]>>2*n)&3:err.append('PA%d (SWD) reconfigured'%n)
 if moder['F']!=GPIO_RESET['F'][0] or pupdr['F']!=GPIO_RESET['F'][1]:err.append('GPIOF (PF2/NRST) reconfigured')
 if m.odr:err.append('outputs HIGH at the first WFI: %#x'%m.odr)
 return dict(name=name,kind='boot',status='FAIL' if err else 'PASS',moder={k:hex(v) for k,v in moder.items()},
  pupdr={k:hex(v) for k,v in pupdr.items()},otyper_a=hex(otyper['A']),exticr=[hex(x) for x in cr],imr1=hex(imr),rtsr1=hex(rtsr),ftsr1=hex(ftsr),
  nvic_iser=hex(iser[0]),errors=err)
EXTRA_BOOT_CASES=[boot_second_reset_during_store,boot_pin_configuration]

def ack_en(t):return 300<=t<400 or t>=500 # GPIO26: LOW >=100 ms, HIGH/LOW/HIGH 100 ms each
def diag_fields(m):
 d=m.var('firmware_diagnostic');return dict(firmware_diagnostic=hex(d),faults=d&0x7ff,strikes=(d>>16)&3,lockout=bool(d&DIAG_LOCKOUT))
def loop_common(m,w,err):
 inv,n=pass_invariants(m);err+=inv+m.isr_errors[:4]
 if n!=m.t:err.append('%d passes for %d ms'%(n,m.t))
 for k,s0,s1 in rise_records(m):
  if not(s0==s1 and valid(s0) and s0[3]==1 and s0[1]==0):err.append('PA12 rise in pass %d not recorded first: %s %s'%(k,s0,s1))
 if m.var('firmware_diagnostic')&F_INTERNAL:err.append('F_INTERNAL: start-up checks failed in the model')
 return n
SERVO_REQ=(2100,2300) # servo request HIGH 200 ms, after the 2 s boot cooldown
def main_locked(img):
 """Locked record: PA12 never set, while READY, the ACK, motor arming and a servo burst behave as unlocked."""
 err=[];w=World(en=ack_en,srv=lambda t:SERVO_REQ[0]<=t<SERVO_REQ[1]);m=run(img,PINRSTF,(seal(NEVER,3,0),seal(NEVER,3,0)),2600,w)
 n=loop_common(m,w,err);d=diag_fields(m)
 pa12_sets=[e for e in m.log if e[0]=='BSRR' and e[1]&PA12]
 if pa12_sets:err.append('PA12 set %d times with a locked record'%len(pa12_sets))
 on=lambda pin:[t for t,o in w.odr_log if o&pin]
 ready,motor,servo=on(PA4),on(PA11),on(PA7)
 contiguous=lambda v:bool(v) and v==list(range(v[0],v[-1]+1))
 if not ready or ready[0]>300 or not contiguous(ready) or ready[-1]!=m.t:err.append('READY (PA4) not HIGH from the first ACK stage on: %s..%s'%(ready[:1],ready[-1:]))
 if not motor or motor[0]>700 or not contiguous(motor) or motor[-1]!=SERVO_REQ[0]-1:err.append('motor permit (PA11) not armed from the ACK until the servo request: %s..%s'%(motor[:1],motor[-1:]))
 if not contiguous(servo) or not SERVO_REQ[0]<=servo[0]<=SERVO_REQ[0]+2 or not SERVO_REQ[1]-2<=servo[-1]<=SERVO_REQ[1]+1 or set(servo)&set(motor):
  err.append('servo permit (PA7) %s..%s, overlap with PA11 %s'%(servo[:1],servo[-1:],sorted(set(servo)&set(motor))[:3]))
 if d['faults']:err.append('fault latched: %#x'%d['faults'])
 if m.slots()[0][2]!=3 or not d['lockout'] or d['strikes']!=3:err.append('lockout not kept/diagnosed: %s %s'%(m.slots()[0],d))
 refresh=sum(1 for e in m.log if e[0]=='KR' and e[1]==0xaaaa)
 if len(w.kicks)<40 or refresh<40:err.append('WDI kicks %d, IWDG refreshes %d'%(len(w.kicks),refresh))
 return dict(passes=n,pa12_sets=len(pa12_sets),ready_from_ms=ready[0] if ready else None,motor_permit_ms=[motor[0],motor[-1]] if motor else None,
  servo_permit_ms=[servo[0],servo[-1]] if servo else None,wdi_kicks=len(w.kicks),**d),err
def main_unlocked(img):
 err=[];w=World();m=run(img,PINRSTF,(seal(NEVER,2,0),seal(NEVER,2,0)),2600,w)
 n=loop_common(m,w,err);d=diag_fields(m)
 first=w.rises[0] if w.rises else None;expect=max(w.falls[1],w.kicks[0]+1) if len(w.falls)>1 and w.kicks else None
 if first is None or first!=expect:err.append('first PA12 set at %s ms, expected %s (2nd heartbeat fall %s, first WDI kick %s)'%(first,expect,w.falls[1:2],w.kicks[:1]))
 if len(w.rises)!=1 or not m.odr&PA12:err.append('PA12 not continuously on: rises %s'%w.rises)
 if m.slots()[0][2]!=0 or d['lockout']:err.append('2 s healthy sensor power did not clear strikes: %s'%m.slots()[0])
 return dict(passes=n,second_heartbeat_fall_ms=w.falls[1],first_wdi_kick_ms=w.kicks[0] if w.kicks else None,first_pa12_ms=first,
  strikes_at_end=m.slots()[0][2],**d),err
def main_fast_heartbeat(img):
 err=[];w=World(hb_period=20,hb_phase=5);m=run(img,PWRRSTF,(garbage(),garbage()),300,w)
 n=loop_common(m,w,err)
 if not w.rises or not w.kicks or w.rises[0]!=w.kicks[0]+1 or w.rises[0]<=w.falls[1]:
  err.append('PA12 at %s, first WDI kick %s, 2nd heartbeat fall %s'%(w.rises[:1],w.kicks[:1],w.falls[1:2]))
 return dict(passes=n,second_heartbeat_fall_ms=w.falls[1],first_wdi_kick_ms=w.kicks[0] if w.kicks else None,
  first_pa12_ms=w.rises[0] if w.rises else None),err
def main_in_boot(img,kind):
 err=[];w=World(fault=kind);m=run(img,PWRRSTF,(garbage(),garbage()),3000,w)
 n=loop_common(m,w,err);d=diag_fields(m);s0,s1=m.slots()
 if len(w.rises)!=3 or len(w.faults)!=3:err.append('rises %s faults %s (expected three of each)'%(w.rises,w.faults))
 if len(w.faults)>=3 and any(e[0]=='BSRR' and e[2]=='loop' and e[1]&PA12 and e[3]>w.faults[2] for e in m.log):
  err.append('PA12 set after the third strike')
 if s0[2]!=3 or not d['lockout'] or d['strikes']!=3:err.append('no lockout: slot %s diag %s'%(s0,d))
 want=F_EXTERNAL if kind=='u30' else F_RAIL
 if not d['faults']&want:err.append('expected latched fault %#x in diagnostics, got %#x'%(want,d['faults']))
 if len(w.kicks)<50:err.append('WDI kicks stopped: %d'%len(w.kicks))
 # the record carries the lockout through a pin reset; a power-on restores the sensor
 w2=World(fault=kind);m2=run(img,PINRSTF,(s0,s1),600,w2);e2=[];loop_common(m2,w2,e2)
 if w2.rises or not diag_fields(m2)['lockout']:e2.append('pin reset lost the lockout: rises %s'%w2.rises)
 w3=World();m3=run(img,PWRRSTF,(s0,s1),600,w3);e3=[];loop_common(m3,w3,e3)
 if not w3.rises or diag_fields(m3)['lockout']:e3.append('power-on did not restore the sensor')
 err+=['after pin reset: '+x for x in e2]+['after power-on: '+x for x in e3]
 return dict(passes=n,pa12_rises_ms=w.rises,faults_ms=w.faults,slot_strikes=s0[2],after_pin_reset_rises=len(w2.rises),
  after_power_on_first_pa12_ms=w3.rises[0] if w3.rises else None,**d),err
def pass_record(m,k):
 """(record stored, PA12 bit of the single BSRR write) of loop pass k."""
 ev=passes(m).get(k,[]);ws=[e[2] for e in ev if e[0]=='W'];bs=[e[1] for e in ev if e[0]=='BSRR']
 return ws[:5],((bs[0]>>12)&1 if len(bs)==1 else None)
def main_midpass_fault(img):
 """U30 FAULT edge between snapshot() and apply(), 7 ms after a switch-on: the EXTI6 handler drops
 PA12 at once, apply() keeps it LOW although the core still permits it this pass, and the next pass
 latches F_EXTERNAL and charges the strike."""
 err=[];w=World(fault='u30',midpass=True);m=Mcu(img,PWRRSTF,garbage(),garbage());m.start()
 while m.t<200:m.step(w)
 n=loop_common(m,w,err);d=diag_fields(m);mp=sorted(m.midpass_passes)
 if not mp or not w.rises or mp[0]!=w.rises[0]+7 or mp!=w.faults:err.append('FAULT injection passes %s, rises %s'%(mp,w.rises))
 else:
  k=mp[0];rec,pa12=pass_record(m,k);nxt,_=pass_record(m,k+1)
  if rec[3:4]!=[1] or pa12!=0:err.append('pass %d: record sensor_high %s (core permit expected 1), PA12 written %s'%(k,rec[3:4],pa12))
  if nxt[1:3]!=[NEVER,1]:err.append('pass %d did not charge the strike: %s'%(k+1,nxt))
  if len(w.rises)>1 and w.rises[1]<k+100:err.append('PA12 back after %d ms, before re-qualification'%(w.rises[1]-k))
 if m.slots()[0][2]!=len(w.faults) or not d['faults']&F_EXTERNAL:err.append('strikes %d for %d faults, diag %s'%(m.slots()[0][2],len(w.faults),d))
 return dict(passes=n,pa12_rises_ms=w.rises,fault_passes_ms=mp,slot_strikes=m.slots()[0][2],**d),err
def main_non_sensor_latches(img):
 """An early heartbeat fall (F_HEARTBEAT) 7 ms after the first switch-on, then a motor PWM edge in
 the ACK stage (F_PWM) 7 ms after the next: latches that are not F_EXTERNAL/F_RAIL never strike."""
 err=[];w=World(rise_events=[('hb',7),('pwm',7)]);m=run(img,PINRSTF,(seal(NEVER,2,0),seal(NEVER,2,0)),2600,w)
 n=loop_common(m,w,err);d=diag_fields(m)
 stored=[e[2] for e in m.log if e[0]=='W' and e[1]==8 and e[-2]=='loop']
 lat=[w.diag_at.get(t) for t in w.fired]
 if len(w.rises)!=3 or len(w.fired)!=2 or [f-r for f,r in zip(w.fired,w.rises)]!=[7,7]:
  err.append('PA12 rises %s, latch events %s (expected a rise before each latch and one after recovery)'%(w.rises,w.fired))
 if len(lat)!=2 or any(x is None or not x&want or x&(F_EXTERNAL|F_RAIL) for x,want in zip(lat,(F_HEARTBEAT,F_PWM))):
  err.append('latched faults %s, expected F_HEARTBEAT then F_PWM'%[hex(x) if x is not None else None for x in lat])
 if not stored or max(stored)!=2:err.append('strike counted for a non-sensor latch: stored strikes up to %s'%(max(stored) if stored else None))
 if m.slots()[0][2]!=0 or d['lockout']:err.append('2 s healthy sensor power did not clear strikes: %s'%m.slots()[0])
 return dict(passes=n,pa12_rises_ms=w.rises,latch_events_ms=w.fired,latched_faults=[hex(x) for x in lat if x is not None],
  max_stored_strikes=max(stored) if stored else None,strikes_at_end=m.slots()[0][2],**d),err
def main_late_trip_not_counted(img):
 """Documented limit: a U30 FAULT 51 ms after each switch-on (outside the 50 ms window) latches
 F_EXTERNAL, is retried after each 100 ms re-qualification and is never counted."""
 err=[];w=World(fault='u30',fault_ms=51);m=run(img,PWRRSTF,(garbage(),garbage()),1000,w)
 n=loop_common(m,w,err);d=diag_fields(m)
 stored=[e[2] for e in m.log if e[0]=='W' and e[1]==8 and e[-2]=='loop']
 if len(w.faults)<5 or [f-r for f,r in zip(w.faults,w.rises)]!=[51]*len(w.faults):err.append('faults %s rises %s'%(w.faults,w.rises))
 if not stored or max(stored)!=0 or d['lockout']:err.append('late trips were counted: stored strikes up to %s'%(max(stored) if stored else None))
 gaps=[b-a for a,b in zip(w.rises,w.rises[1:])]
 return dict(passes=n,pa12_rises_ms=w.rises,faults_ms=w.faults,retry_period_ms=sorted(set(gaps)),max_stored_strikes=max(stored) if stored else None,**d),err
def main_strike_then_pin_reset(img):
 err=[];w=World(fault='u30');m=Mcu(img,PWRRSTF,garbage(),garbage());m.start()
 while not w.faults and m.t<1000:m.step(w) # the step that delivers the FAULT runs the pass that latches it
 s0,s1=m.slots();n=loop_common(m,w,err)
 if s0[2]!=1 or s0[1]!=NEVER:err.append('in-boot strike not charged: %s'%s0)
 b=Mcu(img,PINRSTF,s0,s1);b.boot_to_rmvf();a0,a1=b.slots() # NRST from the same FAULT right after that pass
 if a0!=seal(NEVER,1,0) or a1!=a0:err.append('pin reset counted the same event again: %s'%a0)
 # NRST from the same FAULT while the strike pass is still storing the record: every tear point
 prev,_=pass_record(m,m.t-1);ws=[e for e in passes(m).get(m.t-1,[]) if e[0]=='W'];p1=[e[2] for e in ws[5:10]]
 writes=[(e[1],e[2]) for e in passes(m)[m.t] if e[0]=='W']
 found=reboot_strikes(img,prev,p1,writes)
 if prev[1:3]!=[6,0] or len(writes)!=10 or any(x!=1 for x in found):err.append('record before %s; strikes by tear point %s'%(prev,found))
 return dict(passes=n,strikes_before_reset=s0[2],strikes_after_pin_reset=a0[2],strikes_after_nrst_by_tear_point=found),err
class PinWorld(World):
 """Default heartbeat, GPIO26 LOW, no servo request; from t_at one stimulus on top of the World inputs:
 ('edges',line,n): n edges latched on EXTI line (level unchanged, back LOW by the pass);
 ('high',port,bit): that input held HIGH (no edge); ('low',port,bit): that input held LOW (no edge)."""
 def __init__(self,stim,t_at):super().__init__();self.stim=stim;self.t_at=t_at
 def inputs(self,m,t):
  ev=super().inputs(m,t);k=self.stim
  if k and k[0]=='edges' and t==self.t_at:
   m.rpr|=1<<k[1];ev|=1<<k[1]
   if k[2]>1:m.fpr|=1<<k[1]
  elif k and k[0] in ('high','low') and t>=self.t_at:
   reg='idr_'+k[1].lower();v=getattr(m,reg)
   setattr(m,reg,v|(1<<k[2]) if k[0]=='high' else v&~(1<<k[2]))
  return ev
F_ACK=8
PIN_STIMULI=[('GPIO26 double edge, PA3 EXTI3',('edges',3,2),F_ACK),
 ('PWM edge, PA0 EXTI0 (LN)',('edges',0,2),F_PWM),('PWM edge, PA1 EXTI1 (LP)',('edges',1,2),F_PWM),
 ('PWM edge, PC14 EXTI14 (RP)',('edges',14,2),F_PWM),('PWM edge, PC15 EXTI15 (RN)',('edges',15,2),F_PWM),
 ('PWM level HIGH, PA0 (LN)',('high','A',0),F_PWM),('PWM level HIGH, PA1 (LP)',('high','A',1),F_PWM),
 ('PWM level HIGH, PC14 (RP)',('high','C',14),F_PWM),('PWM level HIGH, PC15 (RN)',('high','C',15),F_PWM),
 ('fault bus LOW, PB6 (level only)',('low','B',6),F_EXTERNAL),('no stimulus',None,0)]
def main_pin_map(img):
 """From the first ACK stage (READY HIGH), each U12 edge line and level input reaches the core: the matching fault
 latches in the pass after the stimulus and READY (PA4) drops; without a stimulus nothing latches."""
 err=[];rows=[];T=200
 for label,stim,want in PIN_STIMULI:
  w=PinWorld(stim,T);m=Mcu(img,PINRSTF,seal(NEVER,0,0),seal(NEVER,0,0));m.start()
  while m.t<T-1:m.step(w)
  pre=m.var('firmware_diagnostic')&0x7ff;ready_pre=bool(m.odr&PA4)
  while m.t<T+3:m.step(w)
  got=m.var('firmware_diagnostic')&0x7ff;ready=bool(m.odr&PA4)
  ok=not pre and ready_pre and (bool(got&want) and not ready if want else not got and ready)
  if not ok:err.append('%s: faults before %#x after %#x (expected %#x), READY before %s after %s'%(label,pre,got,want,ready_pre,ready))
  rows.append(dict(stimulus=label,expected_fault=hex(want),latched_faults=hex(got),ready_after=ready))
 return dict(stimulus_at_ms=T,results=rows),err
def main_fault_inside_apply(img):
 """The fault bus falls inside apply(), after its interrupts are masked: the EXTI6 ISR has not run, so only the PB6
 level read in apply() can stop that pass's BSRR write from keeping PA12, READY, motor or servo HIGH. The pending
 EXTI6 edge is then taken after the pass and the next pass latches F_EXTERNAL."""
 err=[];w=World();m=Mcu(img,PINRSTF,seal(NEVER,0,0),seal(NEVER,0,0));m.start()
 while m.t<199:m.step(w) # READY HIGH from the first ACK stage (160 ms), PA12 on since 60 ms
 before=m.odr
 if not(before&PA12 and before&PA4):err.append('PA12/READY not HIGH before the fault: %#x'%before)
 m.apply_fault_pass=m.pass_no+1;m.step(w);k=m.pass_no;m.apply_fault_pass=None
 m.midpass_passes.add(k) # pass invariant: PA12 must be LOW whatever the record says
 bs=[e[1] for e in passes(m).get(k,[]) if e[0]=='BSRR']
 if len(bs)!=1 or bs[0]&(PA12|PA4|PA11|PA7):err.append('pass %d BSRR %s kept outputs HIGH with PB6 LOW in apply()'%(k,[hex(x) for x in bs]))
 if m.odr&(PA12|PA4|PA11|PA7):err.append('outputs %#x HIGH after the pass'%(m.odr&(PA12|PA4|PA11|PA7)))
 m.fpr|=1<<6;m.fault_isr() # pending EXTI6 IRQ, taken once apply() re-enables interrupts
 for _ in range(3):m.step(w)
 n=loop_common(m,w,err);d=diag_fields(m)
 if not d['faults']&F_EXTERNAL:err.append('F_EXTERNAL not latched after the edge: %#x'%d['faults'])
 return dict(passes=n,pass_with_fault_in_apply_ms=k,outputs_before=hex(before),bsrr_in_that_pass=[hex(x) for x in bs],**d),err
MAIN_CASES=[
 ('loop_locked_record_PA12_never_set_ready_motors_servo_unaffected',main_locked),
 ('loop_unlocked_PA12_after_two_heartbeat_falls_and_first_wdi_kick',main_unlocked),
 ('loop_fast_heartbeat_PA12_waits_for_first_wdi_kick',main_fast_heartbeat),
 ('loop_u30_fault_without_reset_three_strikes_then_lockout',lambda img:main_in_boot(img,'u30')),
 ('loop_safe3_sag_without_reset_three_strikes_then_lockout',lambda img:main_in_boot(img,'sag')),
 ('loop_fault_edge_between_snapshot_and_apply_PA12_stays_off',main_midpass_fault),
 ('loop_fault_bus_low_inside_apply_PB6_read_drops_outputs',main_fault_inside_apply),
 ('loop_heartbeat_and_pwm_latches_after_switch_on_no_strike',main_non_sensor_latches),
 ('loop_u30_fault_51ms_after_switch_on_not_counted_documented_limit',main_late_trip_not_counted),
 ('loop_in_boot_strike_then_pin_reset_counted_once',main_strike_then_pin_reset),
 ('loop_u12_pin_map_edges_and_levels_reach_the_core',main_pin_map),
]
def run_all(img):
 global rng
 rng=0x1b873593;rows=[]
 for c in BOOT_CASES:
  try:rows.append(boot_case(img,*c))
  except Exception as e:rows.append(dict(name=c[0],kind='boot',status='FAIL',errors=[repr(e)]))
 for fn in EXTRA_BOOT_CASES:
  try:rows.append(fn(img))
  except Exception as e:rows.append(dict(name=fn.__name__,kind='boot',status='FAIL',errors=[repr(e)]))
 for name,fn in MAIN_CASES:
  try:detail,err=fn(img)
  except Exception as e:detail,err={},[repr(e)]
  rows.append(dict(name=name,kind='main_loop',status='FAIL' if err else 'PASS',detail=detail,errors=err[:8]))
 return rows
def main():
 img=Image(args.elf);rows=run_all(img)
 ok=all(x['status']=='PASS' for x in rows)
 hexp=img.path.parent/'safety_controller.hex'
 record=dict(status='PASS' if ok else 'FAIL',boot_cases=sum(r['kind']=='boot' for r in rows),main_loop_cases=sum(r['kind']=='main_loop' for r in rows),cases=rows,
  engine='Unicorn'+unicorn.__version__+' executes the built image from Reset_Handler (boot cases: to the RMVF write; main-loop cases: into the foreground loop, time advanced at WFI with the real EXTI/SysTick handlers, one case also delivering a FAULT edge at the safety_step call); registers are memory with a small input model; NOT peripheral/silicon simulation',
  elf_sha256=img.sha,hex_sha256=hashlib.sha256(hexp.read_bytes()).hexdigest() if hexp.is_file() else None,
  guard_slot_address=hex(img.slot),noinit_bytes=img.ni_size,wfi_address=hex(img.wfi),flash_optr_model=hex(OPTR_OK),
  harness='test_firmware_boot.py',harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
 text=json.dumps(record,indent=2)
 out=Path(args.out) if args.out else (B/'firmware_boot_tests.json' if PACKAGE_ELF else img.path.parent/'firmware_boot_tests.json')
 out.write_bytes(text.encode());print(text)
 assert record['status']=='PASS'

if __name__=="__main__":main()
