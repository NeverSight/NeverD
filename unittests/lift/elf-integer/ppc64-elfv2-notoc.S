.abiversion 2
.text
.globl notoc_cross
.type notoc_cross,@function
notoc_cross:
.localentry notoc_cross,1
 mflr 0
 stdu 1,-32(1)
 std 0,48(1)
 addi 3,3,1
 bl remote_apply@notoc
 addi 3,3,19
 addi 1,1,32
 ld 0,16(1)
 mtlr 0
 blr
.size notoc_cross,.-notoc_cross
