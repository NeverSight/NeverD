// Power ISA 3.1 signed D34, register and PC-relative addresses.
.abiversion 2
.machine power10
.text
.p2align 6
.globl eval
.type eval,@function
eval:
 paddi 5,3,-0x18001,0
 pstd 5,0(4),0
 pld 6,0(4),0
 paddi 7,0,0x100008001,0
 xor 6,6,7
 pstw 6,8(4),0
 plwa 7,8(4),0
 add 5,5,7
 plwz 6,8(4),0
 xor 5,5,6
 psth 5,12(4),0
 plha 6,12(4),0
 plhz 7,12(4),0
 xor 5,5,6
 add 5,5,7
 pstb 5,14(4),0
 plbz 3,14(4),0
 add 3,3,5
 paddi 5,0,slot@pcrel,1
 ld 6,0(5)
 pstd 3,slot@pcrel(0),1
 pld 5,seed@pcrel(0),1
 xor 3,3,6
 xor 3,3,5
 blr
.size eval,.-eval

.data
.p2align 3
.type slot,@object
slot:
 .quad 0
.size slot,.-slot
.type seed,@object
seed:
 .quad 0x87456321abcd0001
.size seed,.-seed
