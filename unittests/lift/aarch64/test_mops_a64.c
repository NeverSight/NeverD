/* AArch64 FEAT_MOPS set prologue: destination/count writeback and fill byte. */

#define DEFINE_SET_PROBE(name, mnemonic)                                       \
  __attribute__((naked, noinline)) void name(void) {                           \
    __asm__ volatile(mnemonic " [x0]!, x1!, x2\n\t"                            \
                              "ret"                                            \
                     :                                                         \
                     :                                                         \
                     : "x0", "x1", "memory", "cc");                            \
  }

DEFINE_SET_PROBE(setp_probe, "setp")
DEFINE_SET_PROBE(setpn_probe, "setpn")
DEFINE_SET_PROBE(setpt_probe, "setpt")
DEFINE_SET_PROBE(setptn_probe, "setptn")
