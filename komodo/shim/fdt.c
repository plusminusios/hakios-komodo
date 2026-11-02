/*
 * fdt.c — минимальный парсер Flattened Device Tree (DTB), freestanding.
 *
 * Формат (big-endian):
 *   header: magic=0xd00dfeed, totalsize, off_dt_struct, off_dt_strings, ...
 *   struct block tokens: BEGIN_NODE(1) name\0(pad4); PROP(3) len,nameoff,val(pad4);
 *                        END_NODE(2); NOP(4); END(9).
 *   strings block: null-terminated имена свойств по off_dt_strings[nameoff].
 *
 * Для reg используем #address-cells/#size-cells корня (на arm64/zuma = 2/2).
 */

#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	0x1
#define FDT_END_NODE	0x2
#define FDT_PROP	0x3
#define FDT_NOP		0x4
#define FDT_END		0x9

struct fdt_header {
	uint32_t magic;
	uint32_t totalsize;
	uint32_t off_dt_struct;
	uint32_t off_dt_strings;
	uint32_t off_mem_rsvmap;
	uint32_t version;
	uint32_t last_comp_version;
	uint32_t boot_cpuid_phys;
	uint32_t size_dt_strings;
	uint32_t size_dt_struct;
};

/* --- big-endian чтение --- */
static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* --- мини-строки (без libc) --- */
static int s_eq(const char *a, const char *b)
{
	while (*a && *b) { if (*a != *b) return 0; a++; b++; }
	return *a == *b;
}
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* compatible может быть списком строк "a\0b\0c\0"; ищем точное совпадение любого. */
static int compat_match(const uint8_t *val, int len, const char *want)
{
	int i = 0;
	while (i < len) {
		const char *s = (const char *)(val + i);
		if (s_eq(s, want))
			return 1;
		while (i < len && val[i]) i++;
		i++; /* \0 */
	}
	return 0;
}

int fdt_valid(const void *fdt)
{
	const struct fdt_header *h = fdt;
	return be32((const uint8_t *)&h->magic) == FDT_MAGIC;
}

uint32_t fdt_totalsize(const void *fdt)
{
	const struct fdt_header *h = fdt;
	return be32((const uint8_t *)&h->totalsize);
}

/* Прочитать #address-cells/#size-cells корня (дефолт 2/2 для arm64). */
static void root_cells(const uint8_t *base, uint32_t off_struct,
		       uint32_t off_strings, uint32_t *ac, uint32_t *sc)
{
	*ac = 2; *sc = 2;
	uint32_t i = off_struct;
	int depth = 0;
	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (depth > 1) /* ушли глубже корня */
				return;
		} else if (tok == FDT_END_NODE) {
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			if (depth == 1 && plen == 4) {
				if (s_eq(pn, "#address-cells")) *ac = be32(base + i);
				else if (s_eq(pn, "#size-cells")) *sc = be32(base + i);
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else { /* FDT_END или мусор */
			return;
		}
	}
}

static uint64_t read_cells(const uint8_t *p, uint32_t cells)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < cells; i++)
		v = (v << 32) | be32(p + i * 4);
	return v;
}

/* Общий обход: ищем узел, у которого либо compatible==want (mode 0),
 * либо имя начинается с "memory" (mode 1). Возвращаем его reg. */
static int find_reg(const void *fdt, const char *want, int mem_mode,
		    uint64_t *base_out, uint64_t *size_out)
{
	if (!fdt_valid(fdt))
		return -1;
	const struct fdt_header *h = fdt;
	const uint8_t *base = fdt;
	uint32_t off_struct = be32((const uint8_t *)&h->off_dt_struct);
	uint32_t off_strings = be32((const uint8_t *)&h->off_dt_strings);
	uint32_t ac, sc;
	root_cells(base, off_struct, off_strings, &ac, &sc);

	uint32_t i = off_struct;
	int matched_depth = -1;   /* глубина узла-кандидата */
	int depth = 0;
	uint64_t pend_base = 0, pend_size = 0;
	int have_reg = 0, is_match = 0;

	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (mem_mode) {
				/* имя вида "memory@..." или "memory" */
				if (nm[0]=='m'&&nm[1]=='e'&&nm[2]=='m'&&nm[3]=='o'&&
				    nm[4]=='r'&&nm[5]=='y'&&(nm[6]==0||nm[6]=='@')) {
					is_match = 1; matched_depth = depth; have_reg = 0;
				}
			}
		} else if (tok == FDT_END_NODE) {
			if (is_match && depth == matched_depth) {
				if (have_reg) {
					*base_out = pend_base; *size_out = pend_size;
					return 0;
				}
				is_match = 0; matched_depth = -1;
			}
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			const uint8_t *val = base + i;
			if (!mem_mode && s_eq(pn, "compatible")) {
				if (compat_match(val, plen, want)) {
					is_match = 1; matched_depth = depth; have_reg = 0;
				}
			}
			if (is_match && depth == matched_depth && s_eq(pn, "reg")) {
				pend_base = read_cells(val, ac);
				pend_size = read_cells(val + ac * 4, sc);
				have_reg = 1;
				/* для compatible-режима можно вернуть сразу после END_NODE;
				 * reg уже сохранён */
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else { /* FDT_END */
			break;
		}
	}
	return -1;
}

int fdt_find_reg_by_compatible(const void *fdt, const char *compat,
			       uint64_t *base, uint64_t *size)
{
	return find_reg(fdt, compat, 0, base, size);
}

int fdt_get_memory(const void *fdt, uint64_t *base, uint64_t *size)
{
	return find_reg(fdt, 0, 1, base, size);
}

int fdt_root_string(const void *fdt, const char *prop, char *buf, int len)
{
	if (!fdt_valid(fdt))
		return -1;
	const struct fdt_header *h = fdt;
	const uint8_t *base = fdt;
	uint32_t off_struct = be32((const uint8_t *)&h->off_dt_struct);
	uint32_t off_strings = be32((const uint8_t *)&h->off_dt_strings);
	uint32_t i = off_struct;
	int depth = 0;
	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (depth > 1) return -1;
		} else if (tok == FDT_END_NODE) {
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			if (depth == 1 && s_eq(pn, prop)) {
				int n = 0;
				const char *v = (const char *)(base + i);
				while (n < len - 1 && n < (int)plen && v[n]) { buf[n] = v[n]; n++; }
				buf[n] = 0;
				return n;
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
	return -1;
}

/* ---- расширения: перечисление ядер / chosen / диапазоны памяти ---- */

static int name_is(const char *nm, const char *pfx)
{
	int i = 0;
	while (pfx[i]) { if (nm[i] != pfx[i]) return 0; i++; }
	return 1;
}

int fdt_cpus(const void *fdt, uint64_t *mpidr, int max, int *disabled)
{
	if (disabled) *disabled = 0;
	if (!fdt_valid(fdt)) return -1;
	const struct fdt_header *h = fdt;
	const uint8_t *base = fdt;
	uint32_t off_struct = be32((const uint8_t *)&h->off_dt_struct);
	uint32_t off_strings = be32((const uint8_t *)&h->off_dt_strings);

	uint32_t i = off_struct;
	int depth = 0, cpus_depth = -1;
	uint32_t ac = 2;             /* #address-cells узла /cpus */
	int in_cpu = 0, cpu_depth = -1, cpu_disabled = 0, cpu_has_reg = 0;
	uint64_t cpu_reg = 0;
	int count = 0;

	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (cpus_depth < 0 && s_eq(nm, "cpus"))
				cpus_depth = depth;
			else if (cpus_depth > 0 && depth == cpus_depth + 1 &&
				 name_is(nm, "cpu")) {
				in_cpu = 1; cpu_depth = depth;
				cpu_disabled = 0; cpu_has_reg = 0; cpu_reg = 0;
			}
		} else if (tok == FDT_END_NODE) {
			if (in_cpu && depth == cpu_depth) {
				if (cpu_has_reg) {
					if (cpu_disabled) { if (disabled) (*disabled)++; }
					else { if (count < max) mpidr[count] = cpu_reg; count++; }
				}
				in_cpu = 0; cpu_depth = -1;
			}
			if (cpus_depth > 0 && depth == cpus_depth) break; /* вышли из /cpus */
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			const uint8_t *val = base + i;
			if (cpus_depth > 0 && depth == cpus_depth && plen == 4 &&
			    s_eq(pn, "#address-cells"))
				ac = be32(val);
			if (in_cpu && depth == cpu_depth) {
				if (s_eq(pn, "reg")) { cpu_reg = read_cells(val, ac); cpu_has_reg = 1; }
				else if (s_eq(pn, "status")) {
					if (name_is((const char *)val, "disabled") ||
					    name_is((const char *)val, "fail"))
						cpu_disabled = 1;
				}
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
	return count;
}

int fdt_chosen_string(const void *fdt, const char *prop, char *buf, int len)
{
	if (!fdt_valid(fdt)) return -1;
	const struct fdt_header *h = fdt;
	const uint8_t *base = fdt;
	uint32_t off_struct = be32((const uint8_t *)&h->off_dt_struct);
	uint32_t off_strings = be32((const uint8_t *)&h->off_dt_strings);
	uint32_t i = off_struct;
	int depth = 0, in_chosen = 0, chosen_depth = -1;
	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (!in_chosen && s_eq(nm, "chosen")) {
				in_chosen = 1; chosen_depth = depth;
			}
		} else if (tok == FDT_END_NODE) {
			if (in_chosen && depth == chosen_depth) return -1;
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			if (in_chosen && depth == chosen_depth && s_eq(pn, prop)) {
				int n = 0;
				const char *v = (const char *)(base + i);
				while (n < len - 1 && n < (int)plen && v[n]) { buf[n] = v[n]; n++; }
				buf[n] = 0;
				return n;
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
	return -1;
}

int fdt_memory_ranges(const void *fdt, uint64_t *base_out, uint64_t *size_out, int max)
{
	if (!fdt_valid(fdt)) return -1;
	const struct fdt_header *h = fdt;
	const uint8_t *base = fdt;
	uint32_t off_struct = be32((const uint8_t *)&h->off_dt_struct);
	uint32_t off_strings = be32((const uint8_t *)&h->off_dt_strings);
	uint32_t ac, sc;
	root_cells(base, off_struct, off_strings, &ac, &sc);

	uint32_t i = off_struct;
	int depth = 0, in_mem = 0, mem_depth = -1;
	int count = 0;
	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (nm[0]=='m'&&nm[1]=='e'&&nm[2]=='m'&&nm[3]=='o'&&
			    nm[4]=='r'&&nm[5]=='y'&&(nm[6]==0||nm[6]=='@')) {
				in_mem = 1; mem_depth = depth;
			}
		} else if (tok == FDT_END_NODE) {
			if (in_mem && depth == mem_depth) { in_mem = 0; mem_depth = -1; }
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			const uint8_t *val = base + i;
			if (in_mem && depth == mem_depth && s_eq(pn, "reg")) {
				/* может быть несколько пар base/size подряд */
				uint32_t pair = (ac + sc) * 4;
				uint32_t off = 0;
				while (off + pair <= plen) {
					if (count < max) {
						base_out[count] = read_cells(val + off, ac);
						size_out[count] = read_cells(val + off + ac*4, sc);
					}
					count++;
					off += pair;
				}
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
	return count;
}

int fdt_reserved_ranges(const void *fdt, uint64_t *base_out, uint64_t *size_out, int max)
{
	if (!fdt_valid(fdt)) return -1;
	const struct fdt_header *h = fdt;
	const uint8_t *base = fdt;
	uint32_t off_struct = be32((const uint8_t *)&h->off_dt_struct);
	uint32_t off_strings = be32((const uint8_t *)&h->off_dt_strings);
	uint32_t ac, sc;
	root_cells(base, off_struct, off_strings, &ac, &sc);

	uint32_t i = off_struct;
	int depth = 0, rm_depth = -1;
	int in_child = 0, child_depth = -1, has_reg = 0;
	uint64_t cb = 0, cs = 0;
	int count = 0;

	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (rm_depth < 0 && s_eq(nm, "reserved-memory"))
				rm_depth = depth;
			else if (rm_depth > 0 && depth == rm_depth + 1) {
				in_child = 1; child_depth = depth; has_reg = 0;
			}
		} else if (tok == FDT_END_NODE) {
			if (in_child && depth == child_depth) {
				if (has_reg) {
					if (count < max) { base_out[count] = cb; size_out[count] = cs; }
					count++;
				}
				in_child = 0; child_depth = -1;
			}
			if (rm_depth > 0 && depth == rm_depth) break;
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			const uint8_t *val = base + i;
			if (in_child && depth == child_depth && s_eq(pn, "reg") &&
			    plen >= (ac + sc) * 4) {
				cb = read_cells(val, ac);
				cs = read_cells(val + ac * 4, sc);
				has_reg = 1;
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
	return count;
}

/* Целочисленное свойство /chosen (4 или 8 байт BE). 1 при успехе. */
int fdt_chosen_u64(const void *fdt, const char *prop, uint64_t *out)
{
	if (!fdt_valid(fdt)) return 0;
	const struct fdt_header *h = fdt;
	const uint8_t *base = fdt;
	uint32_t off_struct = be32((const uint8_t *)&h->off_dt_struct);
	uint32_t off_strings = be32((const uint8_t *)&h->off_dt_strings);
	uint32_t i = off_struct;
	int depth = 0, in_chosen = 0, chosen_depth = -1;
	for (;;) {
		uint32_t tok = be32(base + i); i += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)(base + i);
			i += (s_len(nm) + 1 + 3) & ~3u;
			depth++;
			if (!in_chosen && s_eq(nm, "chosen")) { in_chosen = 1; chosen_depth = depth; }
		} else if (tok == FDT_END_NODE) {
			if (in_chosen && depth == chosen_depth) return 0;
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t plen = be32(base + i);
			uint32_t noff = be32(base + i + 4); i += 8;
			const char *pn = (const char *)(base + off_strings + noff);
			if (in_chosen && depth == chosen_depth && s_eq(pn, prop)) {
				if (plen == 8) {
					*out = ((uint64_t)be32(base + i) << 32) | be32(base + i + 4);
					return 1;
				} else if (plen == 4) {
					*out = be32(base + i);
					return 1;
				}
				return 0;
			}
			i += (plen + 3) & ~3u;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
	return 0;
}
