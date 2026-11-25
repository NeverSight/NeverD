// An exported ELFv1 descriptor with its own TOC and nonzero static chain.
.abiversion 1
.section ".opd","aw",@progbits
.p2align 3
.globl remote_other
.type remote_other,@function
remote_other:
.quad .Lcode,.TOC.@tocbase,.Lenvironment
.text
.p2align 2
.Lcode:
  ld 4,0(11)
  ld 5,.Lvalue@toc(2)
  add 4,4,5
  subf 3,3,4
  blr
.section ".toc","aw",@progbits
.p2align 3
.Lvalue: .quad 93
.data
.p2align 3
.Lenvironment: .quad 19
