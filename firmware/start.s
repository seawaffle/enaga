; Enaga entry, interrupt vectors and the interrupt context switch (C674x, SPRUFE8B chapter 5).
;
; The bootloader has copied the image to its load address by EDMA and branches to _start with DDR running; it
; leaves L2 all RAM, L1D half cache and part of DDR uncached, and does no cache maintenance (docs/hardware.md
; "Boot"). _start sets the stack, keeps interrupts off, records the cache registers, writes back
; and invalidates L1D and invalidates L1P, and calls main, which sets the caches (hal/cache.c); the kernel points
; ISTP at `vectors` and enables what it uses (kernel/irq.c).
;
; Interrupt stack gap: each maskable vector first moves SP 16 KiB down, so nothing it saves lands in the
; 16 KiB below the interrupted stack pointer. GCC 13.3 at -O2 can release a function's frame (raise SP) a few
; cycles before the function's last reads of a local array in it (tests/apps/stack); an interrupt there would
; write over the array. Building GCC code with -Werror=frame-larger-than=16384 (no VLAs or alloca) keeps every
; frame inside the gap. Found by the smpltrek-re emulator work, 2026-11-17.
;
; Each maskable vector then pushes B0/B1, loads the interrupt number into B1 and branches to isr_common, which
; saves every register the C calling convention lets enaga_irq clobber (A0-A9, A16-A31, B0-B9, B16-B31) plus
; ILC/RILC (SPLOOP state), calls enaga_irq(number) with interrupts disabled, restores, and returns through
; IRP. The one nesting is inside the app's audio callback (irq_nest_begin/end, kernel/audio.c).

	.global _start
	.global _halt
	.global vectors
	.global tsc_enable
	.global tsc_read
	.global irq_set_istp
	.global irq_enable_mask
	.global irq_global_enable
	.global irq_global_disable
	.global irq_resume_at
	.global restart_at

	.text
_start:
	mvkl .S2 _stack_top - 8, b15    ; the C6000 ABI lets a callee store at *SP: keep SP inside the stack
	mvkh .S2 _stack_top - 8, b15
	and .L2 -8, b15, b15            ; SP 8-byte aligned (C6000 EABI)
	mvc .S2 csr, b0
	and .L2 -2, b0, b0              ; GIE off until the kernel has its handlers in place
	mvc .S2 b0, csr
	mvkl .S1 cache_at_entry, a4     ; the cache registers as the bootloader left them, for the log (hal/cache.c)
	mvkh .S1 cache_at_entry, a4
	mvkl .S2 0x01840000, b4
	mvkh .S2 0x01840000, b4
	ldw .D2T2 *b4, b0               ; L2CFG
	ldw .D2T2 *+b4[8], b1           ; L1PCFG (+0x20)
	ldw .D2T2 *+b4[16], b2          ; L1DCFG (+0x40)
	nop 4
	stw .D1T2 b0, *a4++
	stw .D1T2 b1, *a4++
	stw .D1T2 b2, *a4++
	mvkl .S2 0x01848300, b4         ; MAR 192-199
	mvkh .S2 0x01848300, b4
	mvk .S1 8, a1
mar_copy:
	ldw .D2T2 *b4++, b0
	sub .L1 a1, 1, a1
	nop 3
	stw .D1T2 b0, *a4++
	[a1] b .S1 mar_copy
	nop 5
	mvkl .S2 0x01845044, b4         ; L1DWBINV: nothing L1D held before the jump is relied on (the image came
	mvkh .S2 0x01845044, b4         ; by EDMA, behind the caches)
	mvk .S2 1, b0
	stw .D2T2 b0, *b4
l1d_wait:
	ldw .D2T2 *b4, b0
	nop 4
	and .L2 1, b0, b0
	[b0] b .S2 l1d_wait
	nop 5
	mvkl .S2 0x01845028, b4         ; L1PINV: from here on, code is fetched from what DDR now holds
	mvkh .S2 0x01845028, b4
	mvk .S2 1, b0
	stw .D2T2 b0, *b4
l1p_wait:
	ldw .D2T2 *b4, b0
	nop 4
	and .L2 1, b0, b0
	[b0] b .S2 l1p_wait
	nop 5
	mvkl .S1 main, a3
	mvkh .S1 main, a3
	b .S2X a3
	mvkl .S2 _halt, b3
	mvkh .S2 _halt, b3
	nop 3
_halt:
	b .S2 _halt
	nop 5

; The time-stamp counter: free-running at the CPU clock (372 MHz) once written.
tsc_enable:
	mvk .S2 0, b0
	mvc .S2 b0, tscl
	b .S2 b3
	nop 5
tsc_read:
	mvc .S2 tscl, b0
	b .S2 b3
	mv .L1X b0, a4
	nop 4

; void irq_set_istp(u32 table): ISTP holds the 1 KiB-aligned vector table address.
irq_set_istp:
	mvc .S2X a4, istp
	b .S2 b3
	nop 5

; void irq_enable_mask(u32 mask): IER |= mask | NMIE (maskable interrupts need NMIE, SPRUFE8B 5.2.1).
irq_enable_mask:
	mvc .S2 ier, b0
	or .L2X b0, a4, b0
	or .L2 2, b0, b0
	mvc .S2 b0, ier
	b .S2 b3
	nop 5

irq_global_enable:
	mvc .S2 csr, b0
	or .L2 1, b0, b0
	mvc .S2 b0, csr
	b .S2 b3
	nop 5

; u32 irq_global_disable(void): clears GIE, returns the previous CSR GIE bit.
irq_global_disable:
	mvc .S2 csr, b0
	and .L2 -2, b0, b1
	mvc .S2 b1, csr
	b .S2 b3
	and .L2 1, b0, b0
	mv .L1X b0, a4
	nop 3

; void irq_resume_at(void (*fn)(void), u32 sp): leave interrupt context into fn on a fresh stack. Used when an
; app fails inside the audio callback: B IRP restores TSR from ITSR (so GIE comes back) and continues at fn.
irq_resume_at:
	mvc .S2X a4, irp
	mv .L2 b4, b15
	b .S2 irp
	nop 5

; void restart_at(void (*fn)(void), u32 sp): continue at fn on a fresh stack (main context).
restart_at:
	mv .L2 b4, b15
	b .S2X a4
	nop 5

; void irq_nest_begin(u32 saved[3], u32 mask): inside a handler, let the interrupts in mask nest (SPRUFE8B
; 5.6.2): IRP, ITSR and IER go to saved[0..2] (a nested interrupt overwrites IRP and ITSR), IER becomes
; mask | NMIE, then GIE is set.
	.global irq_nest_begin
	.global irq_nest_end
irq_nest_begin:
	mvc .S2 irp, b0
	stw .D1T2 b0, *a4
	mvc .S2 itsr, b0
	stw .D1T2 b0, *+a4[1]
	mvc .S2 ier, b0
	stw .D1T2 b0, *+a4[2]
	or .L2 2, b4, b0
	mvc .S2 b0, ier
	mvc .S2 csr, b0
	or .L2 1, b0, b0
	mvc .S2 b0, csr
	b .S2 b3
	nop 5

; void irq_nest_end(const u32 saved[3]): GIE off, then IRP, ITSR and IER as irq_nest_begin found them.
irq_nest_end:
	mvc .S2 csr, b0
	and .L2 -2, b0, b0
	mvc .S2 b0, csr
	ldw .D1T2 *a4, b0
	ldw .D1T2 *+a4[1], b1
	ldw .D1T2 *+a4[2], b2
	nop 4
	mvc .S2 b0, irp
	mvc .S2 b1, itsr
	mvc .S2 b2, ier
	b .S2 b3
	nop 5

; Interrupt vector table: 16 fetch packets of 32 bytes, 1 KiB aligned.
	.section .vectors,"ax"
	.p2align 10
vectors:
	b .S2 _start                    ; RESET
	nop 5
	nop
	nop
	nop
	nop
	nop
	nop
	.p2align 5
	b .S2 nmi_entry                 ; NMI / exception
	nop 5
	nop
	nop
	nop
	nop
	nop
	nop
	.p2align 5
	b .S2 _halt                     ; reserved
	nop 5
	nop
	nop
	nop
	nop
	nop
	nop
	.p2align 5
	b .S2 _halt                     ; reserved
	nop 5
	nop
	nop
	nop
	nop
	nop
	nop

	.macro VECTOR number
	.p2align 5
	addk .S2 -16384, b15            ; skip the gap below the interrupted SP (see "Interrupt stack gap" above)
	stw .D2T2 b1, *b15--[2]
	stw .D2T2 b0, *b15--[2]
	mvkl .S2 isr_common, b0
	mvkh .S2 isr_common, b0
	b .S2 b0
	mvk .S2 \number, b1
	nop 4
	.endm

	VECTOR 4
	VECTOR 5
	VECTOR 6
	VECTOR 7
	VECTOR 8
	VECTOR 9
	VECTOR 10
	VECTOR 11
	VECTOR 12
	VECTOR 13
	VECTOR 14
	VECTOR 15

	.text
; Frame: 216 bytes. Words 0-1 stay unused: the C6000 ABI lets a callee store at *SP (GCC prologues do),
; so nothing live may sit there across the call. ILC/RILC at words 2-3, the registers at 4-53, and the
; vector's B0/B1 pushes above the frame.
isr_common:
	addk .S2 -216, b15
	stw .D2T1 a0, *+b15[4]
	stw .D2T1 a1, *+b15[5]
	stw .D2T1 a2, *+b15[6]
	stw .D2T1 a3, *+b15[7]
	stw .D2T1 a4, *+b15[8]
	stw .D2T1 a5, *+b15[9]
	stw .D2T1 a6, *+b15[10]
	stw .D2T1 a7, *+b15[11]
	stw .D2T1 a8, *+b15[12]
	stw .D2T1 a9, *+b15[13]
	stw .D2T1 a16, *+b15[14]
	stw .D2T1 a17, *+b15[15]
	stw .D2T1 a18, *+b15[16]
	stw .D2T1 a19, *+b15[17]
	stw .D2T1 a20, *+b15[18]
	stw .D2T1 a21, *+b15[19]
	stw .D2T1 a22, *+b15[20]
	stw .D2T1 a23, *+b15[21]
	stw .D2T1 a24, *+b15[22]
	stw .D2T1 a25, *+b15[23]
	stw .D2T1 a26, *+b15[24]
	stw .D2T1 a27, *+b15[25]
	stw .D2T1 a28, *+b15[26]
	stw .D2T1 a29, *+b15[27]
	stw .D2T1 a30, *+b15[28]
	stw .D2T1 a31, *+b15[29]
	stw .D2T2 b2, *+b15[30]
	stw .D2T2 b3, *+b15[31]
	stw .D2T2 b4, *+b15[32]
	stw .D2T2 b5, *+b15[33]
	stw .D2T2 b6, *+b15[34]
	stw .D2T2 b7, *+b15[35]
	stw .D2T2 b8, *+b15[36]
	stw .D2T2 b9, *+b15[37]
	stw .D2T2 b16, *+b15[38]
	stw .D2T2 b17, *+b15[39]
	stw .D2T2 b18, *+b15[40]
	stw .D2T2 b19, *+b15[41]
	stw .D2T2 b20, *+b15[42]
	stw .D2T2 b21, *+b15[43]
	stw .D2T2 b22, *+b15[44]
	stw .D2T2 b23, *+b15[45]
	stw .D2T2 b24, *+b15[46]
	stw .D2T2 b25, *+b15[47]
	stw .D2T2 b26, *+b15[48]
	stw .D2T2 b27, *+b15[49]
	stw .D2T2 b28, *+b15[50]
	stw .D2T2 b29, *+b15[51]
	stw .D2T2 b30, *+b15[52]
	stw .D2T2 b31, *+b15[53]
	mvc .S2 ilc, b0
	stw .D2T2 b0, *+b15[2]
	mvc .S2 rilc, b0
	stw .D2T2 b0, *+b15[3]
	mvkl .S1 enaga_irq, a0
	mvkh .S1 enaga_irq, a0
	b .S2X a0
	mv .L1X b1, a4                  ; the interrupt number
	addkpc .S2 isr_return, b3, 3
isr_return:
	ldw .D2T2 *+b15[2], b0
	ldw .D2T2 *+b15[3], b1
	nop 4
	mvc .S2 b0, ilc
	mvc .S2 b1, rilc
	ldw .D2T1 *+b15[4], a0
	ldw .D2T1 *+b15[5], a1
	ldw .D2T1 *+b15[6], a2
	ldw .D2T1 *+b15[7], a3
	ldw .D2T1 *+b15[8], a4
	ldw .D2T1 *+b15[9], a5
	ldw .D2T1 *+b15[10], a6
	ldw .D2T1 *+b15[11], a7
	ldw .D2T1 *+b15[12], a8
	ldw .D2T1 *+b15[13], a9
	ldw .D2T1 *+b15[14], a16
	ldw .D2T1 *+b15[15], a17
	ldw .D2T1 *+b15[16], a18
	ldw .D2T1 *+b15[17], a19
	ldw .D2T1 *+b15[18], a20
	ldw .D2T1 *+b15[19], a21
	ldw .D2T1 *+b15[20], a22
	ldw .D2T1 *+b15[21], a23
	ldw .D2T1 *+b15[22], a24
	ldw .D2T1 *+b15[23], a25
	ldw .D2T1 *+b15[24], a26
	ldw .D2T1 *+b15[25], a27
	ldw .D2T1 *+b15[26], a28
	ldw .D2T1 *+b15[27], a29
	ldw .D2T1 *+b15[28], a30
	ldw .D2T1 *+b15[29], a31
	ldw .D2T2 *+b15[30], b2
	ldw .D2T2 *+b15[31], b3
	ldw .D2T2 *+b15[32], b4
	ldw .D2T2 *+b15[33], b5
	ldw .D2T2 *+b15[34], b6
	ldw .D2T2 *+b15[35], b7
	ldw .D2T2 *+b15[36], b8
	ldw .D2T2 *+b15[37], b9
	ldw .D2T2 *+b15[38], b16
	ldw .D2T2 *+b15[39], b17
	ldw .D2T2 *+b15[40], b18
	ldw .D2T2 *+b15[41], b19
	ldw .D2T2 *+b15[42], b20
	ldw .D2T2 *+b15[43], b21
	ldw .D2T2 *+b15[44], b22
	ldw .D2T2 *+b15[45], b23
	ldw .D2T2 *+b15[46], b24
	ldw .D2T2 *+b15[47], b25
	ldw .D2T2 *+b15[48], b26
	ldw .D2T2 *+b15[49], b27
	ldw .D2T2 *+b15[50], b28
	ldw .D2T2 *+b15[51], b29
	ldw .D2T2 *+b15[52], b30
	ldw .D2T2 *+b15[53], b31
	addk .S2 216, b15
	ldw .D2T2 *++b15[2], b0         ; pushed second by the vector
	ldw .D2T2 *++b15[2], b1
	b .S2 irp
	addk .S2 16384, b15             ; the interrupted SP again
	nop 4

; NMI and exceptions (SPRUFE8B chapter 6; bit values as SYS/BIOS ti/sysbios/family/c64p/Exception.xdc).
; The faulting context is never resumed, so only what the report needs is saved: NRP, EFR, IERR, NTSR, the
; stack pointer and B3 into exception_record (B0/B1 are pushed on the old stack first to free them). Then a
; dedicated stack and enaga_exception (kernel/exception.c), which drops a faulting app or shows the kernel
; crash screen; it does not return.
nmi_entry:
	stw .D2T2 b0, *b15--[2]
	stw .D2T2 b1, *b15--[2]
	mvkl .S2 exception_record, b0
	mvkh .S2 exception_record, b0
	mvc .S2 nrp, b1
	stw .D2T2 b1, *+b0[0]
	mvc .S2 efr, b1
	stw .D2T2 b1, *+b0[1]
	mvc .S2 ierr, b1
	stw .D2T2 b1, *+b0[2]
	mvc .S2 ntsr, b1
	stw .D2T2 b1, *+b0[3]
	addk .S2 16, b15                ; the stack pointer before the two pushes
	stw .D2T2 b15, *+b0[4]
	stw .D2T2 b3, *+b0[5]
	mvkl .S2 exception_stack + 8184, b15   ; the last 8-byte slot of the 8 KiB stack (*SP is writable)
	mvkh .S2 exception_stack + 8184, b15
	mvkl .S1 enaga_exception, a0
	mvkh .S1 enaga_exception, a0
	b .S2X a0
	nop 5
	b .S2 _halt                     ; not reached
	nop 5

; void exception_resume(void (*fn)(void), u32 sp): leave exception context into fn on a fresh stack. EFR and
; IERR are cleared (ECR, IERR = 0); B NRP restores TSR from NTSR and sets NMIE again.
	.global exception_resume
exception_resume:
	mvc .S2 efr, b0
	mvc .S2 b0, ecr
	mvk .S2 0, b0
	mvc .S2 b0, ierr
	mvc .S2X a4, nrp
	mv .L2 b4, b15
	b .S2 nrp
	nop 5

; u32 irp_read(void): the IRP register (the return address of the interrupt being handled).
	.global irp_read
irp_read:
	mvc .S2 irp, b0
	b .S2 b3
	mv .L1X b0, a4
	nop 4

; void exceptions_enable(void): TSR.GEE, so internal and software exceptions are taken (external ones, XEN,
; stay off: no event is routed to EXCEP).
	.global exceptions_enable
exceptions_enable:
	mvc .S2 tsr, b0
	or .L2 4, b0, b0
	mvc .S2 b0, tsr
	b .S2 b3
	nop 5
