| The Amiga side of a ported Mega Drive game: taking the machine over, the
| entry points the rewritten code calls (the layer's page is filled with jumps
| to these), and the vertical blank. Supervisor mode throughout, as the game
| ran on the Mega Drive. 68020 (movec, the exception frame's format word).
#include "port_layout.h"

	.text
	.globl	_port_go, _port_copy, _stub_table

| port_copy(src, dst, len): bytes (dst may be address 0)
_port_copy:
	move.l	4(sp),a0
	move.l	8(sp),a1
	move.l	12(sp),d0
1:	move.b	(a0)+,(a1)+
	subq.l	#1,d0
	bne.s	1b
	rts

| port_go: from Supervisor(); never returns
_port_go:
	move.w	#0x2700,sr
	lea	0xdff000,a6
	move.w	#0x7fff,0x9a(a6)
	move.w	#0x7fff,0x9c(a6)
	move.w	#0x7fff,0x96(a6)
	lea	_lstack_top,sp
	lea	_vectors,a0
	move.w	#255,d0
	lea	fault(pc),a1
1:	move.l	a1,(a0)+
	dbf	d0,1b
	lea	_vectors,a0
	lea	l3(pc),a1
	move.l	a1,0x6c(a0)
	lea	quiet(pc),a1                | the other interrupt levels: nothing of ours is enabled on them
	move.l	a1,0x64(a0)
	move.l	a1,0x68(a0)
	move.l	a1,0x74(a0)
	move.l	a1,0x78(a0)
	lea	l4(pc),a1                   | audio channel 0: the sound's tick
	move.l	a1,0x70(a0)
	lea	_vectors,a0
	movec	a0,vbr
	jsr	_port_place
	jsr	_layer_init
	jsr	_sound_init
	movec	cacr,d0
	or.w	#0x0808,d0                  | clear the caches: code was just copied
	movec	d0,cacr
	lea	0xdff000,a6
	move.w	#0x8380,0x96(a6)            | DMA: bitplanes and the copper
	move.w	#0xc070,0x9a(a6)            | interrupts: the vertical blank, the copper's, and the copper's other one (see l3)
	move.l	_port_sp,sp
	jmp	PORT_PC                     | (the ROM's reset vector: its old code is a jump to the new)

| an exception nothing handles: the border shows which (the vector's offset), and it stops
fault:
	move.w	#0x2700,sr
	movem.l	d0-d7/a0-a7,_fault_regs
	move.l	2(sp),a0                    | where
	moveq	#0,d2
	move.w	6(sp),d2                    | which
	lea	_istack_top,sp
	move.l	d2,-(sp)
	move.l	a0,-(sp)
	jsr	_layer_fault                | (said on the serial port)
	move.w	d2,d0
	and.w	#0x0fff,d0
1:	move.w	d0,0xdff180
	move.w	#0x0f00,0xdff180
	bra.s	1b
quiet:
	move.w	#0x3f9f,0xdff09c            | (everything but the vertical blank and the copper/blitter bits)
	rte

| ---- the layer's entry points. On entry: 4(sp) = the hardware address. Everything is preserved,
| the condition codes too; interrupts are off inside, on the layer's own stack.
	.macro	access name, fn, bits
\name:
	move.w	sr,-(sp)
	or.w	#0x0700,sr
	movem.l	d0-d1/a0-a1,-(sp)
	move.l	22(sp),d0
	move.l	sp,_saved_sp
	lea	_lstack_top,sp
	pea	\bits
	move.l	d0,-(sp)
	jsr	\fn
	move.l	_saved_sp,sp
	movem.l	(sp)+,d0-d1/a0-a1
	move.w	(sp)+,sr
	rts
	.endm
	access	stub_r8, _sh_r, 8
	access	stub_r16, _sh_r, 16
	access	stub_r32, _sh_r, 32
	access	stub_w8, _sh_w, 8
	access	stub_w16, _sh_w, 16
	access	stub_w32, _sh_w, 32

| fixaN: aN may hold a Mega Drive RAM address ($FFxxxxxx or $00FFxxxx, down to $E00000): bring it over
	.macro	fixa name, reg
\name:
	move.w	sr,-(sp)
	move.l	d0,-(sp)
	move.l	\reg,d0
	add.l	#0x00200000,d0
	cmp.l	#0x00200000,d0
	bcs.s	1f
	move.l	\reg,d0
	add.l	#0xff200000,d0
	cmp.l	#0x00200000,d0
	bcc.s	2f
1:	move.l	\reg,d0
	and.l	#0x0000ffff,d0
	add.l	_port_ram,d0
	move.l	d0,\reg
2:	move.l	(sp)+,d0
	move.w	(sp)+,sr
	rts
	.endm
	fixa	stub_fixa0, a0
	fixa	stub_fixa1, a1
	fixa	stub_fixa2, a2
	fixa	stub_fixa3, a3
	fixa	stub_fixa4, a4
	fixa	stub_fixa5, a5
	fixa	stub_fixa6, a6

_stub_table:                            | in agk port's SHIM_NAMES order
	.long	stub_r8, stub_r16, stub_r32, stub_w8, stub_w16, stub_w32
	.long	stub_fixa0, stub_fixa1, stub_fixa2, stub_fixa3, stub_fixa4, stub_fixa5, stub_fixa6
	.long	0

| ---- level 4: Paula has started on the next buffer, sixty times a second whatever the game is
| doing: the sound (sound.c) reads its stream and mixes the buffer after that
l4:
	movem.l	d0-d1/a0-a1,-(sp)
	move.w	#0x0780,0xdff09c
	move.w	#0x0780,0xdff09c
	jsr	_sound_tick
	movem.l	(sp)+,d0-d1/a0-a1
	rte

| ---- level 3: the vertical blank. The layer's part, then the game's own handler (the vector the
| ROM has for the Mega Drive's level 6), which returns through `post`. The picture is drawn while
| the game waits for its next interrupt (the copper's question, below); in `post` only when it
| hasn't waited for some frames.
l3:
	movem.l	d0-d1/a0-a1,-(sp)
	move.w	0xdff01e,d0
	btst	#5,d0
	bne	1f
	btst	#6,d0
	beq.s	4f
	move.w	#0x0040,0xdff09c            | the copper asks every sixteen lines (with the blitter's bit: nothing else uses it)
	move.w	#0x0040,0xdff09c            | whether the game is waiting: the layer draws while it is
	move.l	18(sp),d0                   | (where it was interrupted)
	move.l	sp,_irq_saved_sp
	lea	_istack_top,sp
	move.l	d0,-(sp)
	jsr	_layer_idle
	move.l	_irq_saved_sp,sp
	bra	6f
4:	move.w	#0x0010,0xdff09c            | the copper's: the line the game asked its horizontal interrupt for
	move.w	#0x0010,0xdff09c
	btst	#4,d0
	beq.s	6f
	sf	PORT_SHIM+16                | (the game gets this one: it is awake)
	jsr	_layer_irq_enter
	movem.l	(sp)+,d0-d1/a0-a1
	move.l	PORT_SHIM,-(sp)
	pea	hint_done(pc)
	move.w	#0x2300,-(sp)
	move.l	0x70.w,-(sp)                | the game's handler (the Mega Drive's level 4)
	rts
hint_done:
	move.l	(sp)+,PORT_SHIM
	movem.l	d0-d1/a0-a1,-(sp)
	jsr	_layer_irq_leave
	| (the game may have been waiting for this very interrupt, and is still in its loop: a question
	| the copper asked meanwhile would find it there and keep it from leaving for another frame)
	move.w	#0x0040,0xdff09c
	move.w	#0x0040,0xdff09c
	bra.s	3f
	| an interrupt of the Amiga's that the game didn't get: if it was waiting in a `stop`, it goes on
	| waiting (the Mega Drive would not have woken it). Asleep is the byte agk port sets before each
	| stop and an interrupt the game gets clears: the return address alone is the same just after a wake
6:	move.l	18(sp),a0
	cmp.w	#0x4e72,-4(a0)
	bne.s	3f
	tst.b	PORT_SHIM+16
	beq.s	3f
	subq.l	#4,18(sp)
3:	movem.l	(sp)+,d0-d1/a0-a1
	rte
1:	move.w	#0x0020,0xdff09c
	move.w	#0x0020,0xdff09c
	move.l	18(sp),-(sp)                | (where the game was)
	jsr	_layer_vbl
	addq.l	#4,sp
	tst.l	d0
	movem.l	(sp)+,d0-d1/a0-a1
	bne.s	7f
	move.l	a0,-(sp)                    | (its interrupt is off: a `stop` goes on waiting)
	move.l	6(sp),a0
	cmp.w	#0x4e72,-4(a0)
	bne.s	8f
	tst.b	PORT_SHIM+16
	beq.s	8f
	subq.l	#4,6(sp)
8:	move.l	(sp)+,a0
	bra.s	post
7:	sf	PORT_SHIM+16                | (awake)
	move.l	PORT_SHIM,-(sp)             | the cell: the interrupted code may be between an instruction and its call
	pea	post_cell(pc)                | a frame for the game's return, the 68000's (agk port turns its rte into move (sp)+,sr; rts)
	move.w	#0x2300,-(sp)
	move.l	0x78.w,-(sp)                | the game's handler
	rts
post_cell:
	move.l	(sp)+,PORT_SHIM
post:
	movem.l	d0-d1/a0-a1,-(sp)
	move.l	sp,_irq_saved_sp
	lea	_istack_top,sp
	jsr	_layer_frame                | the display for this frame, the cells and the sprites
	move.l	_irq_saved_sp,sp
	move.w	#0x0040,0xdff09c            | (as above: the game is given the time to leave its loop)
	move.w	#0x0040,0xdff09c
	movem.l	(sp)+,d0-d1/a0-a1
	rte

	.bss
	.globl	_vectors, _lstack_top, _istack_top, _saved_sp, _irq_saved_sp, _fault_regs
_vectors:	.space	1024
_saved_sp:	.space	4
_fault_regs:	.space	64
_irq_saved_sp:	.space	4
	.space	8192
_lstack_top:
	.space	16384
_istack_top:
