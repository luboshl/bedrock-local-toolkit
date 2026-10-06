; Synthetic frames for the emitted bridges; no game process is opened.
.code
public InvokeAlwaysDayTime, AlwaysDayTimeContinue, InvokeAlwaysDayRender, AlwaysDayRenderContinue
InvokeAlwaysDayTime PROC
    push rbx
    sub rsp,80h
    mov rbx,rdx
    mov qword ptr [rsp+58h],r9
    movsxd rax,dword ptr [rbx]
    movss xmm2,dword ptr [rbx+4]
    mov r11,33445566778899AAh
    push r8
    jmp rcx
InvokeAlwaysDayTime ENDP
AlwaysDayTimeContinue PROC
    mov qword ptr [rbx+8],rax
    mov qword ptr [rbx+16],rcx
    mov qword ptr [rbx+24],r11
    movdqu xmmword ptr [rbx+32],xmm2
    add rsp,88h
    pop rbx
    ret
AlwaysDayTimeContinue ENDP
InvokeAlwaysDayRender PROC
    push rbx
    push rbp
    sub rsp,28h
    mov rbx,rdx
    lea rbp,[rdx-37E8h]
    mov r10,r8
    movups xmm0,xmmword ptr [rdx]
    mov rax,1122334455667788h
    mov r11,33445566778899AAh
    cmp rax,rax
    jmp rcx
InvokeAlwaysDayRender ENDP
AlwaysDayRenderContinue PROC
    movups xmmword ptr [r10],xmm0
    mov qword ptr [r10+16],rax
    mov qword ptr [r10+24],rcx
    mov qword ptr [r10+32],rdx
    mov qword ptr [r10+40],r8
    mov qword ptr [r10+48],r11
    pushfq
    pop rax
    mov qword ptr [r10+56],rax
    add rsp,28h
    pop rbp
    pop rbx
    ret
AlwaysDayRenderContinue ENDP
END
