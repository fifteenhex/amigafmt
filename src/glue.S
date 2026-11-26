/*
 * The two crossings between the Amiga program and the C side.
 *
 * Library vector:  jmp stub  ->  stub: move.l #id,-(sp); jmp lvo_common
 *   The Amiga stack then holds [id][caller's return].  lvo_common saves
 *   d0-d7/a0-a6 under it (struct frame), switches to the native stack and
 *   calls amiga_dispatch(frame), which returns the frame to resume from
 *   (normally the same one; StackSwap() hands back one on the new stack).
 *
 * call_amiga_super(regs, pc): the same for Supervisor() functions.
 *
 * call_amiga(regs, pc): run Amiga code with every register from *regs
 *   (a7 included) and a return address pushed on its stack; on return
 *   store the registers back and return d0.  native_sp is the native
 *   stack the dispatcher runs C on; call_amiga sets it to its own frame
 *   and restores the outer value afterwards, so callbacks nest.
 */
	.text

	.globl	lvo_common
lvo_common:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	move.l	%sp,%a2
	move.l	native_sp,%sp
	move.l	%a2,-(%sp)
	jsr	amiga_dispatch
	move.l	%d0,%sp
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	addq.l	#4,%sp
	tst.l	%d0
	rts

	.globl	call_amiga
call_amiga:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	native_sp,-(%sp)
	move.l	%sp,native_sp
	move.l	52(%sp),%a0		/* regs */
	move.l	56(%sp),%a1		/* pc */
	move.l	60(%a0),%a2		/* the Amiga sp */
	move.l	#amiga_return,-(%a2)
	move.l	%a1,-(%a2)
	move.l	%a2,%sp
	movem.l	(%a0),%d0-%d7/%a0-%a6
	rts
/* the same with an exception frame (format 0) for Supervisor(): the
   function's RTE, emulated by the SIGILL handler, returns to amiga_return */
	.globl	call_amiga_super
call_amiga_super:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	native_sp,-(%sp)
	move.l	%sp,native_sp
	move.l	52(%sp),%a0
	move.l	56(%sp),%a1
	move.l	60(%a0),%a2
	move.w	#0,-(%a2)		/* format 0, vector 0 */
	move.l	#amiga_return,-(%a2)	/* PC */
	move.w	#0x2000,-(%a2)		/* SR: supervisor */
	move.l	%a1,-(%a2)
	move.l	%a2,%sp
	movem.l	(%a0),%d0-%d7/%a0-%a6
	rts
amiga_return:
	move.l	%a6,-(%sp)
	move.l	native_sp,%a6
	move.l	52(%a6),%a6
	movem.l	%d0-%d7/%a0-%a5,(%a6)
	move.l	(%sp)+,56(%a6)
	move.l	%sp,60(%a6)
	move.l	native_sp,%sp
	move.l	(%sp)+,native_sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.data
	.globl	native_sp
	.balign	4
native_sp:
	.long	0

	.section .note.GNU-stack,"",%progbits
