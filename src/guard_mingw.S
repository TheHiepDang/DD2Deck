    .text
    .globl dd2_guarded_call
    .seh_proc dd2_guarded_call
dd2_guarded_call:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movq %rcx, %rax
    movq %rdx, %rcx
.Lprotected:
    call *%rax
    nop
.Lend:
    addq $40, %rsp
    ret
.Lcaught:
    xorl %eax, %eax
    addq $40, %rsp
    ret
    .seh_handler __C_specific_handler, @except
    .seh_handlerdata
    .long 1
    .rva .Lprotected, .Lend, dd2_seh_filter, .Lcaught
    .text
    .seh_endproc
