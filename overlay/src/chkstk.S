/* Sonde de pile (comme celle de mingw) : touche chaque page quand une fonction
   réserve plus de 4 Ko de variables locales, pour que Windows agrandisse la pile proprement. */
    .text
    .globl ___chkstk_ms
___chkstk_ms:
    pushq %rcx
    pushq %rax
    cmpq $0x1000, %rax
    leaq 24(%rsp), %rcx
    jb 2f
1:
    subq $0x1000, %rcx
    orq $0, (%rcx)
    subq $0x1000, %rax
    cmpq $0x1000, %rax
    ja 1b
2:
    subq %rax, %rcx
    orq $0, (%rcx)
    popq %rax
    popq %rcx
    ret
