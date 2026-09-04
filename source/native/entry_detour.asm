option casemap:none

EXTERN MfgUnlockDispatchForwarding:PROC

.code

; Each forwarding detour enters through a private relay which places its Slot
; address in r10. Preserve the four ABI register arguments, pass the Slot and
; the original caller return address to C++, then tail-jump through that exact
; entry's trampoline. Because this is a tail jump, NVIDIA still observes the
; original _nvngx/provider caller rather than this module.
PUBLIC MfgUnlockForwardDispatch
MfgUnlockForwardDispatch PROC FRAME
    sub rsp, 88h
    .allocstack 88h
    .endprolog
    mov QWORD PTR [rsp + 40h], rcx
    mov QWORD PTR [rsp + 48h], rdx
    mov QWORD PTR [rsp + 50h], r8
    mov QWORD PTR [rsp + 58h], r9
    mov rax, QWORD PTR [rsp + 0B0h]
    mov QWORD PTR [rsp + 20h], rax
    mov rax, QWORD PTR [rsp + 0B8h]
    mov QWORD PTR [rsp + 28h], rax
    mov QWORD PTR [rsp + 30h], r10
    mov rax, QWORD PTR [rsp + 88h]
    mov QWORD PTR [rsp + 38h], rax
    call MfgUnlockDispatchForwarding
    mov r11, rax
    mov rcx, QWORD PTR [rsp + 40h]
    mov rdx, QWORD PTR [rsp + 48h]
    mov r8, QWORD PTR [rsp + 50h]
    mov r9, QWORD PTR [rsp + 58h]
    add rsp, 88h
    jmp r11
MfgUnlockForwardDispatch ENDP

END
