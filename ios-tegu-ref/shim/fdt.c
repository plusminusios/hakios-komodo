/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_valid(uintptr_t fdt)
{
	return fdt && be32(fdt) == FDT_MAGIC;
}

/* Case-insensitive substring match on an FDT node/prop name. */
static int contains(const char *hay, const char *needle)
{
	for (; *hay; hay++) {
		const char *h = hay, *n = needle;
		while (*n && *h) {
			char a = *h | 0x20, b = *n | 0x20;
			if (a != b)
				break;
			h++;
			n++;
		}
		if (!*n)
			return 1;
	}
	return 0;
}

int fdt_find_framebuffer(uintptr_t fdt, uint64_t *out, const char **which)
{
	if (!fdt_valid(fdt))
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uint32_t depth = 0;
	int match_depth = -1;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			if (contains(name, "splash") || contains(name, "framebuffer"))
				match_depth = depth;
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (match_depth == depth + 1)
				match_depth = -1;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p);
			uintptr_t val = p + 8;
			uintptr_t strings = fdt + be32(fdt + 12);
			uint32_t nameoff = be32(p + 4);
			if (match_depth == depth && len >= 8 &&
			    streq((const char *)(strings + nameoff), "reg")) {
				*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				*which = "fdt reg";
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}
