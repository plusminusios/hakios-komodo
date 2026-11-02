/* host_loader_test.c — verify loader_prepare against a synthetic KOMOXNU
 * payload in a simulated physical RAM buffer. Build with -DLOADER_HOST. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "fb.h"
#include "loader.h"

void loader_set_host_ram(void *p, uint64_t base, uint64_t size);

/* ---- simulated physical RAM ---- */
#define RAM_BASE  0x80000000ULL
#define RAM_SIZE  0x4000000ULL      /* 64 MiB */
static uint8_t ram[RAM_SIZE];
static uint8_t *at(uint64_t pa) { return ram + (pa - RAM_BASE); }

/* ---- kernel image geometry (XNU-like) ---- */
#define VIRT_BASE 0xfffffe0007004000ULL
#define PHYS_BASE 0x80000000ULL
#define MEM_SIZE  0x1000000ULL       /* 16 MiB kernel window */
#define SEG0_SZ   0x4000
#define SEG1_SZ   0x4000
#define KERNEL_END (VIRT_BASE + 0x8000)
#define INITRD_PA  0x82000000ULL     /* above the kernel window (no overlap) */

/* payload header mirror (must match loader.c) */
#define BLL 608
struct pseg { uint64_t vmaddr, vmsize, file_off, file_size; };
struct phdr {
    char magic[8]; uint32_t version, nsegs;
    uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size;
    char cmdline[BLL];
    struct pseg segs[2];
};

/* ---- minimal FDT with /chosen initrd-start/-end ---- */
static void be32w(uint8_t *p, uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static int build_fdt(uint8_t *out, uint64_t rd_start, uint64_t rd_end)
{
    /* strings */
    const char *s1="linux,initrd-start", *s2="linux,initrd-end";
    uint8_t strs[64]; int so=0;
    int o1=so; memcpy(strs+so,s1,strlen(s1)+1); so+=strlen(s1)+1;
    int o2=so; memcpy(strs+so,s2,strlen(s2)+1); so+=strlen(s2)+1;
    /* struct block */
    uint8_t st[256]; int t=0;
    be32w(st+t,1);t+=4; st[t]=0;st[t+1]=0;st[t+2]=0;st[t+3]=0;t+=4;       /* BEGIN_NODE "" */
    be32w(st+t,1);t+=4; memcpy(st+t,"chosen\0\0",8);t+=8;                  /* BEGIN_NODE chosen */
    be32w(st+t,3);t+=4; be32w(st+t,8);t+=4; be32w(st+t,o1);t+=4;          /* PROP start len8 */
    be32w(st+t,(uint32_t)(rd_start>>32));t+=4; be32w(st+t,(uint32_t)rd_start);t+=4;
    be32w(st+t,3);t+=4; be32w(st+t,8);t+=4; be32w(st+t,o2);t+=4;          /* PROP end len8 */
    be32w(st+t,(uint32_t)(rd_end>>32));t+=4; be32w(st+t,(uint32_t)rd_end);t+=4;
    be32w(st+t,2);t+=4;                                                    /* END_NODE chosen */
    be32w(st+t,2);t+=4;                                                    /* END_NODE root */
    be32w(st+t,9);t+=4;                                                    /* FDT_END */
    /* assemble: header(40) + rsvmap(16) + struct + strings */
    int hs=40, rs=16;
    int off_struct=hs+rs, off_strings=off_struct+t, total=off_strings+so;
    memset(out,0,hs+rs);
    be32w(out+0,0xd00dfeed); be32w(out+4,total); be32w(out+8,off_struct);
    be32w(out+12,off_strings); be32w(out+16,hs); be32w(out+20,17);
    be32w(out+24,16); be32w(out+28,0); be32w(out+32,so); be32w(out+36,t);
    memcpy(out+off_struct,st,t); memcpy(out+off_strings,strs,so);
    return total;
}

static uint8_t fdt[512];
static uint32_t fbsmall[64*64];
static int fail;
static void chk(const char*w,int c){printf("  %-44s %s\n",w,c?"ok":"FAIL");if(!c)fail=1;}

int main(void)
{
    loader_set_host_ram(ram, RAM_BASE, RAM_SIZE);
    con_init((uint64_t)(uintptr_t)fbsmall, 64, 64, 64*4);  /* tiny fb: con_log clips */

    /* lay the payload at INITRD_PA */
    struct phdr *p = (struct phdr *)at(INITRD_PA);
    memset(p, 0, sizeof(*p));
    memcpy(p->magic, "KOMOXNU", 8);
    p->version = 1; p->nsegs = 2;
    p->virt_base = VIRT_BASE; p->entry_va = VIRT_BASE + 0x1000;
    p->kernel_end = KERNEL_END; p->phys_base = PHYS_BASE; p->mem_size = MEM_SIZE;
    strcpy(p->cmdline, "debug=0x8 -v komodo");
    uint64_t data_off = sizeof(struct phdr);          /* seg bytes after header */
    p->segs[0] = (struct pseg){ VIRT_BASE,          SEG0_SZ, data_off,           SEG0_SZ };
    p->segs[1] = (struct pseg){ VIRT_BASE + 0x4000, SEG1_SZ, data_off + SEG0_SZ, SEG1_SZ };
    uint64_t adt_off = data_off + SEG0_SZ + SEG1_SZ;
    p->adt_off = adt_off; p->adt_size = 0x40;
    /* fill seg bytes with recognizable patterns, adt with 0xAD */
    memset(at(INITRD_PA)+data_off, 0x11, SEG0_SZ);
    memset(at(INITRD_PA)+data_off+SEG0_SZ, 0x22, SEG1_SZ);
    memset(at(INITRD_PA)+adt_off, 0xAD, 0x40);
    /* /tegu-fbcon reg[0] placeholder at a 4-aligned (not 8-aligned) offset,
     * as in the real komodo.adt; the loader must replace it with fb.base. */
    { uint64_t ph = 0x4b4f4d4f46424144ULL; memcpy(at(INITRD_PA)+adt_off+0x14, &ph, 8); }
    uint64_t payload_len = adt_off + 0x40;

    build_fdt(fdt, INITRD_PA, INITRD_PA + payload_len);

    struct fb_info fb = { 0xfac00000ULL, KOMODO_FB_W, KOMODO_FB_H, KOMODO_FB_STRIDE, FB_SRC_FALLBACK };
    struct loader_result r;
    int rc = loader_prepare((uintptr_t)fdt, &fb, &r);
    printf("loader_prepare rc=%d error=%s\n", rc, r.error?r.error:"(none)");
    if (rc) { printf("RESULT: FAIL\n"); return 1; }

    uint64_t pa_of_va = PHYS_BASE - VIRT_BASE;
    uint64_t seg0_pa = VIRT_BASE + pa_of_va;          /* = PHYS_BASE */
    uint64_t seg1_pa = VIRT_BASE + 0x4000 + pa_of_va;
    uint64_t adt_pa  = KERNEL_END + pa_of_va;

    chk("seg0 landed at phys_base", seg0_pa == PHYS_BASE);
    chk("seg0 byte pattern 0x11", at(seg0_pa)[0]==0x11 && at(seg0_pa)[SEG0_SZ-1]==0x11);
    chk("seg1 byte pattern 0x22", at(seg1_pa)[0]==0x22 && at(seg1_pa)[SEG1_SZ-1]==0x22);
    chk("adt copied (0xAD)",      at(adt_pa)[0]==0xAD && at(adt_pa)[0x3f]==0xAD);
    { uint64_t got; memcpy(&got, at(adt_pa)+0x14, 8);
      chk("adt fb placeholder -> fb.base", got==0xfac00000ULL && r.adt_fb_patched==1);
      chk("adt bytes around placeholder intact", at(adt_pa)[0x13]==0xAD && at(adt_pa)[0x1c]==0xAD); }
    chk("result.nsegs==2",        r.nsegs==2);
    chk("result.adt_pa correct",  r.adt_pa==adt_pa);
    chk("entry_pa = entry_va+off", r.entry_pa == VIRT_BASE+0x1000+pa_of_va);

    /* boot_args checks (same struct layout the kernel reads) */
    struct {
        uint16_t Revision, Version; uint64_t virtBase, physBase, memSize, topOfKernelData;
        struct { uint64_t v_baseAddr,v_display,v_rowBytes,v_width,v_height,v_depth; } Video;
        uint32_t machineType; uint64_t deviceTreeP; uint32_t deviceTreeLength;
        char CommandLine[608]; uint64_t bootFlags, memSizeActual;
    } *ba = (void*)at(r.boot_args_pa);   /* NON-packed: matches XNU boot_args ABI */
    chk("ba.Revision==2", ba->Revision==2);
    chk("ba.Version==2",  ba->Version==2);
    chk("ba.virtBase",    ba->virtBase==VIRT_BASE);
    chk("ba.physBase",    ba->physBase==PHYS_BASE);
    chk("ba.memSize",     ba->memSize==MEM_SIZE);
    chk("ba.Video.width==1344",  ba->Video.v_width==KOMODO_FB_W);
    chk("ba.Video.height==2992", ba->Video.v_height==KOMODO_FB_H);
    chk("ba.Video.rowBytes==5376", ba->Video.v_rowBytes==KOMODO_FB_STRIDE);
    chk("ba.Video.baseAddr==fb",   ba->Video.v_baseAddr==0xfac00000ULL);
    chk("ba.deviceTreeP = adt VA",  ba->deviceTreeP==(adt_pa - pa_of_va));
    chk("ba.deviceTreeLength==0x40",ba->deviceTreeLength==0x40);
    chk("ba.CommandLine preserved", strcmp(ba->CommandLine,"debug=0x8 -v komodo")==0);
    chk("ba.topOfKernelData>ba_pa", r.top_of_kernel_data > r.boot_args_pa);

    printf("RESULT: %s\n", fail?"FAIL":"PASS");
    return fail;
}
