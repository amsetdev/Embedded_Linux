/*
 * Linker script for STM32MP157F-DK2 Cortex-M4 firmware.
 * Addresses verified against /proc/device-tree/reserved-memory/ on the board.
 */
ENTRY(Reset_Handler)

/* OpenAMP region symbols referenced by openamp.c */
__OPENAMP_region_start__ = 0x10040000;
__OPENAMP_region_end__   = 0x10046000;   /* covers vring0+vring1+buffer */

MEMORY
{
    /* M4 code + data — mcuram2@10000000, 256 KB */
    SRAM1 (xrw) : ORIGIN = 0x10000000, LENGTH = 0x3E000

    /* OpenAMP shared regions (DO NOT put code here) */
    VRING0 (rw) : ORIGIN = 0x10040000, LENGTH = 0x1000
    VRING1 (rw) : ORIGIN = 0x10041000, LENGTH = 0x1000
    VDEV_BUF (rw) : ORIGIN = 0x10042000, LENGTH = 0x4000
    RSC_TABLE (rw) : ORIGIN = 0x10048000, LENGTH = 0x800
}

_stack_size = 0x1000;
_heap_size  = 0x1000;

SECTIONS
{
    /* Resource table must live at 0x10048000 */
    .resource_table :
    {
        KEEP(*(.resource_table))
    } > RSC_TABLE

    /* Vector table at start of SRAM1 */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > SRAM1

    .text :
    {
        *(.text*)
        *(.rodata*)
        *(.glue_7*)
        *(.glue_7)
        KEEP(*(.init))
        KEEP(*(.fini))
    } > SRAM1

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > SRAM1
    .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > SRAM1

    .data :
    {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > SRAM1
    /* Loaded by remoteproc into RAM — LMA == VMA */
    _sidata = _sdata;

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > SRAM1

    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        . = . + _heap_size;
        . = . + _stack_size;
        . = ALIGN(8);
        _estack = .;
    } > SRAM1
}
