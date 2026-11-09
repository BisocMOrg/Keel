"""Build the safety-controller image into firmware/safety-mcu/build/.

Compiles src/, platform/ and vendor/ with Arm GNU Toolchain 14.3.Rel1 (set
ARM_GNU_BIN to its bin folder or put it on PATH), inserts the image CRC-32
into .safety_crc and writes safety_controller.{elf,bin,hex,map} and
firmware_build.json. Requires pyelftools (tools/requirements.txt).
"""
from pathlib import Path
import subprocess,sys,json,hashlib,zlib,datetime,os,shutil
P=Path(__file__).resolve().parents[1] # firmware/safety-mcu
from elftools.elf.elffile import ELFFile
compiler=os.environ.get('ARM_GNU_BIN')
G=Path(compiler) if compiler else Path(shutil.which('arm-none-eabi-gcc') or shutil.which('arm-none-eabi-gcc.exe') or '').parent
suffix='.exe' if os.name=='nt' else ''
assert (G/('arm-none-eabi-gcc'+suffix)).is_file(), 'Set ARM_GNU_BIN to the ArmGNU14.3.Rel1 bin folder or put it on PATH'
B=P/'build';B.mkdir(exist_ok=True)
elf=B/'safety_controller.elf';binary=B/'safety_controller.bin'
flags=['-mcpu=cortex-m0plus','-mthumb','-std=c11','-Os','-g3','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-DSTM32C011xx','-DUSE_ST_SYSTEM',*['-I'+str(P/d) for d in ('src','vendor/st','vendor/cmsis')]]
sources=['src/safety_core.c','src/firmware.c','platform/freestanding_runtime.c','vendor/st/system_stm32c0xx.c','vendor/st/startup_stm32c011xx.s']
cmd=[str(G/('arm-none-eabi-gcc'+suffix)),*flags,*[str(P/x) for x in sources],'-nostdlib','-Wl,--gc-sections','-T'+str(P/'platform/stm32c011f4.ld'),'-Wl,-Map='+str(B/'safety_controller.map'),'-lgcc','-o',str(elf)]
p=subprocess.run(cmd,capture_output=True,text=True);(B/'firmware_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
objcopy=str(G/('arm-none-eabi-objcopy'+suffix))
subprocess.run([objcopy,'-O','binary',str(elf),str(binary)],check=True)
with elf.open('rb') as f:
 e=ELFFile(f);sym={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
end=sym['__image_end']-0x08000000;crc_offset=sym['expected_image_crc']-0x08000000
assert crc_offset>=end
raw=binary.read_bytes();crc=zlib.crc32(raw[:end]);(B/'image_crc.bin').write_bytes(crc.to_bytes(4,'little'))
subprocess.run([objcopy,'--update-section','.safety_crc='+str(B/'image_crc.bin'),str(elf)],check=True)
subprocess.run([objcopy,'-O','binary',str(elf),str(binary)],check=True)
subprocess.run([objcopy,'-O','ihex',str(elf),str(B/'safety_controller.hex')],check=True)
raw=binary.read_bytes();assert int.from_bytes(raw[crc_offset:crc_offset+4],'little')==zlib.crc32(raw[:end])
size=subprocess.check_output([str(G/('arm-none-eabi-size'+suffix)),str(elf)],text=True)
release=P/'release/safety_controller.hex'
record=dict(status='REBUILT_FROM_SOURCE',matches_release_hex=release.is_file() and release.read_bytes()==(B/'safety_controller.hex').read_bytes(),created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),command=cmd,compiler=subprocess.check_output([str(G/('arm-none-eabi-gcc'+suffix)),'--version'],text=True).splitlines()[0],binary_bytes=len(raw),flash_capacity_bytes=16384,ram_capacity_bytes=6144,size=size,crc32=hex(crc),crc_region_bytes=end,crc_offset=crc_offset,sources={x:hashlib.sha256((P/x).read_bytes()).hexdigest() for x in sources},header_and_link_inputs={x:hashlib.sha256((P/x).read_bytes()).hexdigest() for x in ['src/safety_core.h','platform/stm32c011f4.ld']},outputs={x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in [elf,binary,B/'safety_controller.hex']},limitations=['Unit tests execute the compiled policy core, not MCU peripherals','The hardware characterisation still open is listed in docs/qualification.md','Engineering safety interlock, not a certified safety device'])
(B/'firmware_build.json').write_text(json.dumps(record,indent=2));print(json.dumps(record,indent=2))
