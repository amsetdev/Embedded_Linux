/*
 * Platform stubs for STM32MP1 Cortex-M4 (no MMU, no DMA cache).
 * Provides symbols required by libmetal's generic system layer.
 */
#include <stddef.h>
#include "stm32mp1xx_hal.h"

/* ── libmetal IO mapping (no MMU: VA == PA) ────────────────────────── */
typedef unsigned long metal_phys_addr_t;

void *metal_machine_io_mem_map(void *va, metal_phys_addr_t pa,
                                size_t size, unsigned int flags)
{
    (void)pa; (void)size; (void)flags;
    return va;
}

/* ── Cache ops (M4 has no DMA-coherency issues in shared SRAM) ──────── */
void metal_machine_cache_flush(void *addr, unsigned int len)
{ (void)addr; (void)len; }

void metal_machine_cache_invalidate(void *addr, unsigned int len)
{ (void)addr; (void)len; }

/* ── IPCC handle (extern'd by mbox_ipcc.c) ─────────────────────────── */
IPCC_HandleTypeDef hipcc;

/* ── OpenAMP log stubs (weak — openamp_log.c overrides if it defines them) */
#include <stdarg.h>
__attribute__((weak)) void log_dbg(const char *fmt, ...) { (void)fmt; }
__attribute__((weak)) void log_info(const char *fmt, ...) { (void)fmt; }
__attribute__((weak)) void log_wrn(const char *fmt, ...) { (void)fmt; }
__attribute__((weak)) void log_err(const char *fmt, ...) { (void)fmt; }

/* ── Newlib bare-metal stubs ────────────────────────────────────────── */
/* _init/_fini: called by __libc_init_array; no constructors on M4 */
void _init(void) {}
void _fini(void) {}

/* _sbrk: heap allocator — grows upward from end of BSS */
extern char end;   /* defined by linker script: PROVIDE(end = .) */
void *_sbrk(int incr)
{
    static char *heap = &end;
    char *prev = heap;
    heap += incr;
    return (void *)prev;
}
