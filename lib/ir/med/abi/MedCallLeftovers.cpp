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
#include <cstdint>
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
  // Each transported value with its inputs and the values that read it, as
  // ranges of Inputs and Readers: a large callee transports thousands.
  struct Node {
    MedVar Output;
    uint32_t FirstInput = 0;
    uint32_t InputCount = 0;
    uint32_t FirstReader = 0;
    uint32_t ReaderCount = 0;
    Origin State;
    bool Seed = false;
    bool Queued = true;
  };
  auto transports = [](const MedVar &V) {
    return (V.Kind == MedVar::Reg || V.Kind == MedVar::Temp) && V.Size;
  };
  auto copies = [&](const MedOp &Op) {
    return !Op.Dead && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           transports(Op.Output) && Op.Output != Op.Inputs[0];
  };
  size_t Expected = Func.CallClobbers.size();
  for (const MedBlock &Block : Func.Blocks) {
    Expected += Block.Phis.size();
    for (const MedOp &Op : Block.Ops)
      Expected += copies(Op);
  }
  llvm::DenseMap<ValueKey, uint32_t> Index;
  Index.reserve(Expected);
  std::vector<Node> Nodes;
  Nodes.reserve(Expected);
  std::vector<MedVar> Inputs;
  auto add = [&](const MedVar &Output, Origin State, bool Seed = false) {
    const bool Inserted =
        Index.try_emplace(valueKey(Output), static_cast<uint32_t>(Nodes.size()))
            .second;
    assert(Inserted && "multiple definitions of a transported SSA value");
    (void)Inserted;
    Node &N = Nodes.emplace_back();
    N.Output = Output;
    N.FirstInput = static_cast<uint32_t>(Inputs.size());
    N.State = State;
    N.Seed = Seed;
  };
  auto closeInputs = [&] {
    Nodes.back().InputCount =
        static_cast<uint32_t>(Inputs.size()) - Nodes.back().FirstInput;
  };
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Clobber.Value.Kind == MedVar::Reg && Clobber.Value.Size &&
        Clobber.PreservedPrefixSize == 0 &&
        !ResultFields.count({static_cast<int>(Clobber.Value.Kind),
                             Clobber.Value.Id, Clobber.Value.SSAVer}))
      add(Clobber.Value, {OriginKind::Register, Clobber.Value.RegOff}, true);

  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if (transports(Phi.Output)) {
        add(Phi.Output, {});
        for (const auto &[Pred, Incoming] : Phi.Args)
          Inputs.push_back(Incoming);
        if (Phi.ExceptionalEntry)
          Inputs.push_back(*Phi.ExceptionalEntry);
        closeInputs();
      }
    for (const MedOp &Op : Block.Ops)
      if (copies(Op)) {
        add(Op.Output, {});
        Inputs.push_back(Op.Inputs[0]);
        closeInputs();
      }
  }
  // The node each input names, read once: an input of another width or no
  // transported value brings something else, and an Unspecified one nothing.
  // Every named input makes its node's readers include the reading node, in
  // the order the nodes read them.
  constexpr int32_t OtherSource = -1;
  constexpr int32_t UnspecifiedSource = -2;
  std::vector<int32_t> Named(Inputs.size(), OtherSource);
  std::vector<int32_t> Sources(Inputs.size(), OtherSource);
  for (const Node &N : Nodes)
    for (uint32_t J = N.FirstInput; J < N.FirstInput + N.InputCount; ++J) {
      const MedVar &Input = Inputs[J];
      if (const auto It = Index.find(valueKey(Input)); It != Index.end()) {
        Named[J] = static_cast<int32_t>(It->second);
        ++Nodes[It->second].ReaderCount;
      }
      if (Input.Size == N.Output.Size)
        Sources[J] =
            Input.Kind == MedVar::Unspecified ? UnspecifiedSource : Named[J];
    }
  std::vector<uint32_t> Readers;
  {
    uint32_t Next = 0;
    for (Node &N : Nodes) {
      N.FirstReader = Next;
      Next += N.ReaderCount;
    }
    Readers.resize(Next);
    std::vector<uint32_t> Filled(Nodes.size(), 0);
    for (uint32_t I = 0; I < Nodes.size(); ++I)
      for (uint32_t J = Nodes[I].FirstInput;
           J < Nodes[I].FirstInput + Nodes[I].InputCount; ++J)
        if (Named[J] != OtherSource) {
          Node &Read = Nodes[static_cast<uint32_t>(Named[J])];
          Readers[Read.FirstReader +
                  Filled[static_cast<uint32_t>(Named[J])]++] = I;
        }
  }

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
  std::vector<uint32_t> Work(Nodes.size());
  for (uint32_t I = 0; I < Nodes.size(); ++I)
    Work[I] = I;
  for (size_t W = 0; W < Work.size(); ++W) {
    Node &N = Nodes[Work[W]];
    N.Queued = false;
    if (N.Seed)
      continue;
    Origin Next;
    if (!N.InputCount)
      Next.Kind = OriginKind::Other;
    for (uint32_t J = N.FirstInput; J < N.FirstInput + N.InputCount; ++J) {
      Origin Source{OriginKind::Other};
      if (Sources[J] == UnspecifiedSource)
        Source.Kind = OriginKind::Unspecified;
      else if (Sources[J] != OtherSource)
        Source = Nodes[static_cast<uint32_t>(Sources[J])].State;
      Next = merge(Next, Source);
    }
    if (N.Output.Kind == MedVar::Reg) {
      if (Next.Kind == OriginKind::Register && Next.Register != N.Output.RegOff)
        Next = {OriginKind::Other};
    }
    if (Next == N.State)
      continue;
    N.State = Next;
    for (uint32_t R = N.FirstReader; R < N.FirstReader + N.ReaderCount; ++R)
      if (Node &Reader = Nodes[Readers[R]]; !Reader.Queued) {
        Reader.Queued = true;
        Work.push_back(Readers[R]);
      }
  }
  for (const auto &N : Nodes)
    if (N.State.Kind == OriginKind::Register)
      Values.try_emplace(valueKey(N.Output), N.State.Register);
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
