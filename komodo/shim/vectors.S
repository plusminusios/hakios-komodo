/*
 * vectors.S — таблица векторов исключений AArch64.
 *
 * 16 входов по 0x80 байт, база выровнена на 0x800. Каждый вход кладёт тип в
 * x18 и прыгает в общий обработчик, который читает ESR/ELR/FAR текущего EL и
 * зовёт exc_handler(esr, elr, far, type). Так фолты перестают «уходить в никуда»
 * (как в Phase-1) и становятся фиксируемыми.
 *
 * Мы НЕ возвращаемся из исключения (halt), поэтому полный контекст не сохраняем;
 * x18 можно затирать.
 */

	.section ".text"
	.macro ventry type
	.align 7
	mov	x18, #\type
	b	exc_common
	.endm

	.align 11
	.global vector_table
vector_table:
	/* Current EL, SP_EL0 */
	ventry 0	/* sync */
	ventry 1	/* irq  */
	ventry 2	/* fiq  */
	ventry 3	/* serror */
	/* Current EL, SP_ELx */
	ventry 4
	ventry 5
	ventry 6
	ventry 7
	/* Lower EL, AArch64 */
	ventry 8
	ventry 9
	ventry 10
	ventry 11
	/* Lower EL, AArch32 */
	ventry 12
	ventry 13
	ventry 14
	ventry 15

exc_common:
	/* выбрать регистры текущего EL */
	mrs	x9, CurrentEL
	lsr	x9, x9, #2
	cmp	x9, #2
	b.eq	1f
	mrs	x0, esr_el1
	mrs	x1, elr_el1
	mrs	x2, far_el1
	b	2f
1:	mrs	x0, esr_el2
	mrs	x1, elr_el2
	mrs	x2, far_el2
2:	mov	x3, x18
	bl	exc_handler
3:	wfe
	b	3b

	/* Установить VBAR текущего EL на vector_table. */
	.global exc_install_vectors
exc_install_vectors:
	adrp	x0, vector_table
	add	x0, x0, :lo12:vector_table
	mrs	x9, CurrentEL
	lsr	x9, x9, #2
	cmp	x9, #2
	b.eq	1f
	msr	vbar_el1, x0
	b	2f
1:	msr	vbar_el2, x0
2:	isb
	ret
