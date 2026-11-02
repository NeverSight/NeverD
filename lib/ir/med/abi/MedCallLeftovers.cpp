//===- MedCallLeftovers.cpp - Values calls leave in registers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallLeftovers.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <optional>
#include <vector>

namespace neverd {
namespace {

std::tuple<int, int, int> valueKey(const MedVar &V) {
  return {static_cast<int>(V.Kind), V.Id, V.SSAVer};
}

/// Whether every call keeps all of the view: the convention preserves its
/// low bytes through the whole view, or one of its callee-saved ranges
/// holds it.
bool conventionPreserves(const TargetRegInfo &TRI, BinaryFormat Format,
                         uint64_t RegOff, uint16_t Size) {
  if (Size == 0)
    return false;
  if (TRI.callPreservedPrefixSize(RegOff, Size) >= Size)
    return true;
  // A run asks about every register of every call with the same target and
  // format, whose ranges never change: build them once per thread.
  thread_local const TargetRegInfo *RangesTarget = nullptr;
  thread_local std::optional<BinaryFormat> RangesFormat;
  thread_local std::vector<TargetRegisterRange> Ranges;
  if (RangesTarget != &TRI || RangesFormat != Format) {
    Ranges = TRI.callPreservedRanges(Format);
    RangesTarget = &TRI;
    RangesFormat = Format;
  }
  return llvm::any_of(Ranges, [&](const auto &Range) {
    return RegOff >= Range.Offset && Size <= Range.Bytes &&
           RegOff + Size <= Range.Offset + Range.Bytes;
  });
}

} // namespace

bool callClobbersRegister(const MedOp &Call, uint64_t RegOff, uint16_t Size,
                          const TargetRegInfo &TRI, BinaryFormat Format) {
  if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
      Call.PreservesCallerSaved)
    return false;
  if (Call.X87ArgumentInput && !Call.X87ImportArguments &&
      TRI.TheArch == Arch::X86) {
    // The checked consumer closure forbids control-word changes.
    if (RegOff == x86reg::FPU_CW && Size <= 2)
      return false;
    if (TRI.isX87StackReg(RegOff))
      return (Call.X87ClobberedSlots >> x86reg::stRegIndex(RegOff)) & 1;
  }
  if (TRI.isFrameOrLinkReg(RegOff) || TRI.isStackPointer(RegOff) ||
      (Call.Output.Kind == MedVar::Reg && Call.Output.RegOff == RegOff))
    return false;
  return !conventionPreserves(TRI, Format, RegOff, Size) &&
         !callPreservesRegisterBytes(TRI.TheArch, Call.CallPreservedGPRs,
                                     RegOff, Size);
}

std::optional<MedVar> valueLeftByCall(const MedFunc &Func, const MedOp &Call,
                                      uint64_t RegOff, uint16_t Size,
                                      const TargetRegInfo &TRI,
                                      BinaryFormat Format) {
  if (!callClobbersRegister(Call, RegOff, Size, TRI, Format))
    return std::nullopt;
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Call.CallSiteId != 0 && Clobber.CallSiteId == Call.CallSiteId &&
        Clobber.Value.Kind == MedVar::Reg && Clobber.Value.RegOff == RegOff &&
        Clobber.Value.Size == Size)
      return Clobber.Value;
  return MedVar::makeUnspecified(Size, TRI.TheArch);
}

ValuesLeftByCalls::ValuesLeftByCalls(const MedFunc &Func)
    : ValuesLeftByCalls(Func, {}) {}

ValuesLeftByCalls::ValuesLeftByCalls(const MedFunc &Func,
                                     llvm::ArrayRef<MedVar> PossibleResults) {
  std::set<std::tuple<int, int, int>> ResultFields;
  for (const MedVar &Field : PossibleResults)
    ResultFields.insert(valueKey(Field));
  for (const MedStructReturnCandidate &Candidate : Func.StructReturnCandidates)
    for (const MedVar &Field : Candidate.Fields)
      ResultFields.insert(valueKey(Field));
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Clobber.Value.Kind == MedVar::Reg && Clobber.PreservedPrefixSize == 0 &&
        !ResultFields.count(valueKey(Clobber.Value)))
      Values.insert(valueKey(Clobber.Value));
  if (Values.empty())
    return;

  // A join holds only what calls left where everything reaching it does.
  // Take every join for one, then drop each that anything else reaches, and
  // each that a dropped join reaches: the joins that remain, loops of them
  // included, are all such joins.
  std::set<std::tuple<int, int, int>> Joins;
  std::vector<const PhiNode *> Phis;
  for (const MedBlock &Block : Func.Blocks)
    for (const PhiNode &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg) {
        Joins.insert(valueKey(Phi.Output));
        Phis.push_back(&Phi);
      }
  std::map<std::tuple<int, int, int>, std::vector<const PhiNode *>> Readers;
  std::vector<const PhiNode *> Dropped;
  for (const PhiNode *Phi : Phis) {
    bool ReachedByOther = Phi->Args.empty();
    auto reachedBy = [&](const MedVar &Incoming) {
      const auto Key = valueKey(Incoming);
      Readers[Key].push_back(Phi);
      ReachedByOther |= Incoming.Kind != MedVar::Unspecified &&
                        !Values.count(Key) && !Joins.count(Key);
    };
    for (const auto &[Pred, Incoming] : Phi->Args)
      reachedBy(Incoming);
    // The exception dispatcher's entry is one more path.
    if (Phi->ExceptionalEntry)
      reachedBy(*Phi->ExceptionalEntry);
    if (ReachedByOther)
      Dropped.push_back(Phi);
  }
  while (!Dropped.empty()) {
    const PhiNode *Phi = Dropped.back();
    Dropped.pop_back();
    const auto Key = valueKey(Phi->Output);
    if (!Joins.erase(Key))
      continue;
    if (const auto It = Readers.find(Key); It != Readers.end())
      llvm::append_range(Dropped, It->second);
  }
  Values.insert(Joins.begin(), Joins.end());
}

bool ValuesLeftByCalls::contains(const MedVar &V) const {
  return V.Kind == MedVar::Unspecified ||
         (V.Kind == MedVar::Reg && Values.count(valueKey(V)));
}

} // namespace neverd
