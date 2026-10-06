; Execute the actual emitted bridges with the game's observed stack alignment.
; Capture volatile integer/SIMD registers and flags after the bridge returns.
.const
testVector DWORD 11223344h, 55667788h, 12345678h, 76543210h
.code
public InvokeOwnBridge, InvokeDepthBridge, OwnContinueTest, OwnSkipTest, DepthContinueTest
public ClobberTrue, ClobberFalse

prepare MACRO
    push rbx
    push rdi
    push r12
    sub rsp,80h
    mov rbx,rdx
    mov r12,r8
    lea rdi,[rdx+8]
    mov rax,1122334455667788h
    mov r10,2233445566778899h
    mov r11,33445566778899AAh
    movdqu xmm0,xmmword ptr [testVector]
    movdqa xmm1,xmm0
    movdqa xmm2,xmm0
    movdqa xmm3,xmm0
    movdqa xmm4,xmm0
    movdqa xmm5,xmm0
ENDM

InvokeOwnBridge PROC
    prepare
    jmp rcx
InvokeOwnBridge ENDP

InvokeDepthBridge PROC
    prepare
    mov byte ptr [rsp+50h],7Ch
    cmp rax,rax
    jmp rcx
InvokeDepthBridge ENDP

OwnContinueTest PROC
    mov byte ptr [r9+160],1
    jmp capture
OwnContinueTest ENDP
OwnSkipTest PROC
    mov byte ptr [r9+160],0
    jmp capture
OwnSkipTest ENDP
DepthContinueTest PROC
    mov qword ptr [r9],rax
    mov al,byte ptr [rsp+50h]
    mov byte ptr [r9+160],al
    mov rax,qword ptr [r9]
    jmp capture
DepthContinueTest ENDP

capture:
    mov qword ptr [r9],rax
    mov qword ptr [r9+8],rcx
    mov qword ptr [r9+16],rdx
    mov qword ptr [r9+24],r8
    mov qword ptr [r9+32],r9
    mov qword ptr [r9+40],r10
    mov qword ptr [r9+48],r11
    pushfq
    pop rax
    mov qword ptr [r9+56],rax
    movdqu xmmword ptr [r9+64],xmm0
    movdqu xmmword ptr [r9+80],xmm1
    movdqu xmmword ptr [r9+96],xmm2
    movdqu xmmword ptr [r9+112],xmm3
    movdqu xmmword ptr [r9+128],xmm4
    movdqu xmmword ptr [r9+144],xmm5
    add rsp,80h
    pop r12
    pop rdi
    pop rbx
    ret

ClobberTrue PROC
    mov rax,rsp
    and eax,0Fh
    cmp eax,8
    jne badAlignment
    mov rcx,-1
    mov rdx,-1
    mov r8,-1
    mov r9,-1
    mov r10,-1
    mov r11,-1
    pxor xmm0,xmm0
    pxor xmm1,xmm1
    pxor xmm2,xmm2
    pxor xmm3,xmm3
    pxor xmm4,xmm4
    pxor xmm5,xmm5
    mov eax,1
    ret
badAlignment:
    mov eax,2
    ret
ClobberTrue ENDP
ClobberFalse PROC
    xor eax,eax
    ret
ClobberFalse ENDP
END
