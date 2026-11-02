/* wdt.c — optional: stop the zumapro hardware watchdogs before XNU runs.
 *
 * komodo DT: watchdog_cl0@10060000, watchdog_cl1@10070000 (google,zuma-clN-wdt,
 * Samsung s3c2410 layout, Linux sets timeout-sec 30). Whether ABL leaves them
 * armed during `fastboot boot` is unknown. Touching a clock-gated Exynos IP can
 * hang the bus, so this is OFF by default: build with `make WDT_OFF=1` only if
 * the phone reboots by itself ~30 s into the shim/XNU screens.
 * WTCON (+0x0): bit5 timer enable, bit2 IRQ enable, bit0 reset enable.
 */
#include <stdint.h>

#define WDT_CL0   0x10060000UL
#define WDT_CL1   0x10070000UL
#define WTCON     0x0
#define WTCNT     0x8

void wdt_disable(void)
{
#if defined(SHIM_WDT_OFF) && !defined(SHIM_HOST)
    const uintptr_t bases[] = { WDT_CL0, WDT_CL1 };
    for (unsigned i = 0; i < 2; i++) {
        volatile uint32_t *r = (volatile uint32_t *)bases[i];
        r[WTCNT / 4] = 0xffffu;          /* reload first, in case it is counting */
        r[WTCON / 4] = 0;                /* timer, IRQ and reset off */
    }
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
