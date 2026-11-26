MEMORY { FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 512K
         RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 64K }
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
}
