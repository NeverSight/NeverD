/* Both encodings use the same four transfers. The source runner compares
 * all 16 surrounding bytes and the reloaded word for every address alignment. */
.globl mips_merge_store_word
.type mips_merge_store_word,@function
.ent mips_merge_store_word
mips_merge_store_word:
#ifdef __MIPSEL__
  swl $5,3($4)
  swr $5,0($4)
  lwl $2,3($4)
  lwr $2,0($4)
#else
  swl $5,0($4)
  swr $5,3($4)
  lwl $2,0($4)
  lwr $2,3($4)
#endif
  jr $31
  nop
.end mips_merge_store_word
.size mips_merge_store_word,.-mips_merge_store_word

#ifdef WIDE
.macro single_store name,store
.globl \name
.type \name,@function
.ent \name
\name:
  \store $5,0($4)
  jr $31
  or $2,$5,$0
.end \name
.size \name,.-\name
.endm
single_store mips_merge_store_left_double,sdl
single_store mips_merge_store_right_double,sdr

.globl mips_merge_store_double
.type mips_merge_store_double,@function
.ent mips_merge_store_double
mips_merge_store_double:
#ifdef __MIPSEL__
  sdl $5,7($4)
  sdr $5,0($4)
  ldl $2,7($4)
  ldr $2,0($4)
#else
  sdl $5,0($4)
  sdr $5,7($4)
  ldl $2,0($4)
  ldr $2,7($4)
#endif
  jr $31
  nop
.end mips_merge_store_double
.size mips_merge_store_double,.-mips_merge_store_double
#endif
