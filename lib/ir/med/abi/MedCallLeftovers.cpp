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

#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

std::tuple<int, int, int, uint16_t> valueKey(const MedVar &V) {
  return {static_cast<int>(V.Kind), V.Id, V.SSAVer, V.Size};
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
  // A candidate may read only a prefix of the SSA clobber. Its whole value
  // must remain excluded; it is not an unused ABI scratch value.
  std::set<std::tuple<int, int, int>> ResultFields;
  for (const MedVar &Field : PossibleResults)
    ResultFields.emplace(static_cast<int>(Field.Kind), Field.Id, Field.SSAVer);
  for (const MedStructReturnCandidate &Candidate : Func.StructReturnCandidates)
    for (const MedVar &Field : Candidate.Fields)
      ResultFields.emplace(static_cast<int>(Field.Kind), Field.Id,
                           Field.SSAVer);

  enum class OriginKind { Pending, Unspecified, Register, Other };
  struct Origin {
    OriginKind Kind = OriginKind::Pending;
    uint64_t Register = 0;
    bool operator==(const Origin &) const = default;
  };
  struct Node {
    MedVar Output;
    std::vector<MedVar> Inputs;
    std::vector<size_t> Readers;
    Origin State;
    bool Seed = false;
    bool Queued = true;
  };
  std::map<ValueKey, size_t> Index;
  std::vector<Node> Nodes;
  auto add = [&](MedVar Output, std::vector<MedVar> Inputs, Origin State,
                 bool Seed = false) {
    const bool Inserted = Index.emplace(valueKey(Output), Nodes.size()).second;
    assert(Inserted && "multiple definitions of a transported SSA value");
    (void)Inserted;
    Nodes.push_back({Output, std::move(Inputs), {}, State, Seed});
  };
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Clobber.Value.Kind == MedVar::Reg && Clobber.Value.Size &&
        Clobber.PreservedPrefixSize == 0 &&
        !ResultFields.count({static_cast<int>(Clobber.Value.Kind),
                             Clobber.Value.Id, Clobber.Value.SSAVer}))
      add(Clobber.Value, {}, {OriginKind::Register, Clobber.Value.RegOff},
          true);

  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if ((Phi.Output.Kind == MedVar::Reg || Phi.Output.Kind == MedVar::Temp) &&
          Phi.Output.Size) {
        std::vector<MedVar> Inputs;
        for (const auto &[Pred, Incoming] : Phi.Args)
          Inputs.push_back(Incoming);
        if (Phi.ExceptionalEntry)
          Inputs.push_back(*Phi.ExceptionalEntry);
        add(Phi.Output, std::move(Inputs), {});
      }
    for (const MedOp &Op : Block.Ops)
      if (!Op.Dead && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp) &&
          Op.Output.Size && Op.Output != Op.Inputs[0])
        add(Op.Output, {Op.Inputs[0]}, {});
  }
  for (size_t I = 0; I < Nodes.size(); ++I)
    for (const auto &Input : Nodes[I].Inputs)
      if (const auto It = Index.find(valueKey(Input)); It != Index.end())
        Nodes[It->second].Readers.push_back(I);

  auto merge = [](Origin A, Origin B) {
    if (A.Kind == OriginKind::Other || B.Kind == OriginKind::Other)
      return Origin{OriginKind::Other};
    if (A.Kind == OriginKind::Pending || A.Kind == OriginKind::Unspecified)
      return B.Kind == OriginKind::Pending ? A : B;
    if (B.Kind == OriginKind::Pending || B.Kind == OriginKind::Unspecified)
      return A;
    return A.Register == B.Register ? A : Origin{OriginKind::Other};
  };
  // Start with actual calls, not a presumed origin for every PHI. A cycle
  // without a seed stays pending. A mixed/exceptional input invalidates all
  // readers, including copies in a loop, without rescanning unrelated blocks.
  std::vector<size_t> Work;
  for (size_t I = 0; I < Nodes.size(); ++I)
    Work.push_back(I);
  for (size_t I = 0; I < Work.size(); ++I) {
    auto &N = Nodes[Work[I]];
    N.Queued = false;
    if (N.Seed)
      continue;
    Origin Next;
    if (N.Inputs.empty())
      Next.Kind = OriginKind::Other;
    for (const auto &Input : N.Inputs) {
      Origin Source{OriginKind::Other};
      if (Input.Size == N.Output.Size) {
        if (Input.Kind == MedVar::Unspecified)
          Source.Kind = OriginKind::Unspecified;
        else if (const auto It = Index.find(valueKey(Input)); It != Index.end())
          Source = Nodes[It->second].State;
      }
      Next = merge(Next, Source);
    }
    if (N.Output.Kind == MedVar::Reg) {
      if (Next.Kind == OriginKind::Register && Next.Register != N.Output.RegOff)
        Next = {OriginKind::Other};
    }
    if (Next == N.State)
      continue;
    N.State = Next;
    for (size_t Reader : N.Readers)
      if (!Nodes[Reader].Queued) {
        Nodes[Reader].Queued = true;
        Work.push_back(Reader);
      }
  }
  for (const auto &N : Nodes)
    if (N.State.Kind == OriginKind::Register)
      Values.emplace(valueKey(N.Output), N.State.Register);
}

bool ValuesLeftByCalls::contains(const MedVar &V) const {
  return V.Kind == MedVar::Unspecified ||
         (V.Kind == MedVar::Reg && containsForRegister(V, V.RegOff));
}

bool ValuesLeftByCalls::containsForRegister(const MedVar &V,
                                            uint64_t RegOff) const {
  if (V.Kind == MedVar::Unspecified)
    return true;
  const auto It = Values.find(valueKey(V));
  return It != Values.end() && It->second == RegOff;
}

} // namespace neverd
