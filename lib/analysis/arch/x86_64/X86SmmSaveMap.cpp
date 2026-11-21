//===- X86SmmSaveMap.cpp - KVM processor state in SMRAM -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mutable system-management save images. Layouts and their CPU capability
/// selector follow Linux arch/x86/kvm/smm.{c,h}.
//===----------------------------------------------------------------------===//

#include "X86SmmSaveMap.h"

#include "X86ModeState.h"

#include "llvm/Support/Endian.h"

namespace neverd::analysis::detail {
using namespace translate;

namespace {
namespace le = llvm::support::endian;

struct SegmentLocation {
  unsigned Cache;
  // The legacy layout separates visible selectors from cached descriptors.
  unsigned LegacySelector = 0;
};

struct SaveMap {
  bool Wide;
  uint32_t Revision;
  SegmentLocation ES, CS, SS, DS, FS, GS, LDTR, TR;
  unsigned GDTR, IDTR, SMBase, Shadow, CR4, CR3, CR0, DR7, DR6, Flags, IP;

  unsigned gprOffset(unsigned I) const {
    return Wide ? 0x1f8 - 8 * I : 0x1d0 + 4 * I;
  }
  uint64_t readWord(const X86SmmSaveImage &B, unsigned At) const {
    return Wide ? le::read64le(B.data() + At) : le::read32le(B.data() + At);
  }
  void writeWord(X86SmmSaveImage &B, unsigned At, uint64_t Value) const {
    if (Wide)
      le::write64le(B.data() + At, Value);
    else
      le::write32le(B.data() + At, uint32_t(Value));
  }
  void putSegment(X86SmmSaveImage &B, SegmentLocation At,
                  X64ContinuationSegment D, bool Unusable) const {
    const uint16_t Attr = Unusable ? D.Attributes & ~0x80u : D.Attributes;
    if (Wide) {
      le::write16le(B.data() + At.Cache, D.Selector);
      le::write16le(B.data() + At.Cache + 2, Attr);
    } else {
      le::write32le(B.data() + At.LegacySelector, D.Selector);
      le::write32le(B.data() + At.Cache, uint32_t(Attr) << 8);
    }
    le::write32le(B.data() + At.Cache + 4, D.Limit);
    writeWord(B, At.Cache + 8, D.Base);
  }
  X64ContinuationSegment segment(const X86SmmSaveImage &B,
                                 SegmentLocation At) const {
    const auto Attr = Wide ? le::read16le(B.data() + At.Cache + 2)
                           : le::read32le(B.data() + At.Cache) >> 8;
    const unsigned Selector = Wide ? At.Cache : At.LegacySelector;
    return {readWord(B, At.Cache + 8), le::read32le(B.data() + At.Cache + 4),
            le::read16le(B.data() + Selector), uint16_t(Attr & 0xf0ff)};
  }
  void putTable(X86SmmSaveImage &B, unsigned At,
                const X64ContinuationDescriptorTable &T) const {
    le::write32le(B.data() + At + 4, T.Limit);
    writeWord(B, At + 8, T.Base);
  }
  X64ContinuationDescriptorTable table(const X86SmmSaveImage &B,
                                       unsigned At) const {
    return {readWord(B, At + 8), le::read16le(B.data() + At + 4)};
  }
};

constexpr SaveMap Kvm64{.Wide = true,
                        .Revision = 0x20064,
                        .ES = {0x00},
                        .CS = {0x10},
                        .SS = {0x20},
                        .DS = {0x30},
                        .FS = {0x40},
                        .GS = {0x50},
                        .LDTR = {0x70},
                        .TR = {0x90},
                        .GDTR = 0x60,
                        .IDTR = 0x80,
                        .SMBase = 0x100,
                        .Shadow = 0xcb,
                        .CR4 = 0x148,
                        .CR3 = 0x150,
                        .CR0 = 0x158,
                        .DR7 = 0x160,
                        .DR6 = 0x168,
                        .Flags = 0x170,
                        .IP = 0x178};
constexpr SaveMap Kvm32{.Wide = false,
                        .Revision = 0x20000,
                        .ES = {0x184, 0x1a8},
                        .CS = {0x190, 0x1ac},
                        .SS = {0x19c, 0x1b0},
                        .DS = {0x12c, 0x1b4},
                        .FS = {0x138, 0x1b8},
                        .GS = {0x144, 0x1bc},
                        .LDTR = {0x178, 0x1c0},
                        .TR = {0x15c, 0x1c4},
                        .GDTR = 0x16c,
                        .IDTR = 0x150,
                        .SMBase = 0xf8,
                        .Shadow = 0x11a,
                        .CR4 = 0x114,
                        .CR3 = 0x1f8,
                        .CR0 = 0x1fc,
                        .DR7 = 0x1c8,
                        .DR6 = 0x1cc,
                        .Flags = 0x1f4,
                        .IP = 0x1f0};
} // namespace

X86SmmSaveImage saveX86SmmState(const X86ModeState &S, const X86PagingState &C,
                                bool LongModeCapable) {
  const auto &M = LongModeCapable ? Kvm64 : Kvm32;
  X86SmmSaveImage B{};
  const auto &V = S.Core.Core;
  const auto &R = modeRegisters(S);
  M.putSegment(B, M.ES, V.ES, V.Unusable & 8);
  M.putSegment(B, M.CS, V.Core.CS, V.Unusable & 1);
  M.putSegment(B, M.SS, V.Core.SS, V.Unusable & 2);
  M.putSegment(B, M.DS, V.DS, V.Unusable & 4);
  M.putSegment(B, M.FS, {R.FSBase, V.FS.Limit, V.FS.Selector, V.FS.Attributes},
               V.Unusable & 16);
  M.putSegment(B, M.GS, {R.GSBase, V.GS.Limit, V.GS.Selector, V.GS.Attributes},
               V.Unusable & 32);
  M.putTable(B, M.GDTR, V.GDTR);
  M.putSegment(B, M.LDTR, V.LDTR, V.Unusable & 64);
  M.putTable(B, M.IDTR, V.IDTR);
  M.putSegment(B, M.TR, V.TR, V.Unusable & 128);
  B[M.Shadow] = uint8_t(S.SSBlocked) | (uint8_t(S.STIBlocked) << 1);
  if (M.Wide)
    le::write64le(B.data() + 0xd0, C.EFER);
  le::write32le(B.data() + 0xfc, M.Revision);
  le::write32le(B.data() + M.SMBase, S.SMM->SMBase);
  M.writeWord(B, M.CR4, C.CR4);
  M.writeWord(B, M.CR3, C.CR3);
  M.writeWord(B, M.CR0, C.CR0);
  M.writeWord(B, M.DR7, S.Debug->DR7);
  M.writeWord(B, M.DR6, S.Debug->DR6);
  M.writeWord(B, M.Flags, R.RFlags);
  M.writeWord(B, M.IP, R.PC);
  for (unsigned I = 0, Count = M.Wide ? 16 : 8; I != Count; ++I)
    M.writeWord(B, M.gprOffset(I), R.GPR[I]);
  return B;
}

void restoreX86SmmState(const X86SmmSaveImage &B, X86ModeState &S,
                        X86PagingState &C, bool LongModeCapable) {
  const auto &M = LongModeCapable ? Kvm64 : Kvm32;
  auto &V = S.Core.Core;
  auto &R = modeRegisters(S);
  V.Unusable = 0;
  const auto Get = [&](SegmentLocation At, unsigned Bit) {
    auto D = M.segment(B, At);
    if (!(D.Attributes & 0x80))
      V.Unusable |= 1u << Bit;
    return D;
  };
  V.ES = Get(M.ES, 3);
  V.Core.CS = Get(M.CS, 0);
  V.Core.SS = Get(M.SS, 1);
  V.DS = Get(M.DS, 2);
  auto FS = Get(M.FS, 4), GS = Get(M.GS, 5);
  V.FS = {FS.Limit, FS.Selector, FS.Attributes};
  V.GS = {GS.Limit, GS.Selector, GS.Attributes};
  R.FSBase = FS.Base;
  R.GSBase = GS.Base;
  V.GDTR = M.table(B, M.GDTR);
  V.LDTR = Get(M.LDTR, 6);
  V.IDTR = M.table(B, M.IDTR);
  V.TR = Get(M.TR, 7);
  S.SSBlocked = B[M.Shadow] & 1;
  S.STIBlocked = B[M.Shadow] & 2;
  C.CR4 = M.readWord(B, M.CR4);
  C.CR3 = M.readWord(B, M.CR3);
  C.CR0 = M.readWord(B, M.CR0);
  if (M.Wide) {
    C.EFER = le::read64le(B.data() + 0xd0) & ~uint64_t(0x400);
    if ((C.CR0 & 0x80000000) && (C.EFER & 0x100))
      C.EFER |= 0x400;
  }
  S.Debug->DR7 = M.readWord(B, M.DR7);
  S.Debug->DR6 = M.readWord(B, M.DR6);
  C.RFlags = R.RFlags = M.readWord(B, M.Flags) | 2;
  R.PC = M.readWord(B, M.IP);
  for (unsigned I = 0, Count = M.Wide ? 16 : 8; I != Count; ++I)
    R.GPR[I] = M.readWord(B, M.gprOffset(I));
  S.SMM->SMBase = le::read32le(B.data() + M.SMBase);
  if (!S.SMM->EnteredInsideNMI)
    V.NMIBlocked = false;
  S.RetainedRealCS = (C.CR0 & 1) && !(R.RFlags & 0x20000) &&
                     S.SMM->InterruptedRealCS == V.Core.CS;
  S.SMM->InterruptedRealCS.reset();
  S.SMM->EnteredInsideNMI = S.SMM->Active = false;
}
} // namespace neverd::analysis::detail
