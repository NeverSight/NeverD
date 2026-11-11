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
