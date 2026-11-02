/*
 * start.S — komodo shim entry (arm64 Linux Image, loaded by ABL via
 * `fastboot boot`). ABL follows the Linux boot protocol: x0 = FDT phys ptr,
 * x1..x3 = 0, MMU/caches off, running at EL2 (hyp) or EL1.
 *
 *   - save x0..x3 to boot_regs (debug: the shim prints them)
 *   - mask interrupts, turn MMU/caches off on the current EL
 *   - set up the stack, zero .bss
 *   - shim_main(fdt, 0)      (0 = discover the framebuffer from the FDT)
 */
	.section ".head.text","ax"
	.global _start
_start:
	b	_stext
	.long	0
	.quad	0
	.quad	_end - _start		/* image_size */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere (as stock GKI + tegu) */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"		/* arm64 Image magic */
	.long	0

	.section ".text"
_stext:
	msr	daifset, #0xf		/* mask D/A/I/F */

	/* Save what ABL handed us (x0=FDT, x1..x3 per protocol = 0). */
	adrp	x4, boot_regs
	add	x4, x4, :lo12:boot_regs
	stp	x0, x1, [x4]
	stp	x2, x3, [x4, #16]
	mrs	x5, CurrentEL
	str	x5, [x4, #32]

	mov	x19, x0			/* keep FDT */

	/* Turn MMU + caches off at whatever EL we entered. */
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #2
	b.eq	1f
	mrs	x1, sctlr_el1
	bic	x1, x1, #(1 << 0)	/* M  (MMU)   */
	bic	x1, x1, #(1 << 2)	/* C  (D$)    */
	bic	x1, x1, #(1 << 12)	/* I  (I$)    */
	msr	sctlr_el1, x1
	b	2f
1:	mrs	x1, sctlr_el2
	bic	x1, x1, #(1 << 0)
	bic	x1, x1, #(1 << 2)
	bic	x1, x1, #(1 << 12)
	msr	sctlr_el2, x1
2:	isb

	/* ABL ran with caches on: write back + invalidate every data/unified
	 * cache level by set/way, so no dirty ABL line can later be evicted on
	 * top of the kernel we copy with caches off. */
	bl	dcache_clean_inval_all

	/* Stack. */
	adrp	x1, _stack_top
	add	x1, x1, :lo12:_stack_top
	mov	sp, x1

	/* Zero .bss. */
	adrp	x1, _bss_start
	add	x1, x1, :lo12:_bss_start
	adrp	x2, _bss_end
	add	x2, x2, :lo12:_bss_end
3:	cmp	x1, x2
	b.hs	4f
	str	xzr, [x1], #8
	b	3b

4:	mov	x0, x19			/* fdt */
	mov	x1, #0			/* fb_override = 0 -> discover */
	bl	shim_main

	/* shim_main does not return; park if it ever does. */
5:	wfe
	b	5b

/*
 * dcache_clean_inval_all — DC CISW over all cache levels reported by CLIDR_EL1
 * (ARM ARM D7.x reference loop). Clobbers x0-x11. Safe at EL1 and EL2.
 */
	.global dcache_clean_inval_all
dcache_clean_inval_all:
	dsb	sy
	mrs	x0, clidr_el1
	and	x3, x0, #0x7000000
	lsr	x3, x3, #23		/* LoC * 2 */
	cbz	x3, 5f
	mov	x10, #0			/* level * 2 */
1:	add	x2, x10, x10, lsr #1	/* level * 3 */
	lsr	x1, x0, x2
	and	x1, x1, #7		/* cache type at this level */
	cmp	x1, #2
	b.lt	4f			/* no data cache here */
	msr	csselr_el1, x10
	isb
	mrs	x1, ccsidr_el1
	and	x2, x1, #7
	add	x2, x2, #4		/* log2(line bytes) */
	mov	x4, #0x3ff
	and	x4, x4, x1, lsr #3	/* max way */
	clz	w5, w4			/* way shift */
	mov	x7, #0x7fff
	and	x7, x7, x1, lsr #13	/* max set */
2:	mov	x9, x4
3:	lsl	x6, x9, x5
	orr	x11, x10, x6
	lsl	x6, x7, x2
	orr	x11, x11, x6
	dc	cisw, x11
	subs	x9, x9, #1
	b.ge	3b
	subs	x7, x7, #1
	b.ge	2b
4:	add	x10, x10, #2
	cmp	x3, x10
	b.gt	1b
5:	mov	x10, #0
	msr	csselr_el1, x10
	dsb	sy
	isb
	ret

/*
 * xnu_enter(entry_pa, boot_args_pa) — hand over to XNU at EL1, x0 = boot_args.
 * Pixel ABL starts kernels at EL2 (pKVM); XNU wants EL1 with no hypervisor
 * in the way. Same EL2 setup as the kernel's own secondary-CPU path (patch
 * 0065), plus VPIDR/VMPIDR so EL1 reads of MIDR/MPIDR return the real values.
 * VBAR_EL2 stays on the shim's vectors: anything that traps to EL2 later
 * paints the red fault screen instead of vanishing.
 */
	.global xnu_enter
xnu_enter:
	msr	daifset, #0xf
	mov	x19, x0
	mov	x20, x1
	mrs	x2, CurrentEL
	cmp	x2, #0x8
	b.ne	1f
	mov	x2, #(1 << 31)		/* HCR_EL2.RW: EL1 is AArch64; no VM, no traps */
	msr	hcr_el2, x2
	msr	vttbr_el2, xzr
	msr	cptr_el2, xzr
	msr	mdcr_el2, xzr
	msr	hstr_el2, xzr
	mov	x2, #3			/* EL1 physical counter + timer access */
	msr	cnthctl_el2, x2
	msr	cntvoff_el2, xzr
	mrs	x2, midr_el1
	msr	vpidr_el2, x2
	mrs	x2, mpidr_el1
	msr	vmpidr_el2, x2
	mrs	x2, S3_4_C12_C9_5	/* ICC_SRE_EL2 */
	orr	x2, x2, #0x1		/* SRE                                  */
	orr	x2, x2, #0x8		/* Enable: EL1 may use the GICv3 sysregs */
	msr	S3_4_C12_C9_5, x2
	movz	x2, #0x0800		/* SCTLR_EL1 = 0x30d00800: RES1 bits, */
	movk	x2, #0x30d0, lsl #16	/* M/C/I off, little endian          */
	msr	sctlr_el1, x2
	isb
	msr	elr_el2, x19
	mov	x2, #0x3c5		/* EL1h, DAIF masked */
	msr	spsr_el2, x2
	mov	x0, x20
	eret
1:	mov	x0, x20			/* already EL1: plain jump */
	br	x19

	.section ".data"
	.global boot_regs
	.align 3
boot_regs:
	.quad 0		/* x0 (FDT)   */
	.quad 0		/* x1         */
	.quad 0		/* x2         */
	.quad 0		/* x3         */
	.quad 0		/* CurrentEL  */
