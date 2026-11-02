#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* True if a valid FDT header sits at fdt (magic 0xd00dfeed). */
int fdt_valid(uintptr_t fdt);

/*
 * Issue #5: the static tegu DTBs have NO splash/framebuffer node, so ABL
 * must inject it (or not). Scan the live FDT for any node whose name
 * contains "splash" or "framebuffer" and return the first 8-byte `reg`
 * cell pair as the fb base. *which gets the matched node name.
 */
int fdt_find_framebuffer(uintptr_t fdt, uint64_t *out, const char **which);
