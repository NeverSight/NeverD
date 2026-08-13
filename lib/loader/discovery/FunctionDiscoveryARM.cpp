//===- FunctionDiscoveryARM.cpp - ARM32 import thunk scan --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 import thunk recognition for heuristic function discovery.
/// Recognizes the LDR pc,[pc,#-4] veneer followed by the absolute target.
///
//===----------------------------------------------------------------------===//

#include "FunctionDiscoveryDetail.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

namespace neverd {

namespace {

bool readThumbMovImm16(const uint8_t *P, bool IsMovT, uint16_t &Imm) {
  uint16_t Op1 = readLE<uint16_t>(P);
  uint16_t Op2 = readLE<uint16_t>(P + 2);
  if ((Op1 & arm::kThumbMovImmOp1Mask) !=
          (IsMovT ? arm::kThumbMovTIPOp1 : arm::kThumbMovWIPOp1) ||
      (Op2 & arm::kThumbMovIPOp2Mask) != arm::kThumbMovIPOp2)
    return false;
  Imm = uint16_t((Op2 & 0x00ff) | ((Op2 >> 4) & 0x0700) |
                 ((Op1 << 1) & 0x0800) | ((Op1 & 0x000f) << 12));
  return true;
}

size_t scanThumbImportThunks(BinaryImage &Img, const Segment &Seg,
                             const std::map<va_t, size_t> &Targets,
                             std::set<va_t> &Existing) {
  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < arm::kThumbImportThunkLen)
    return 0;

  size_t Added = 0;
  for (size_t I = 0; I + arm::kThumbImportThunkLen <= N;
       I += arm::kThumbInsnSize) {
    uint16_t Low;
    uint16_t High;
    if (!readThumbMovImm16(D + I, false, Low) ||
        !readThumbMovImm16(D + I + arm::kThumbMovImmInsnSize, true, High) ||
        readLE<uint32_t>(D + I + 2 * arm::kThumbMovImmInsnSize) !=
            arm::kThumbLdrPCFromIP)
      continue;

    va_t Target = normalizeCodeAddress(uint32_t(Low) | (uint32_t(High) << 16),
                                       Img.Arch, Img.Mode);
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    va_t ThunkVA = normalizeCodeAddress(Seg.VA + I, Img.Arch, Img.Mode);
    Img.recordImportStub(ThunkVA, TargetIt->second);
    if (!Existing.insert(ThunkVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(ThunkVA, arm::kThumbImportThunkLen));
    ++Added;
  }
  return Added;
}

} // anonymous namespace

size_t scanImportThunksARM(BinaryImage &Img, const Segment &Seg,
                           const std::map<va_t, size_t> &Targets,
                           std::set<va_t> &Existing) {
  if (Img.Mode == InstructionMode::Thumb)
    return scanThumbImportThunks(Img, Seg, Targets, Existing);

  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < arm::kLdrPCTrampLen)
    return 0;

  size_t Added = 0;
  for (size_t I = 0; I + arm::kLdrPCTrampLen <= N; I += arm::kInsnSize) {
    uint32_t W = readLE<uint32_t>(D + I);
    if (W != arm::kLdrPC)
      continue;
    uint32_t AbsAddr = readLE<uint32_t>(D + I + arm::kInsnSize);
    va_t Target = AbsAddr;
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    va_t InsnVA = Seg.VA + I;
    Img.recordImportStub(InsnVA, TargetIt->second);
    if (!Existing.insert(InsnVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(InsnVA, arm::kLdrPCTrampLen));
    ++Added;
  }
  return Added;
}

} // namespace neverd
