//===- RegistrationCxxState.cpp - Current-image C++ call evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind ordinary and invocation-specific C++ state proofs to current code.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/RegistrationCxxState.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationConditionalFrame.h"
#include "RegistrationSourceCall.h"
#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFCxxEpilogue.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"

#include <set>
#include <utility>

namespace neverd {

namespace {
RegistrationStateAnalysis solveCheckedCxxStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *SEHControls,
    registration_state::ConditionalFrame *Inputs = nullptr,
    size_t *StateWork = nullptr) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return {};
  const auto &EH = *Function.ExceptionMetadata;
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
  const auto Prologue = coff_loader::getCheckedX86CxxPrologueCall(Image, EH);
  std::vector<coff_loader::X86CxxEpilogueCall> Epilogues;
  if (Prologue)
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.Addr != Prologue->CallVA)
          if (const auto Exit = coff_loader::getCheckedX86CxxEpilogueCall(
                  Image, EH, Op.Addr)) {
            if (Epilogues.size() == limits::kMaxRegistrationEHRecords)
              return {};
            Epilogues.push_back(*Exit);
          }
  const va_t Cookie =
      Image.Base <= UINT32_MAX && Image.DynInfo.SecurityCookieRVA &&
              Image.DynInfo.SecurityCookieRVA <= UINT32_MAX - Image.Base
          ? Image.Base + Image.DynInfo.SecurityCookieRVA
          : 0;
  registration_state::RegistrationStateSolver Solver(
      Function, Cookie, 0, Callees, Cleanups, Stacks, LocalUnwinds, SEHControls,
      Inputs, nullptr, nullptr, Handler ? &*Handler : nullptr,
      Prologue ? &*Prologue : nullptr, &Epilogues);
  auto Result = Solver.run(StateWork ? *StateWork : 0);
  if (StateWork)
    *StateWork = Solver.workUsed();
  return Result;
}
} // namespace

RegistrationStateAnalysis analyzeCheckedX86CxxStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *SEHControls) {
  return solveCheckedCxxStates(Image, Function, Callees, Cleanups, Stacks,
                               LocalUnwinds, SEHControls);
}

namespace registration_state {
std::optional<BoundCxxCalleeEffect>
proveBoundCxxCallee(const BinaryImage &Image, const LowOp &Op,
                    const FrameState &Parent, uint32_t StackArgumentBytes,
                    size_t &Work,
                    llvm::ArrayRef<RegistrationObjectExtent> CallerObjects,
                    const std::set<int32_t> *Initialized) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Op.Opcode != NdOp::CALL ||
      Op.Seq < 0 || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4 || Op.Addr > UINT32_MAX - 5 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      StackArgumentBytes > UINT16_MAX || StackArgumentBytes % 4 ||
      !registration_abi::chargeCalleeWork(Work, 5))
    return std::nullopt;
  const va_t Target = Op.Inputs[0].Offset;
  // Invocation inputs are never cached by target. Cycles and deep acyclic
  // chains refuse before another decoder/solver can consume host stack.
  using Query = std::pair<const BinaryImage *, va_t>;
  static thread_local std::set<Query> Active;
  const Query Identity{&Image, Target};
  if (Active.size() >= limits::ir::kRegistrationCxxMaxBoundCallDepth ||
      !Active.insert(Identity).second)
    return std::nullopt;
  const auto Leave = llvm::make_scope_exit([&] { Active.erase(Identity); });
  const auto Call = readImmutableCodeBytes(Image, Op.Addr, 5);
  const auto *EH = Image.ExceptionMetadata.findFunction(Target);
  if (!Call || (*Call)[0] != 0xe8 ||
      uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Call->data() + 1))) !=
          Target ||
      !EH || !EH->Registration || !EH->Cxx || EH->CodeRange.Begin != Target ||
      EH->Registration->RealignedFrame || !EH->Cxx->TryBlocks.empty() ||
      !coff_loader::getCheckedX86CxxHandlerPointerSource(Image, *EH))
    return std::nullopt;
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, *EH);
  if (!Handler)
    return std::nullopt;
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  RegistrationCallCalleeIndex Index(Image, &Work);
  CFGBuilder Builder;
  Builder.setRegistrationCalleeIndex(&Index);
  const auto Function =
      Builder.buildIntegerFrameProof(Image, Dec, Target, "bound-cxx-callee");
  if (!Function.hasCompleteLiftCoverage() ||
      !registration_abi::chargeDecodedCallee(Work, Function))
    return std::nullopt;
  for (const auto &Block : Function.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries) {
      if (!registration_abi::chargeCalleeWork(Work, 1) ||
          !registration_abi::hasImmutableCalleeInstruction(Image, Insn, Dec))
        return std::nullopt;
      if (Insn.Control == LowInstructionControl::Return) {
        const auto Bytes = readImmutableCodeBytes(Image, Insn.Address, 3);
        if (Insn.Size != 3 || !Bytes || (*Bytes)[0] != 0xc2)
          return std::nullopt;
        const uint32_t Pop = readLE<uint16_t>(Bytes->data() + 1);
        if (!Pop || Pop % 4 ||
            (StackArgumentBytes && Pop != StackArgumentBytes))
          return std::nullopt;
        StackArgumentBytes = Pop;
      }
    }
  if (!StackArgumentBytes)
    return std::nullopt;
  const auto SP =
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP < INT32_MIN + 8 ||
      int64_t(*SP) + StackArgumentBytes > INT32_MAX)
    return std::nullopt;
  const int64_t Base = int64_t(*SP) - 8;
  ConditionalFrame Inputs;
  auto ReadWord = [&](int32_t Offset) -> std::optional<FrameValue> {
    return registration_abi::readCallerFrameWord(Parent, Offset, Initialized,
                                                 Work);
  };
  for (unsigned Index = 0; Index != Parent.Registers.size(); ++Index) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    const auto &Value = Parent.Registers[Index];
    if (isX86VolatileFrameRegister(Index) && !Value.MayBeFrame)
      Inputs.Registers.emplace(Index, Value);
  }
  for (uint32_t Arg = 0; Arg < StackArgumentBytes; Arg += 4) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    const auto Cell = ReadWord(int32_t(*SP + int64_t(Arg)));
    if (!Cell)
      continue;
    const auto &Value = *Cell;
    if (!Value.MayBeFrame) {
      Inputs.Arguments.emplace(8 + Arg, Value);
      continue;
    }
    if (!Value.Offset || Value != FrameValue::frame(*Value.Offset))
      continue;
    const auto Object = ReadWord(*Value.Offset);
    const int64_t Offset = int64_t(*Value.Offset) - Base;
    if (!registration_abi::chargeCalleeWork(Work, CallerObjects.size()))
      return std::nullopt;
    const bool Borrowed = llvm::any_of(CallerObjects, [&](const auto &Range) {
      return Range.Begin >= 8 && Range.Begin <= *Value.Offset &&
             int64_t(*Value.Offset) + 4 <= Range.End;
    });
    // The exact incoming area ends before the supplied caller word. Saved
    // EBP/PC are outside the caller allocation. Its other administration and
    // object ownership remain preconditions of the caller's frame proof.
    if (!Object || Object->MayBeFrame ||
        *Value.Offset < int64_t(*SP) + StackArgumentBytes ||
        (!Borrowed && int64_t(*Value.Offset) + 4 > 0) || Offset > INT32_MAX - 4)
      continue;
    Inputs.Arguments.emplace(8 + Arg, FrameValue::frame(int32_t(Offset)));
    Inputs.Objects.push_back({int32_t(Offset), int32_t(Offset + 4)});
    Inputs.ObjectCells.emplace(int32_t(Offset), *Object);
  }
  if (Inputs.Objects.empty())
    return std::nullopt;
  Inputs.ProveCxxCall =
      [&](const LowFunc &Caller, const LowOp &Call, const FrameState &State,
          const std::set<int32_t> &InitializedBytes,
          size_t &Used) -> std::optional<BoundCxxCalleeEffect> {
    if (!checkedRegistrationDirectCall(Image, Caller, Call, Used))
      return std::nullopt;
    return proveBoundCxxCallee(Image, Call, State, 0, Used, Inputs.Objects,
                               &InitializedBytes);
  };
  const auto Callees = Index.contracts(Function);
  const auto Cleanups = Index.cleanupContracts(Function);
  const auto Stacks = Index.stackContracts(Function);
  if (!Callees || !Cleanups || !Stacks)
    return std::nullopt;
  // This source-only domain admits private/borrowed writes. An image write
  // could change the personality IAT, a nested import or another proof input
  // while FS0 publishes the incoming pointer. A stack/leaf ABI alone cannot
  // preserve that evidence; image-writing children need a separate closure.
  // Direct CRT throws also need the enclosing runtime-object/callback proof;
  // checked private throw helpers already carry their closed object effects.
  auto HasPrivateEffects = [&](const auto &C) {
    return registration_abi::chargeCalleeWork(Work, 1) && !C.isRuntimeThrow() &&
           C.ImageWrites.empty() && C.CallerPCWrites.empty();
  };
  for (const auto &Callee : *Callees)
    if (!HasPrivateEffects(Callee))
      return std::nullopt;
  for (const auto &Cleanup : *Cleanups) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    for (const auto &Call : Cleanup.Calls)
      if (!HasPrivateEffects(Call.Leaf))
        return std::nullopt;
  }
  size_t StateWork = Work;
  const auto States =
      solveCheckedCxxStates(Image, Function, &*Callees, &*Cleanups, &*Stacks,
                            nullptr, nullptr, &Inputs, &StateWork);
  Work = StateWork;
  if (!Inputs.Complete || Inputs.ReturnValues.empty() || !States.Complete ||
      !States.CallbackStatesComplete || !States.RegistrationLifetimeComplete ||
      !States.ChainOperationsComplete || !States.CallFrameEffectsComplete ||
      !States.CleanupFrameEffectsComplete || !States.CxxCatchObjectsComplete ||
      !States.RuntimeObjectAccessesComplete ||
      !States.IncomingFrameAccessesComplete || !States.ImageReadsComplete ||
      (!Handler->CookieGuards.empty() && !States.SecurityCookiesComplete))
    return std::nullopt;
  if (!registration_abi::chargeCalleeWork(
          Work, Inputs.Objects.size() + Inputs.Reads.size() +
                    Inputs.Writes.size() + States.ImageReads.size() + 1))
    return std::nullopt;
  BoundCxxCalleeEffect Effect;
  Effect.StackPopBytes = StackArgumentBytes;
  for (const auto &Object : Inputs.Objects)
    Effect.Objects.push_back(
        {int32_t(Base + Object.Begin), int32_t(Base + Object.End)});
  Effect.ImageReads = States.ImageReads;
  for (const auto &[Begin, End] : Inputs.Reads)
    Effect.Reads.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  for (const auto &[Begin, End] : Inputs.Writes)
    Effect.Writes.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  for (const auto &[Identity, Value] : Inputs.ReturnValues) {
    if (!registration_abi::chargeCalleeWork(Work, Inputs.Objects.size() + 1))
      return std::nullopt;
    BoundCxxCalleeEffect::Return Return{Identity.first, Identity.second,
                                        Value.MayBeFrame, std::nullopt};
    if (Value.Offset && Value == FrameValue::frame(*Value.Offset) &&
        llvm::any_of(Inputs.Objects, [&](const auto &Object) {
          return Object.Begin <= *Value.Offset && *Value.Offset <= Object.End;
        }))
      Return.BorrowedOffset = int32_t(Base + *Value.Offset);
    Effect.Returns.push_back(Return);
  }
  return Effect;
}
} // namespace registration_state

} // namespace neverd
