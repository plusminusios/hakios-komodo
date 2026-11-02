/* host_test.c — render the komodo console to a PNG on the host, to verify the
 * font and layout offline (no phone). Build/run via test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"

static uint32_t fb[KOMODO_FB_W * KOMODO_FB_H];

/* minimal PNG writer (no zlib: stored/uncompressed deflate blocks) */
static unsigned crc_table[256];
static void crc_init(void){for(int n=0;n<256;n++){unsigned c=n;for(int k=0;k<8;k++)c=c&1?0xedb88320u^(c>>1):c>>1;crc_table[n]=c;}}
static unsigned crc(unsigned c,const unsigned char*b,size_t n){for(size_t i=0;i<n;i++)c=crc_table[(c^b[i])&0xff]^(c>>8);return c;}
static unsigned adler(const unsigned char*d,size_t n){unsigned a=1,b=0;for(size_t i=0;i<n;i++){a=(a+d[i])%65521;b=(b+a)%65521;}return (b<<16)|a;}
static void w32(FILE*f,unsigned v){unsigned char b[4]={v>>24,v>>16,v>>8,v};fwrite(b,1,4,f);}
static void chunk(FILE*f,const char*t,const unsigned char*d,size_t n){
    w32(f,(unsigned)n);unsigned c=crc(0xffffffffu,(const unsigned char*)t,4);c=crc(c,d,n);
    fwrite(t,1,4,f);fwrite(d,1,n,f);w32(f,c^0xffffffffu);
}
static void write_png(const char*path,uint32_t*px,int w,int h){
    crc_init();FILE*f=fopen(path,"wb");if(!f){perror("open");return;}
    fwrite("\x89PNG\r\n\x1a\n",1,8,f);
    unsigned char ihdr[13]={w>>24,w>>16,w>>8,w,h>>24,h>>16,h>>8,h,8,2,0,0,0};
    chunk(f,"IHDR",ihdr,13);
    size_t raw=(size_t)h*(1+w*3);unsigned char*rb=malloc(raw);size_t o=0;
    for(int y=0;y<h;y++){rb[o++]=0;for(int x=0;x<w;x++){uint32_t v=px[y*w+x];rb[o++]=(v>>16)&0xff;rb[o++]=(v>>8)&0xff;rb[o++]=v&0xff;}}
    /* zlib stream: 2-byte header + stored deflate blocks + adler32 */
    size_t cap=raw+raw/65535+64;unsigned char*z=malloc(cap);size_t zo=0;z[zo++]=0x78;z[zo++]=0x01;
    size_t p=0;while(p<raw){size_t n=raw-p;if(n>65535)n=65535;z[zo++]=(p+n>=raw)?1:0;z[zo++]=n&0xff;z[zo++]=n>>8;z[zo++]=~n&0xff;z[zo++]=(~n>>8)&0xff;memcpy(z+zo,rb+p,n);zo+=n;p+=n;}
    unsigned ad=adler(rb,raw);z[zo++]=ad>>24;z[zo++]=ad>>16;z[zo++]=ad>>8;z[zo++]=ad;
    chunk(f,"IDAT",z,zo);chunk(f,"IEND",NULL,0);fclose(f);free(rb);free(z);
}

static long nonblack(void){long n=0;for(long i=0;i<(long)KOMODO_FB_W*KOMODO_FB_H;i++)if((fb[i]&0xffffff))n++;return n;}

int main(int argc, char**argv)
{
    const char *out = argc>1?argv[1]:"komodo-console.png";
    con_init((uint64_t)(uintptr_t)fb, KOMODO_FB_W, KOMODO_FB_H, KOMODO_FB_STRIDE);
    con_fill(C_BLACK);
    /* Simulate a boot that reached stage 5, then print some register values. */
    con_stage(5, C_GREEN);
    con_hexrow(0, "FDT",  0x0000000080000000ULL, C_WHITE);
    con_hexrow(1, "GICD", 0x0000000010400000ULL, C_CYAN);
    con_hexrow(2, "ESR",  0x0000000096000010ULL, C_YELLOW);
    con_hexrow(3, "ELR",  0x00000000FD3FF000ULL, C_WHITE);
    con_hexrow(4, "KOMODO", 0x1344000029920000ULL, C_RED);
    printf("fb %ux%u, non-black pixels: %ld\n", KOMODO_FB_W, KOMODO_FB_H, nonblack());
    write_png(out, fb, KOMODO_FB_W, KOMODO_FB_H);
    printf("wrote %s\n", out);
    return nonblack() > 0 ? 0 : 1;
}
