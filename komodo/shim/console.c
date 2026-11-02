/* console.c — komodo framebuffer console (freestanding). See console.h. */
#include "console.h"

static volatile uint32_t *g_fb;
static uint32_t g_w, g_h, g_stride_px;

/* Compact 8x8 font. FONT_CHARS[i] is the character of glyph i; FONT[i] is its
 * 8 rows (8 columns each, '#' = pixel on). Enough for hex + debug labels. */
static const char FONT_CHARS[] =
    " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-.>";

/* Flat 2D char array (NOT an array of pointers): keeps the glyph data inline so
 * the freestanding image has no ABS64 relocations (it is loaded at an arbitrary
 * address by ABL with no relocation pass). Each row is <= 8 chars + NUL. */
static const char FONT[][8][9] = {
/* ' ' */ {"        ","        ","        ","        ","        ","        ","        ","        "},
/* '0' */ {" ####   ","#    #  ","#   ##  ","#  # #  ","# #  #  ","##   #  ","#    #  "," ####   "},
/* '1' */ {"  ##    "," # #    ","   #    ","   #    ","   #    ","   #    ","   #    "," #####  "},
/* '2' */ {" ####   ","#    #  ","     #  ","    #   ","   #    ","  #     "," #      ","######  "},
/* '3' */ {" ####   ","#    #  ","     #  ","  ###   ","     #  ","     #  ","#    #  "," ####   "},
/* '4' */ {"   ##   ","  # #   "," #  #   ","#   #   ","######  ","    #   ","    #   ","    #   "},
/* '5' */ {"######  ","#       ","#       ","#####   ","     #  ","     #  ","#    #  "," ####   "},
/* '6' */ {" ####   ","#    #  ","#       ","#####   ","#    #  ","#    #  ","#    #  "," ####   "},
/* '7' */ {"######  ","     #  ","    #   ","   #    ","  #     ","  #     ","  #     ","  #     "},
/* '8' */ {" ####   ","#    #  ","#    #  "," ####   ","#    #  ","#    #  ","#    #  "," ####   "},
/* '9' */ {" ####   ","#    #  ","#    #  "," #####  ","     #  ","     #  ","#    #  "," ####   "},
/* 'A' */ {" ####   ","#    #  ","#    #  ","#    #  ","######  ","#    #  ","#    #  ","#    #  "},
/* 'B' */ {"#####   ","#    #  ","#    #  ","#####   ","#    #  ","#    #  ","#    #  ","#####   "},
/* 'C' */ {" ####   ","#    #  ","#       ","#       ","#       ","#       ","#    #  "," ####   "},
/* 'D' */ {"#####   ","#    #  ","#    #  ","#    #  ","#    #  ","#    #  ","#    #  ","#####   "},
/* 'E' */ {"######  ","#       ","#       ","#####   ","#       ","#       ","#       ","######  "},
/* 'F' */ {"######  ","#       ","#       ","#####   ","#       ","#       ","#       ","#       "},
/* 'G' */ {" ####   ","#    #  ","#       ","#  ###  ","#    #  ","#    #  ","#    #  "," ####   "},
/* 'H' */ {"#    #  ","#    #  ","#    #  ","######  ","#    #  ","#    #  ","#    #  ","#    #  "},
/* 'I' */ {" #####  ","   #    ","   #    ","   #    ","   #    ","   #    ","   #    "," #####  "},
/* 'J' */ {"   ###  ","     #  ","     #  ","     #  ","     #  ","#    #  ","#    #  "," ####   "},
/* 'K' */ {"#    #  ","#   #   ","#  #    ","###     ","#  #    ","#   #   ","#    #  ","#    #  "},
/* 'L' */ {"#       ","#       ","#       ","#       ","#       ","#       ","#       ","######  "},
/* 'M' */ {"#    #  ","##  ##  ","# ## #  ","#    #  ","#    #  ","#    #  ","#    #  ","#    #  "},
/* 'N' */ {"#    #  ","##   #  ","# #  #  ","#  # #  ","#   ##  ","#    #  ","#    #  ","#    #  "},
/* 'O' */ {" ####   ","#    #  ","#    #  ","#    #  ","#    #  ","#    #  ","#    #  "," ####   "},
/* 'P' */ {"#####   ","#    #  ","#    #  ","#####   ","#       ","#       ","#       ","#       "},
/* 'Q' */ {" ####   ","#    #  ","#    #  ","#    #  ","#    #  ","#  # #  ","#   #   "," ### #  "},
/* 'R' */ {"#####   ","#    #  ","#    #  ","#####   ","#  #    ","#   #   ","#    #  ","#    #  "},
/* 'S' */ {" ####   ","#    #  ","#       "," ####   ","     #  ","     #  ","#    #  "," ####   "},
/* 'T' */ {"####### ","   #    ","   #    ","   #    ","   #    ","   #    ","   #    ","   #    "},
/* 'U' */ {"#    #  ","#    #  ","#    #  ","#    #  ","#    #  ","#    #  ","#    #  "," ####   "},
/* 'V' */ {"#    #  ","#    #  ","#    #  ","#    #  ","#    #  "," #  #   ","  ##    ","  ##    "},
/* 'W' */ {"#    #  ","#    #  ","#    #  ","#    #  ","# ## #  ","# ## #  ","##  ##  ","#    #  "},
/* 'X' */ {"#    #  ","#    #  "," #  #   ","  ##    ","  ##    "," #  #   ","#    #  ","#    #  "},
/* 'Y' */ {"#    #  ","#    #  "," #  #   ","  ##    ","   #    ","   #    ","   #    ","   #    "},
/* 'Z' */ {"######  ","     #  ","    #   ","   #    ","  #     "," #      ","#       ","######  "},
/* ':' */ {"        ","  ##    ","  ##    ","        ","        ","  ##    ","  ##    ","        "},
/* '-' */ {"        ","        ","        "," #####  ","        ","        ","        ","        "},
/* '.' */ {"        ","        ","        ","        ","        ","        ","  ##    ","  ##    "},
/* '>' */ {"#       "," #      ","  #     ","   #    ","    #   ","   #    ","  #     ","#       "},
};

int con_init(uint64_t fb_base, uint32_t w, uint32_t h, uint32_t stride)
{
    if (!fb_base) {
        fb_base = KOMODO_FB_FALLBACK;
        w = KOMODO_FB_W; h = KOMODO_FB_H; stride = KOMODO_FB_STRIDE;
    }
    if (!w) w = KOMODO_FB_W;
    if (!h) h = KOMODO_FB_H;
    if (!stride) stride = w * 4;
    g_fb = (volatile uint32_t *)fb_base;
    g_w = w; g_h = h; g_stride_px = stride / 4;
    return g_fb != 0;
}

void con_fill(uint32_t color)
{
    for (uint32_t y = 0; y < g_h; y++) {
        volatile uint32_t *row = g_fb + (uint64_t)y * g_stride_px;
        for (uint32_t x = 0; x < g_w; x++)
            row[x] = color;
    }
}

void con_stage(int n, uint32_t color)
{
    /* Each bar: 1/20 of the screen height, a gap of the same, from the top. */
    uint32_t bar = g_h / 40;
    if (!bar) bar = 1;
    for (int i = 0; i < n; i++) {
        uint32_t y0 = (uint32_t)i * bar * 2;
        for (uint32_t y = y0; y < y0 + bar && y < g_h; y++) {
            volatile uint32_t *row = g_fb + (uint64_t)y * g_stride_px;
            for (uint32_t x = 0; x < g_w; x++)
                row[x] = color;
        }
    }
}

static int glyph_index(char c)
{
    if (c >= 'a' && c <= 'z')   /* font has only uppercase: fold */
        c = (char)(c - 'a' + 'A');
    for (int i = 0; FONT_CHARS[i]; i++)
        if (FONT_CHARS[i] == c)
            return i;
    return 0; /* space */
}

void con_glyph(int gidx, int px, int py, int scale, uint32_t color)
{
    if (gidx < 0 || (unsigned)gidx >= sizeof(FONT) / sizeof(FONT[0]))
        return;
    for (int ry = 0; ry < 8; ry++) {
        const char *rowstr = FONT[gidx][ry];
        for (int rx = 0; rx < 8 && rowstr[rx]; rx++) {
            if (rowstr[rx] != '#')
                continue;
            for (int sy = 0; sy < scale; sy++) {
                int y = py + ry * scale + sy;
                if (y < 0 || (uint32_t)y >= g_h) continue;
                volatile uint32_t *row = g_fb + (uint64_t)y * g_stride_px;
                for (int sx = 0; sx < scale; sx++) {
                    int x = px + rx * scale + sx;
                    if (x < 0 || (uint32_t)x >= g_w) continue;
                    row[x] = color;
                }
            }
        }
    }
}

void con_str(const char *s, int px, int py, int scale, uint32_t color)
{
    int x = px;
    for (; *s; s++) {
        con_glyph(glyph_index(*s), x, py, scale, color);
        x += 9 * scale;
    }
}

void con_hexrow(int row, const char *label, uint64_t value, uint32_t color)
{
    const int scale = 3;          /* 24px glyphs: legible in a phone photo */
    const int lh = 8 * scale + 6; /* line height */
    int py = 8 + row * lh;
    int x = 8;
    /* label */
    for (const char *s = label; *s; s++) {
        con_glyph(glyph_index(*s), x, py, scale, color);
        x += 9 * scale;
    }
    con_glyph(glyph_index(':'), x, py, scale, color); x += 9 * scale;
    con_glyph(glyph_index('>'), x, py, scale, color); x += 9 * scale;
    /* 16 hex nibbles, MSB first */
    static const char HEX[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        int nib = (int)((value >> (i * 4)) & 0xf);
        con_glyph(glyph_index(HEX[nib]), x, py, scale, color);
        x += 9 * scale;
    }
}

/* Auto-advancing log on top of con_hexrow. */
static int g_logrow;
void con_logreset(void) { g_logrow = 0; }
void con_log(const char *label, uint64_t value, uint32_t color)
{
    int maxrows = (int)(g_h / (8 * 3 + 6));   /* same line height as con_hexrow */
    if (maxrows < 1) maxrows = 1;
    con_hexrow(g_logrow % maxrows, label, value, color);
    g_logrow++;
}

void con_logstr(const char *s, uint32_t color)
{
    int maxrows = (int)(g_h / (8 * 3 + 6));
    if (maxrows < 1) maxrows = 1;
    int py = 8 + (g_logrow % maxrows) * (8 * 3 + 6);
    con_str(s, 8, py, 3, color);
    g_logrow++;
}
