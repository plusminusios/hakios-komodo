/*
 * exceptions.c — shim exception handler. Catches a fault that would otherwise
 * bounce the device silently back to fastboot, records it, and PAINTS it on the
 * framebuffer (red screen + ESR/ELR/FAR/EC) so the first hardware session can
 * read the cause from a photo. Returns into the vectors.S wfe-loop (we do not
 * restore context).
 *
 * ESR_ELx.EC (bits 31:26): 0x25 Data Abort (cur EL), 0x24 (lower EL),
 * 0x21/0x20 Instruction Abort, 0x15 SVC, 0x17 SMC, 0x00 Unknown.
 */
#include <stdint.h>
#include "console.h"

void fb_decon_trigger(void);   /* fb.c: push the framebuffer to the panel */

struct exc_record {
    uint32_t valid;   /* 0xFA17ED00 | type */
    uint32_t type;
    uint64_t esr, elr, far;
    uint32_t ec;
};

volatile struct exc_record g_last_exc __attribute__((aligned(64)));

void exc_handler(uint64_t esr, uint64_t elr, uint64_t far, uint64_t type)
{
    g_last_exc.type = (uint32_t)type;
    g_last_exc.esr = esr;
    g_last_exc.elr = elr;
    g_last_exc.far = far;
    g_last_exc.ec = (uint32_t)((esr >> 26) & 0x3f);
    g_last_exc.valid = 0xFA17ED00u | (uint32_t)(type & 0xff);
#ifndef SHIM_HOST
    __asm__ volatile("dsb sy" ::: "memory");   /* AArch64 barrier (device only) */
#endif

    /* Make the fault visible: red screen + the key registers. */
    con_fill(C_RED);
    con_logreset();
    con_logstr("EXCEPTION", C_WHITE);
    con_log("TYPE", type, C_WHITE);
    con_log("EC",   g_last_exc.ec, C_YELLOW);
    con_log("ESR",  esr, C_WHITE);
    con_log("ELR",  elr, C_WHITE);
    con_log("FAR",  far, C_WHITE);
    fb_decon_trigger();   /* make the fault visible on the panel */
}

#ifndef SHIM_HOST
void exc_install_vectors(void);   /* vectors.S */
void exc_init(void)
{
    g_last_exc.valid = 0;
    exc_install_vectors();
}
#else
void exc_init(void) { g_last_exc.valid = 0; }   /* host: no AArch64 vectors */
#endif
