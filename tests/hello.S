/* the smallest useful AmigaOS program: Write() to Output() */
	.text
start:
	move.l	4.w,%a6
	lea	dosname(%pc),%a1
	moveq	#36,%d0
	jsr	-552(%a6)		/* OpenLibrary */
	tst.l	%d0
	beq.s	1f
	move.l	%d0,%a6
	jsr	-60(%a6)		/* Output */
	move.l	%d0,%d1
	lea	msg(%pc),%a0
	move.l	%a0,%d2
	moveq	#msgend-msg,%d3
	jsr	-48(%a6)		/* Write */
	moveq	#0,%d0
	rts
1:	moveq	#20,%d0
	rts
dosname:
	.asciz	"dos.library"
msg:
	.ascii	"Hello, World!\n"
msgend:
	.section .note.GNU-stack,"",%progbits
