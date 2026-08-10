//===- X86_32_BcdRoundTripTests.cpp - i386 BCD adjust roundtrip -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the 32-bit-only BCD adjust family (AAA/AAS/AAD/AAM/
// DAA/DAS).  These were lifted as a placeholder intrinsic followed by a COPY of
// an *uninitialised* temp into AX, so the result was garbage and none of the
// AF/CF/SF/ZF/PF effects were modelled.  Now that i386 is a roundtrip target
// they can be verified against Unicorn (QEMU), which executes them natively.
//
// Each probe seeds AL/AH and the AF/CF flags with a preceding `addb`, runs the
// BCD instruction, then folds the architecturally-DEFINED state into EAX:
//   bits 0..7   = AL result
//   bits 8..15  = LAHF flag byte masked to the flags the instruction defines
//   bits 16..23 = AH result
// AAA/AAS define only CF/AF (mask 0x11); DAA/DAS define CF/PF/AF/ZF/SF (0xD5);
// AAM/AAD set LOGICB flags so only SF/ZF/PF are defined (0xC4).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86BcdRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86BcdRT, Verify) { roundTripX86(GetParam()); }

// Build a probe: seed AL/AH from `a`, `addb` the high byte into AL to set the
// flags, run `insn`, and fold AL | (flags & mask) << 8 | AH << 16 into EAX.
static RoundTripTC mkBcd(const std::string &name, const std::string &insn,
                         const char *mask, uint64_t input) {
  // Only flag-neutral MOVs may run between the BCD op and LAHF, otherwise LAHF
  // would capture the combiner's flags (and AF is *undefined* after a logical
  // op, so QEMU and the recompiled code legitimately disagree on it).
  std::string body =
      "\"movl %1,%%eax\\n\\t\""
      "\"movl %%eax,%%ecx\\n\\t\""
      "\"shrl $8,%%ecx\\n\\t\""
      "\"addb %%cl,%%al\\n\\t\""
      "\"" + insn + "\\n\\t\""
      "\"movzbl %%al,%%edx\\n\\t\""
      "\"movzbl %%ah,%%ecx\\n\\t\""
      "\"lahf\\n\\t\""
      "\"movzbl %%ah,%%eax\\n\\t\""
      "\"andl $" + mask + ",%%eax\\n\\t\""
      "\"shll $8,%%eax\\n\\t\""
      "\"shll $16,%%ecx\\n\\t\""
      "\"orl %%ecx,%%eax\\n\\t\""
      "\"orl %%edx,%%eax\\n\\t\""
      "\"movl %%eax,%0\\n\\t\"";
  std::string src = "int " + name + "(int a){unsigned o;__asm__ volatile(" +
                    body + ":\"=&r\"(o):\"r\"(a):\"eax\",\"ecx\",\"edx\",\"cc\");"
                           "return (int)o;}\n";
  return {name, src, {input}, "Bcd", 0, ""};
}

// clang-format off
static std::vector<RoundTripTC> makeAll() {
  std::vector<RoundTripTC> v;
  // DAA: nibble>9, AF from add, AL>0x99 (wrap + CF/ZF), CF from add.
  v.push_back(mkBcd("bcd_daa_nib",  "daa", "0xD5", 0x000AULL));
  v.push_back(mkBcd("bcd_daa_af",   "daa", "0xD5", 0x0808ULL));
  v.push_back(mkBcd("bcd_daa_hi",   "daa", "0xD5", 0x009AULL));
  v.push_back(mkBcd("bcd_daa_cf",   "daa", "0xD5", 0x9090ULL));
  // DAS: nibble borrow, AF, high borrow, CF.
  v.push_back(mkBcd("bcd_das_nib",  "das", "0xD5", 0x000FULL));
  v.push_back(mkBcd("bcd_das_af",   "das", "0xD5", 0x1008ULL));
  v.push_back(mkBcd("bcd_das_hi",   "das", "0xD5", 0x00FFULL));
  v.push_back(mkBcd("bcd_das_cf",   "das", "0xD5", 0x05FBULL));
  // AAA: low nibble>9 carries into AH; control with low nibble<=9.
  v.push_back(mkBcd("bcd_aaa_adj",  "aaa", "0x11", 0x000AULL));
  v.push_back(mkBcd("bcd_aaa_carry","aaa", "0x11", 0xFA09ULL));
  v.push_back(mkBcd("bcd_aaa_none", "aaa", "0x11", 0x0203ULL));
  // AAS: low nibble>9 borrows from AH; control.
  v.push_back(mkBcd("bcd_aas_adj",  "aas", "0x11", 0x100FULL));
  v.push_back(mkBcd("bcd_aas_borrow","aas","0x11", 0x0503ULL));
  v.push_back(mkBcd("bcd_aas_none", "aas", "0x11", 0x0402ULL));
  // AAM: AL/AH = AL divmod base (default 10, then explicit base 16).
  v.push_back(mkBcd("bcd_aam_d10a", "aam", "0xC4", 0x004BULL));
  v.push_back(mkBcd("bcd_aam_d10b", "aam", "0xC4", 0x0063ULL));
  v.push_back(mkBcd("bcd_aam_b16",  "aam $16", "0xC4", 0x00ABULL));
  // AAD: AL = AH*base + AL (default 10, then explicit base 16).
  v.push_back(mkBcd("bcd_aad_d10",  "aad", "0xC4", 0x0705ULL));
  v.push_back(mkBcd("bcd_aad_b16",  "aad $16", "0xC4", 0x0A0BULL));
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kBcd = makeAll();

INSTANTIATE_TEST_SUITE_P(Bcd, X86BcdRT, ::testing::ValuesIn(kBcd), rtTCName);
