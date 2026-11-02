/* host_shim_test.c — run shim_main on the real komodo-base.dtb with a local
 * framebuffer, verify the discovered values, render the console to a PNG.
 * Build with -DSHIM_HOST (so shim_main returns instead of spinning). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "fb.h"

void shim_main(const void *fdt, uint64_t fb_override);

struct shim_report {
    uint32_t stage; uint64_t fdt_ptr; int fdt_valid;
    uint64_t mem_base, mem_size, gicd; int ncpus, cpus_disabled;
    struct fb_info fb;
};
extern struct shim_report g_shim_report;

static uint32_t fbbuf[KOMODO_FB_W * KOMODO_FB_H];

/* tiny PNG writer (shared shape with host_test.c) */
static unsigned ct[256];
static void ci(void){for(int n=0;n<256;n++){unsigned c=n;for(int k=0;k<8;k++)c=c&1?0xedb88320u^(c>>1):c>>1;ct[n]=c;}}
static unsigned cr(unsigned c,const unsigned char*b,size_t n){for(size_t i=0;i<n;i++)c=ct[(c^b[i])&0xff]^(c>>8);return c;}
static unsigned ad(const unsigned char*d,size_t n){unsigned a=1,b=0;for(size_t i=0;i<n;i++){a=(a+d[i])%65521;b=(b+a)%65521;}return(b<<16)|a;}
static void w32(FILE*f,unsigned v){unsigned char b[4]={v>>24,v>>16,v>>8,v};fwrite(b,1,4,f);}
static void ck(FILE*f,const char*t,const unsigned char*d,size_t n){w32(f,(unsigned)n);unsigned c=cr(0xffffffffu,(const unsigned char*)t,4);c=cr(c,d,n);fwrite(t,1,4,f);fwrite(d,1,n,f);w32(f,c^0xffffffffu);}
static void png(const char*p,uint32_t*px,int w,int h){ci();FILE*f=fopen(p,"wb");if(!f)return;fwrite("\x89PNG\r\n\x1a\n",1,8,f);unsigned char ih[13]={w>>24,w>>16,w>>8,w,h>>24,h>>16,h>>8,h,8,2,0,0,0};ck(f,"IHDR",ih,13);size_t raw=(size_t)h*(1+w*3);unsigned char*rb=malloc(raw);size_t o=0;for(int y=0;y<h;y++){rb[o++]=0;for(int x=0;x<w;x++){uint32_t v=px[y*w+x];rb[o++]=(v>>16)&0xff;rb[o++]=(v>>8)&0xff;rb[o++]=v&0xff;}}size_t cap=raw+raw/65535+64;unsigned char*z=malloc(cap);size_t zo=0;z[zo++]=0x78;z[zo++]=1;size_t pp=0;while(pp<raw){size_t n=raw-pp;if(n>65535)n=65535;z[zo++]=(pp+n>=raw)?1:0;z[zo++]=n&0xff;z[zo++]=n>>8;z[zo++]=~n&0xff;z[zo++]=(~n>>8)&0xff;memcpy(z+zo,rb+pp,n);zo+=n;pp+=n;}unsigned a=ad(rb,raw);z[zo++]=a>>24;z[zo++]=a>>16;z[zo++]=a>>8;z[zo++]=a;ck(f,"IDAT",z,zo);ck(f,"IEND",NULL,0);fclose(f);free(rb);free(z);}

int main(int argc, char**argv)
{
    const char *dtb = argc>1?argv[1]:"komodo-base.dtb";
    const char *out = argc>2?argv[2]:"komodo-shim.png";
    FILE *f=fopen(dtb,"rb"); if(!f){perror("dtb");return 2;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    void *buf=malloc(n); if(fread(buf,1,n,f)!=(size_t)n){perror("read");return 2;} fclose(f);

    shim_main(buf, (uint64_t)(uintptr_t)fbbuf);

    struct shim_report *r=&g_shim_report;
    printf("stage        = %u (expect 3)\n", r->stage);
    printf("fdt_valid    = %d (expect 1)\n", r->fdt_valid);
    printf("mem_base     = 0x%llx (expect 0x80000000)\n",(unsigned long long)r->mem_base);
    printf("gicd         = 0x%llx (expect 0x10400000)\n",(unsigned long long)r->gicd);
    printf("ncpus        = %d (expect 9 nodes)\n", r->ncpus);
    printf("fb.source    = %d (0=fallback: komodo has no fb node, expected)\n", r->fb.source);
    printf("fb.base      = 0x%llx  %ux%u stride %u\n",(unsigned long long)r->fb.base,r->fb.width,r->fb.height,r->fb.stride);

    png(out, fbbuf, KOMODO_FB_W, KOMODO_FB_H);
    printf("wrote %s\n", out);

    int ok = r->stage==3 && r->fdt_valid==1 && r->mem_base==0x80000000ULL
          && r->gicd==0x10400000ULL && r->fb.source==FB_SRC_FALLBACK
          && r->fb.width==KOMODO_FB_W && r->fb.height==KOMODO_FB_H;
    printf("RESULT: %s\n", ok?"PASS":"FAIL");
    return ok?0:1;
}
