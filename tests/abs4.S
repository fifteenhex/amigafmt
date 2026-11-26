/* every way this program reads ExecBase must give the same value: with
   page 0 mapped these are plain reads, without it amigafmt emulates them */
	.text
start:
	movea.l	4.w,%a6
	move.l	%a6,%d7
	moveq	#0,%d6			/* failures */
	movea.l	4.l,%a5
	cmp.l	%a5,%d7
	beq.s	1f
	addq.l	#1,%d6
1:	move.l	4.w,%d0
	cmp.l	%d0,%d7
	beq.s	2f
	addq.l	#2,%d6
2:	suba.l	%a0,%a0
	move.l	4(%a0),%d1
	cmp.l	%d1,%d7
	beq.s	3f
	addq.l	#4,%d6
3:	move.l	4.w,-(%sp)
	move.l	(%sp)+,%d2
	cmp.l	%d2,%d7
	beq.s	4f
	addq.l	#8,%d6
4:	lea	4.w,%a1
	move.l	(%a1),%d3
	cmp.l	%d3,%d7
	beq.s	5f
	add.l	#16,%d6
5:	move.l	4.w,store
	cmp.l	store,%d7
	beq.s	6f
	add.l	#32,%d6
6:	move.l	%d6,%d0			/* 0 when all agree */
	rts
	.bss
store:	.long	0
	.section .note.GNU-stack,"",%progbits
