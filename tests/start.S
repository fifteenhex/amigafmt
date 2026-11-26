/* entry for the C test programs: main(arglen, args) */
	.text
	.globl	_start
_start:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	4.w,%a6
	move.l	%a6,SysBase
	move.l	%a0,-(%sp)
	move.l	%d0,-(%sp)
	jsr	tmain
	addq.l	#8,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/* u32 amicall(struct R *r, void *base, int off): d0-d7/a0-a5 from *r,
   a6 = base, jsr off(a6); d0/d1 are stored back into *r */
	.globl	amicall
amicall:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%a6
	move.l	56(%sp),%d0
	lea	(%a6,%d0.l),%a1
	move.l	%a0,-(%sp)
	pea	1f(%pc)
	move.l	%a1,-(%sp)
	movem.l	(%a0),%d0-%d7/%a0-%a5
	rts
1:	move.l	(%sp)+,%a1
	move.l	%d0,(%a1)
	move.l	%d1,4(%a1)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.bss
	.globl	SysBase
	.balign	4
SysBase:
	.long	0
	.section .note.GNU-stack,"",%progbits
