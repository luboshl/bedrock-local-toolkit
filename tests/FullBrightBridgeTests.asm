; Reproduce the profiled producer epilogue without opening Minecraft.
.const
testVector DWORD 11223344h, 55667788h, 12345678h, 76543210h
.code
public InvokeFullBrightBridge, FullBrightContinueTest

InvokeFullBrightBridge PROC
    push rbp
    push rsi
    push rbx
    sub rsp,80h
    lea rbp,[rsp+60h]
    mov rsi,rdx
    mov rbx,r8
    movaps xmmword ptr [rsp+60h],xmm6
    movdqu xmm6,xmmword ptr [testVector]
    movaps xmmword ptr [rbp-10h],xmm6
    pxor xmm6,xmm6
    mov rax,1122334455667788h
    mov qword ptr [rbp-38h],rax
    mov r11,33445566778899AAh
    cmp rax,rax
    jmp rcx
InvokeFullBrightBridge ENDP

FullBrightContinueTest PROC
    mov qword ptr [rbx],rax
    mov qword ptr [rbx+8],rcx
    mov qword ptr [rbx+16],rdx
    mov qword ptr [rbx+24],r8
    mov qword ptr [rbx+32],r11
    mov qword ptr [rbx+40],rsi
    pushfq
    pop rax
    mov qword ptr [rbx+48],rax
    movdqu xmmword ptr [rbx+56],xmm6
    movaps xmm6,xmmword ptr [rsp+60h]
    add rsp,80h
    pop rbx
    pop rsi
    pop rbp
    ret
FullBrightContinueTest ENDP
END
