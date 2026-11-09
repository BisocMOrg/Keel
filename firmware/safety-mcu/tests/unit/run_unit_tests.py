"""Compile test_safety_core.c for Cortex-M0+ and run it under Unicorn.

Instruction emulation of the compiled policy core only; no STM32 peripheral or
silicon simulation. Set ARM_GNU_BIN as for tools/build_firmware.py. Requires
pyelftools and unicorn (MCU_RUNTIME may name a folder holding both).
"""
from pathlib import Path
import sys,os,shutil,subprocess,json,hashlib,datetime
sys.dont_write_bytecode=True
P=Path(__file__).resolve().parents[2] # firmware/safety-mcu
if os.environ.get('MCU_RUNTIME'):sys.path.insert(0,os.environ['MCU_RUNTIME'])
from elftools.elf.elffile import ELFFile
import unicorn
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS,UC_HOOK_INTR
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_XPSR
compiler=os.environ.get('ARM_GNU_BIN')
G=Path(compiler) if compiler else Path(shutil.which('arm-none-eabi-gcc') or shutil.which('arm-none-eabi-gcc.exe') or '').parent
suffix='.exe' if os.name=='nt' else ''
assert (G/('arm-none-eabi-gcc'+suffix)).is_file(), 'Set ARM_GNU_BIN to the ArmGNU14.3.Rel1 bin folder or put it on PATH'
B=P/'build';B.mkdir(exist_ok=True);elf_path=B/'unit_tests.elf'
flags=['-mcpu=cortex-m0plus','-mthumb','-std=c11','-Os','-g3','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-DSTM32C011xx','-I'+str(P/'src')]
cmd=[str(G/('arm-none-eabi-gcc'+suffix)),*flags,*[str(P/x) for x in ['src/safety_core.c','tests/unit/test_safety_core.c','platform/freestanding_runtime.c','vendor/st/startup_stm32c011xx.s']],'-nostdlib','-Wl,--gc-sections','-T'+str(P/'platform/stm32c011f4.ld'),'-Wl,-Map='+str(B/'unit_tests.map'),'-lgcc','-o',str(elf_path)]
p=subprocess.run(cmd,capture_output=True,text=True);(B/'test_compile.log').write_bytes((p.stdout+p.stderr).encode());assert p.returncode==0,p.stdout+p.stderr
size=subprocess.check_output([str(G/('arm-none-eabi-size'+suffix)),str(elf_path)],text=True)
with elf_path.open('rb') as f:
 elf=ELFFile(f);uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);uc.mem_map(0x08000000,0x10000);uc.mem_map(0x20000000,0x10000)
 for seg in elf.iter_segments():
  if seg['p_type']=='PT_LOAD' and seg['p_filesz']:uc.mem_write(seg['p_paddr'],seg.data())
 syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
uc.reg_write(UC_ARM_REG_SP,0x20001800);uc.reg_write(UC_ARM_REG_XPSR,0x01000000)
uc.hook_add(UC_HOOK_INTR,lambda u,n,d:u.emu_stop()) # test ends at bkpt #0
uc.emu_start(syms['Reset_Handler']|1,0,count=1000000000)
rd=lambda n:int.from_bytes(uc.mem_read(syms[n],4),'little')
result=dict(engine='Unicorn'+unicorn.__version__+' ARM-M-class instruction emulation; NOT peripheral/silicon simulation',done=hex(rd('test_done')),assertions=rd('test_total'),passed=rd('test_passed'),failed=rd('test_failed'),failed_ids=[int.from_bytes(uc.mem_read(syms['failed_ids']+4*i,4),'little') for i in range(min(rd('test_failed'),64))],compiler=subprocess.check_output([str(G/('arm-none-eabi-gcc'+suffix)),'--version'],text=True).splitlines()[0],size=size,command=cmd,elf_sha256=hashlib.sha256(elf_path.read_bytes()).hexdigest(),source_sha256={x:hashlib.sha256((P/x).read_bytes()).hexdigest() for x in ['src/safety_core.c','src/safety_core.h','tests/unit/test_safety_core.c']},adversarial_run2_steps={n[4:]:rd(n) for n in ('cov_sensor_on','cov_sensor_on_en_low','cov_actuator_on') if n in syms},strike_limiter_coverage={n[4:]:rd(n) for n in ('cov_lockout_steps','cov_short_events','cov_guard_random_rejected','cov_in_boot_strikes','cov_trip_events') if n in syms},runner='run_unit_tests.py',runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S')
text=json.dumps(result,indent=2)
(B/('unit_results_'+stamp+'.json')).write_bytes(text.encode());(B/'unit_results.json').write_bytes(text.encode());print(text)
assert result['done']=='0x600dc0de' and result['failed']==0
