ENTRY(Reset_Handler)
MEMORY { FLASH(rx) : ORIGIN = 0x08000000, LENGTH = 16K
         RAM(rwx) : ORIGIN = 0x20000000, LENGTH = 6K }
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
 .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
 .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
 .ARM.exidx : { *(.ARM.exidx*) } > FLASH
 .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> FLASH
 _sidata = LOADADDR(.data);
 .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
 /* Outside _sbss.._ebss and .data: startup neither zeroes nor copies it, so
  * contents survive pin/watchdog resets (sensor strike record). */
 .noinit (NOLOAD) : { . = ALIGN(4); _snoinit = .; KEEP(*(.noinit*)) . = ALIGN(4); _enoinit = .; } > RAM
 __image_end = LOADADDR(.data) + SIZEOF(.data);
 .safety_crc : { KEEP(*(.safety_crc)) } > FLASH
 ASSERT(_enoinit < ORIGIN(RAM)+LENGTH(RAM)-1024,"Less than 1KiB stack margin")
 ASSERT(_snoinit >= _ebss,".noinit must not overlap zeroed .bss")
 /DISCARD/ : { *(.comment*) *(.note*) }
}
