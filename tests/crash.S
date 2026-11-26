/* an illegal instruction: amigafmt must report it and return 20 */
	.text
start:
	nop
	illegal
	rts
	.section .note.GNU-stack,"",%progbits
