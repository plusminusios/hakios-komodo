/* loader.h — load the KOMOXNU payload (XNU kernel + ADT) and boot it. */
#ifndef KOMODO_SHIM_LOADER_H
#define KOMODO_SHIM_LOADER_H

#include <stdint.h>
#include "fb.h"

/* Result of preparing the boot (everything bar the jump). */
struct loader_result {
    const char *error;     /* NULL on success */
    uint64_t    entry_pa;  /* kernel entry, physical */
    uint64_t    boot_args_pa;
    uint64_t    adt_pa;
    uint64_t    top_of_kernel_data;
    uint32_t    nsegs;
    int         adt_fb_patched;   /* /tegu-fbcon placeholders replaced */
};

/* Find + stage the payload from the ABL ramdisk (via /chosen initrd in `dtb`),
 * copy segments, place the ADT and boot_args. Does NOT jump. Returns 0 on
 * success (fills `out`), non-zero on error. `fb` supplies the Video args. */
int loader_prepare(uintptr_t dtb, const struct fb_info *fb, struct loader_result *out);

/* Prepare, then jump to the kernel with x0 = boot_args (never returns on
 * success). On the host build (LOADER_HOST) the jump is compiled out. */
const char *loader_boot(uintptr_t dtb, const struct fb_info *fb);

#endif
