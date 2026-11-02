//===- RegistrationConditionalCallee.cpp - Bound original SEH helpers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "RegistrationABIPrivate.h"
#include "RegistrationCRTFrame.h"
#include "RegistrationJumpRuntime.h"
#include "RegistrationOriginalImage.h"
#include "RegistrationSEHPrivateCall.h"
#include "RegistrationStateSolver.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>

namespace neverd::registration_state {

namespace {
struct SEH4EntryCalls {
  std::optional<coff_loader::X86SEH4PrologueCall> Entry;
  std::vector<coff_loader::X86SEH4EpilogueCall> Exits;
  size_t WorkUsed = 0;

  SEH4EntryCalls(const BinaryImage &Image, const LowFunc &Function)
      : Entry(getCheckedX86RegistrationSEH4ProloguePrefix(Image, Function)) {
    if (!Entry)
      return;
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops) {
        if (!registration_abi::chargeCalleeWork(WorkUsed, 1)) {
          Entry.reset();
          Exits.clear();
          return;
        }
        if (Op.Opcode == NdOp::CALL && Op.Addr != Entry->CallVA)
          if (const auto Exit = coff_loader::getCheckedX86SEH4EpilogueCall(
                  Image, *Function.ExceptionMetadata, Op.Addr)) {
            if (Exits.size() == limits::kMaxRegistrationEHRecords) {
              Entry.reset();
              Exits.clear();
              return;
            }
            Exits.push_back(*Exit);
          }
      }
  }
};

struct SEHCookieInputs {
  va_t CookieVA = 0;
  va_t CheckVA = 0;
};

// Authenticate image identities here. The state solver remains responsible
// for initialization, slot separation and the live cookie at each transfer.
SEHCookieInputs getSEHCookieInputs(const BinaryImage &Image,
                                   const ExceptionFunction &EH) {
  SEHCookieInputs Inputs;
  if (EH.Personality != ExceptionPersonality::ExceptHandler4 ||
      !EH.Registration)
    return Inputs;
  if (Image.DynInfo.SecurityCookieRVA && Image.Base <= UINT32_MAX - 3 &&
      Image.DynInfo.SecurityCookieRVA <= UINT32_MAX - 3 - Image.Base)
    Inputs.CookieVA = Image.Base + Image.DynInfo.SecurityCookieRVA;
  if (EH.Registration->GSCookieOffset != -2)
    if (auto Check =
            coff_loader::getCheckedX86EH4CookieCheck(Image, EH.PersonalityVA);
        Check &&
        coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, *Check))
      Inputs.CheckVA = *Check;
  return Inputs;
}
std::optional<BoundCalleeEffect>
proveBoundSEHCalleeImpl(const BinaryImage &Image, const BinaryImage &CallImage,
                        const LowOp &Op, const FrameState &Parent, size_t &Work,
                        const std::map<int32_t, SEHJumpCapture> *Captures,
                        const std::set<int32_t> *Initialized,
                        OriginalImageFrameProof *OriginalImage) {
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4 || Op.Seq < 0 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  const va_t Target = Op.Inputs[0].Offset;
  const auto *EH = Image.ExceptionMetadata.findFunction(Target);
  if (!EH || !EH->Registration || EH->Cxx || EH->Registration->RealignedFrame ||
      EH->CodeRange.Begin != Target ||
      (EH->Personality != ExceptionPersonality::ExceptHandler3 &&
       EH->Personality != ExceptionPersonality::ExceptHandler4))
    return std::nullopt;
  if (!registration_abi::chargeCalleeWork(Work, 5))
    return std::nullopt;
  const auto Call = readImmutableCodeBytes(CallImage, Op.Addr, 5);
  if (!Call || (*Call)[0] != 0xe8 || Op.Addr > UINT32_MAX - 5 ||
      uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Call->data() + 1))) !=
          Target)
    return std::nullopt;
  const auto SP =
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP < INT32_MIN + 8 || *SP > INT32_MAX - 128)
    return std::nullopt;
  const int64_t Base = int64_t(*SP) - 8;
  ConditionalFrame Inputs;
  Inputs.Work = Work;
  auto AccountWork = llvm::scope_exit([&] { Work = Inputs.Work; });
  for (int32_t Arg = 8;
       Arg <= ::neverd::limits::ir::
                  kRegistrationConditionalCalleeProveBoundSEHCalleeArgLimit;
       Arg += 4) {
    const auto Cell = registration_abi::readCallerFrameWord(
        Parent, *SP + Arg - 8, Initialized, Inputs.Work);
    if (!Cell)
      continue;
    const auto &Value = *Cell;
    if (!Value.MayBeFrame) {
      Inputs.Arguments.emplace(Arg, Value);
      continue;
    }
    if (!Value.Offset || Value.ReturnPC || Value.CallbackAddress ||
        Value.SEHException)
      continue;
    if (Captures)
      if (const auto Capture = Captures->find(*Value.Offset);
          Capture != Captures->end() &&
          hasSEHJumpCapture(Parent, Capture->second)) {
        FrameValue Buffer;
        Buffer.MayBeFrame = true;
        Buffer.JumpCapture = Capture->second.Call;
        Inputs.Arguments.emplace(Arg, Buffer);
        Inputs.JumpBuffers.emplace(Capture->second.Call, Capture->second);
        continue;
      }
    const auto Object = registration_abi::readCallerFrameWord(
        Parent, *Value.Offset, Initialized, Inputs.Work);
    const int64_t Offset = int64_t(*Value.Offset) - Base;
    if (Captures &&
        !registration_abi::chargeCalleeWork(Inputs.Work, Captures->size()))
      return std::nullopt;
    if (Captures && llvm::any_of(*Captures, [&](const auto &Entry) {
          const int64_t Begin = Entry.second.BufferOffset;
          return int64_t(*Value.Offset) <
                     Begin + RegistrationSEHJumpCapture::BufferBytes &&
                 Begin < int64_t(*Value.Offset) + 4;
        }))
      continue;
    if (!Object || Object->MayBeFrame || *Value.Offset < *SP + 128 ||
        Offset > INT32_MAX - 4 || Offset < 136 || *Value.Offset >= -24 ||
        int64_t(*Value.Offset) + 4 > -24)
      continue;
    Inputs.Arguments.emplace(Arg, FrameValue::frame(int32_t(Offset)));
    Inputs.Objects.push_back({int32_t(Offset), int32_t(Offset + 4)});
    Inputs.ObjectCells.emplace(int32_t(Offset), *Object);
  }
  if (Inputs.Arguments.empty())
    return std::nullopt;
  if (OriginalImage) {
    Inputs.EvaluateOriginalImage =
        [&](const LowOp &Op, const FrameTransfer &Transfer, size_t &Used,
            bool Cookie) -> std::optional<FrameValue> {
      if (Cookie) {
        if (!OriginalImage->recordCookieRead(Op, Transfer, Used))
          Inputs.Complete = false;
        return std::nullopt;
      }
      return OriginalImage->evaluate(Op, Transfer, Used);
    };
    Inputs.ProveImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                                  size_t &Used) {
      return !Write && OriginalImage->permitsRead(Address, Width, Used);
    };
  }
  Inputs.ProveCall = [&](const LowOp &Call, const FrameState &State,
                         const std::set<int32_t> &Initialized,
                         std::optional<FrameValue> &Returned) {
    if (OriginalImage) {
      BoundPlainCalleeEffect Plain;
      if (!registration_abi::hasConditionalCallFrame(
              Image, Call, State, Inputs.Work, &Initialized, &Returned, &Plain,
              OriginalImage))
        return false;
      return true;
    }
    return registration_abi::hasConditionalCallFrame(
        Image, Call, State, Inputs.Work, &Initialized, &Returned);
  };
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  RegistrationCallCalleeIndex Index(Image, &Inputs.Work);
  CFGBuilder Builder;
  Builder.setRegistrationCalleeIndex(&Index);
  const auto Function =
      Builder.buildIntegerFrameProof(Image, Dec, Target, "bound-seh-callee");
  if (!Function.hasCompleteLiftCoverage() ||
      !registration_abi::chargeDecodedCallee(Inputs.Work, Function))
    return std::nullopt;
  for (const auto &Block : Function.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries) {
      if (!registration_abi::chargeCalleeWork(Inputs.Work, 1) ||
          !registration_abi::hasImmutableCalleeInstruction(Image, Insn, Dec))
        return std::nullopt;
      if (Insn.Control == LowInstructionControl::Return) {
        const auto Byte = readImmutableCodeBytes(Image, Insn.Address, 1);
        if (!Byte || (*Byte)[0] != 0xc3 || Insn.Size != 1)
          return std::nullopt;
      }
    }
  if (OriginalImage && !OriginalImage->recordCode(Function, Inputs.Work))
    return std::nullopt;
  const auto Stacks = Index.stackContracts(Function);
  const auto Unwinds = Index.localUnwindContracts(Function);
  const auto Controls = Index.sehControlContracts(Function);
  if (!Stacks || !Unwinds || !Controls)
    return std::nullopt;
  SEHJumpRuntime Jumps;
  bool HasInterlocked = false, HasSleep = false;
  for (const auto &Imported : Image.Imports) {
    const bool Atomic = registration_abi::isCheckedRegistrationInterlockedSlot(
        Image, Imported.IATAddr);
    const bool Sleep = registration_abi::isCheckedRegistrationSleepSlot(
        Image, Imported.IATAddr);
    HasInterlocked |= Atomic;
    HasSleep |= Sleep;
    if (checkedJumpRuntime(Image, Imported.IATAddr, true) ||
        isCheckedControlFPSlot(Image, Imported.IATAddr) ||
        registration_abi::isCheckedRegistrationStartupInfoSlot(
            Image, Imported.IATAddr) ||
        registration_abi::isCheckedRegistrationHeapTerminationSlot(
            Image, Imported.IATAddr) ||
        Atomic || Sleep ||
        registration_abi::isCheckedRegistrationRaiseSlot(Image,
                                                         Imported.IATAddr))
      Jumps.ImportSlots.insert(Imported.IATAddr);
  }
  Jumps.Resume = [&](const LowOp &Resume, size_t &Used) {
    const auto Binding = checkedJumpRuntimeCall(Image, Function, Resume, Used);
    return Binding && Binding->Routine == JumpRuntimeBinding::Kind::Resume;
  };
  Jumps.Raise = [&](const LowOp &Raise, const FrameState &State, size_t &Used) {
    return registration_abi::hasClosedRegistrationRaise(Image, Raise, State) &&
           registration_abi::chargeCalleeWork(Used, 10);
  };
  Jumps.ControlFP = [&](const LowOp &Op, size_t &Used) {
    return isCheckedControlFPCall(Image, Op, Used);
  };
  Jumps.StartupInfo = [&](const LowOp &Op, size_t &Used) {
    return registration_abi::isCheckedRegistrationStartupInfoCall(Image, Op,
                                                                  Used);
  };
  Jumps.HeapTermination = [&](const LowOp &Op, size_t &Work) {
    return registration_abi::isCheckedRegistrationHeapTerminationCall(Image, Op,
                                                                      Work);
  };
  if (HasInterlocked)
    Jumps.Interlocked = [&](const LowOp &Op, size_t &Work) {
      return registration_abi::isCheckedRegistrationInterlockedCall(Image, Op,
                                                                    Work);
    };
  if (HasSleep)
    Jumps.Sleep = [&](const LowOp &Op, size_t &Work) {
      return registration_abi::isCheckedRegistrationSleepCall(Image, Op, Work);
    };
  Jumps.ImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                          size_t &Used) {
    return registration_abi::hasSEHCallbackImageMemory(Image, Address, Width,
                                                       Write, Used);
  };
  const auto Cookies = getSEHCookieInputs(Image, *EH);
  const SEH4EntryCalls Shared(Image, Function);
  if (!registration_abi::chargeCalleeWork(Inputs.Work, Shared.WorkUsed))
    return std::nullopt;
  RegistrationStateSolver Solver(
      Function, Cookies.CookieVA, Cookies.CheckVA, nullptr, nullptr, &*Stacks,
      &*Unwinds, &*Controls, &Inputs, nullptr, &Jumps, nullptr, nullptr,
      nullptr, Shared.Entry ? &*Shared.Entry : nullptr, &Shared.Exits);
  const auto States = Solver.run();
  if (!registration_abi::chargeCalleeWork(Inputs.Work, Solver.workUsed()))
    return std::nullopt;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      !Inputs.Complete ||
      (EH->Personality == ExceptionPersonality::ExceptHandler4 &&
       !States.SecurityCookiesComplete))
    return std::nullopt;
  if (OriginalImage && Shared.Entry) {
    if (!OriginalImage->recordProtocol(Shared.Entry->Caller,
                                       Shared.Entry->AbsoluteOperands,
                                       Inputs.Work) ||
        !OriginalImage->recordProtocol(
            Shared.Entry->Helper, Shared.Entry->AbsoluteOperands, Inputs.Work))
      return std::nullopt;
    for (const auto &Exit : Shared.Exits)
      if (!OriginalImage->recordProtocol(Exit.Caller, {}, Inputs.Work) ||
          !OriginalImage->recordProtocol(Exit.Helper, {}, Inputs.Work))
        return std::nullopt;
  }
  // Filter/finally returns belong to runtime invocations. They cannot prove
  // an ordinary returning call when the helper's own epilogue does not return.
  bool HasParentReturn = false;
  for (const auto &Block : Function.Blocks) {
    const auto State = llvm::find_if(States.Blocks, [&](const auto &Record) {
      return Record.BlockId == Block.Id && Record.Reached &&
             !Record.CallbackOnly;
    });
    if (State == States.Blocks.end())
      continue;
    HasParentReturn |= llvm::any_of(Block.Ops, [](const LowOp &Return) {
      return Return.Opcode == NdOp::RETURN;
    });
  }
  if (!HasParentReturn && Inputs.JumpResumes.empty())
    return std::nullopt;
  BoundCalleeEffect Effect;
  Effect.HasOrdinaryReturn = HasParentReturn;
  if (HasParentReturn != !Inputs.ReturnValues.empty() ||
      !registration_abi::chargeCalleeWork(Inputs.Work,
                                          Inputs.ReturnValues.size()))
    return std::nullopt;
  for (const auto &[Identity, Value] : Inputs.ReturnValues) {
    const auto Projected = projectX86CallReturnValue(Value);
    Effect.ReturnValue =
        Effect.ReturnValue ? join(*Effect.ReturnValue, Projected) : Projected;
  }
  for (const auto &[Identity, Resume] : Inputs.JumpResumes)
    Effect.JumpResumes.push_back(Resume);
  for (const auto &[Begin, End] : Inputs.Reads)
    Effect.Reads.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  for (const auto &[Begin, End] : Inputs.Writes)
    Effect.Writes.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  if (Inputs.NativeFramePrivate) {
    RegistrationSEHBoundCall Native;
    Native.Address = Op.Addr;
    Native.OpSeq = Op.Seq;
    Native.TargetVA = Target;
    Native.StackOffset = *SP;
    Native.HasOrdinaryReturn = HasParentReturn;
    if (!OriginalImage && Effect.ReturnValue && Effect.ReturnValue->Constant &&
        *Effect.ReturnValue ==
            FrameValue::constant(*Effect.ReturnValue->Constant))
      Native.ConstantReturn = Effect.ReturnValue->Constant;
    if (!Inputs.UsedArguments.empty())
      Native.Arguments.resize((*Inputs.UsedArguments.rbegin() - 8) / 4 + 1);
    for (const auto Offset : Inputs.UsedArguments) {
      const auto &Value = Inputs.Arguments.at(Offset);
      auto &Argument = Native.Arguments[(Offset - 8) / 4];
      using Kind = RegistrationSEHBoundArgument::Kind;
      if (Value.JumpCapture) {
        Argument.ArgumentKind = Kind::JumpBuffer;
        Argument.Capture = *Value.JumpCapture;
      } else if (Value.Offset) {
        Argument.ArgumentKind = Kind::Object;
        Argument.Offset = int32_t(Base + *Value.Offset);
      } else {
        Argument.ArgumentKind = Kind::Scalar;
        Argument.Offset = *SP + Offset - 8;
      }
    }
    Effect.NativeCall = std::move(Native);
  }
  return Effect;
}
} // namespace

std::optional<BoundCalleeEffect>
proveBoundSEHCallee(const BinaryImage &Image, const LowOp &Op,
                    const FrameState &Parent, size_t &Work,
                    const std::map<int32_t, SEHJumpCapture> *Captures,
                    const std::set<int32_t> *Initialized) {
  return proveBoundSEHCalleeImpl(Image, Image, Op, Parent, Work, Captures,
                                 Initialized, nullptr);
}

namespace {
std::optional<OriginalImageSEHInvocation>
proveOriginalImageSEHCallee(const BinaryImage &Image,
                            const BinaryImage &CallImage, const LowOp &Op,
                            const FrameState &Parent, size_t &Work,
                            const std::set<int32_t> *Initialized) {
  OriginalImageFrameProof Original(Image);
  auto Effect = proveBoundSEHCalleeImpl(Image, CallImage, Op, Parent, Work,
                                        nullptr, Initialized, &Original);
  if (!Effect || !Effect->HasOrdinaryReturn || !Effect->NativeCall ||
      !Effect->Reads.empty() || !Effect->Writes.empty() ||
      !Effect->JumpResumes.empty() || !Effect->ReturnValue ||
      !Effect->ReturnValue->Constant ||
      *Effect->ReturnValue !=
          FrameValue::constant(*Effect->ReturnValue->Constant))
    return std::nullopt;
  OriginalImageSEHInvocation Result;
  Result.Call = std::move(*Effect->NativeCall);
  Result.ReturnValue = *Effect->ReturnValue->Constant;
  for (const auto &Argument : Result.Call.Arguments) {
    using Kind = RegistrationSEHBoundArgument::Kind;
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    if (Argument.ArgumentKind == Kind::Unused)
      continue;
    if (Argument.ArgumentKind != Kind::Scalar)
      return std::nullopt;
    const auto Value = registration_abi::readCallerFrameWord(
        Parent, Argument.Offset, Initialized, Work);
    if (!Value)
      return std::nullopt;
    if (Value->ImageAddress) {
      if (*Value != FrameValue::imageAddress(*Value->ImageAddress) ||
          !Original.recordAddress(*Value->ImageAddress, Work))
        return std::nullopt;
    } else if (!Value->Constant ||
               *Value != FrameValue::constant(*Value->Constant))
      return std::nullopt;
    Result.Arguments.emplace(Argument.Offset, *Value);
  }
  if (!registration_abi::chargeCalleeWork(Work, Original.addresses().size() +
                                                    Original.reads().size() +
                                                    Original.code().size()))
    return std::nullopt;
  for (const auto &[Address, Read] : Original.reads()) {
    if (!Read)
      continue;
    if (!registration_abi::chargeCalleeWork(Work, Read->Bytes.size()))
      return std::nullopt;
    Result.Reads.emplace(Address, *Read);
  }
  for (const auto &[Address, Bytes] : Original.code()) {
    if (!registration_abi::chargeCalleeWork(Work, Bytes.size()))
      return std::nullopt;
    Result.Code.emplace(Address, Bytes);
  }
  if (!registration_abi::chargeCalleeWork(
          Work, Original.readAddressIdentities().size()))
    return std::nullopt;
  Result.ReadAddressIdentities = Original.readAddressIdentities();
  if (!registration_abi::chargeCalleeWork(
          Work, Original.protocolOperands().size() +
                    Original.codeOperands().size() +
                    Original.cookieReadAddressIdentities().size()))
    return std::nullopt;
  Result.ProtocolOperands = Original.protocolOperands();
  Result.CodeOperands = Original.codeOperands();
  Result.CookieReadAddressIdentities = Original.cookieReadAddressIdentities();
  for (const auto &[Address, Bytes] : Original.protocolCode()) {
    if (!registration_abi::chargeCalleeWork(Work, Bytes.size() + 1))
      return std::nullopt;
    Result.ProtocolCode.emplace(Address, Bytes);
  }
  Result.Addresses = Original.addresses();
  return Result;
}
} // namespace

std::optional<OriginalImageSEHInvocation>
proveBoundOriginalImageSEHCallee(const BinaryImage &Image, const LowOp &Op,
                                 const FrameState &Parent, size_t &Work,
                                 const std::set<int32_t> *Initialized) {
  return proveOriginalImageSEHCallee(Image, Image, Op, Parent, Work,
                                     Initialized);
}

std::optional<OriginalImageSEHReplay>
replayBoundOriginalImageSEHCallee(const BinaryImage &Original,
                                  const BinaryImage &Candidate, const LowOp &Op,
                                  const FrameState &Parent, size_t &Work,
                                  const std::set<int32_t> *Initialized) {
  const auto IsPE32 = [](const BinaryImage &Image) {
    return Image.Arch == Arch::X86 && Image.Bits == Bitness::Bits32 &&
           Image.Format == BinaryFormat::COFF && !Image.IsRelocatable &&
           Image.Base <= UINT32_MAX;
  };
  if (!IsPE32(Original) || !IsPE32(Candidate) ||
      Original.Base != Candidate.Base)
    return std::nullopt;
  auto Before = proveOriginalImageSEHCallee(Original, Original, Op, Parent,
                                            Work, Initialized);
  if (!Before)
    return std::nullopt;
  auto After = proveOriginalImageSEHCallee(Candidate, Original, Op, Parent,
                                           Work, Initialized);
  if (!After)
    return std::nullopt;
  for (const auto *Proof : {&*Before, &*After}) {
    if (!registration_abi::chargeCalleeWork(
            Work, Proof->Call.Arguments.size() + Proof->Arguments.size() + 1))
      return std::nullopt;
    for (const auto *Code : {&Proof->Code, &Proof->ProtocolCode})
      for (const auto &[Address, Bytes] : *Code)
        if (!registration_abi::chargeCalleeWork(Work, Bytes.size() + 1))
          return std::nullopt;
    if (!registration_abi::chargeCalleeWork(Work,
                                            Proof->ProtocolOperands.size()))
      return std::nullopt;
  }
  if (Before->Call != After->Call || Before->Arguments != After->Arguments ||
      Before->ReturnValue != After->ReturnValue ||
      Before->Code != After->Code ||
      Before->ProtocolCode != After->ProtocolCode ||
      Before->ProtocolOperands != After->ProtocolOperands)
    return std::nullopt;
  return OriginalImageSEHReplay{std::move(*Before), std::move(*After)};
}
} // namespace neverd::registration_state

namespace neverd {
using namespace registration_state;

namespace {
// Active analyzeCheckedX86SEHStates calls on this thread. Bound SEH calls are
// checked only by analyses nested less deeply than the limit.
thread_local unsigned SEHStateAnalysisDepth = 0;
} // namespace

unsigned x86SEHStateAnalysisLevel() {
  return std::min(SEHStateAnalysisDepth,
                  ::neverd::limits::ir::
                      kRegistrationConditionalCalleeMaxBoundCallCheckDepth);
}

namespace {
RegistrationStateAnalysis solveCheckedX86SEHStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *Controls,
    RegistrationCallCalleeIndex *Index,
    const BoundCalleeProver *ScopedProver = nullptr,
    size_t *CumulativeWork = nullptr) {
  unsigned &Depth = SEHStateAnalysisDepth;
  const bool CheckBoundCalls =
      Depth < ::neverd::limits::ir::
                  kRegistrationConditionalCalleeMaxBoundCallCheckDepth;
  ++Depth;
  auto RestoreDepth = llvm::scope_exit([&] { --Depth; });
  BoundCalleeProver Prove =
      [&](const LowOp &Op, const FrameState &State, size_t &Work,
          const std::map<int32_t, SEHJumpCapture> &Captures) {
        // General bound helpers still require complete value cells. The
        // runtime argument byte inventory supplies no helper-borrow authority.
        return proveBoundSEHCallee(Image, Op, State, Work, &Captures);
      };
  SEHPrivateCallProof PrivateCalls(Image, Function);
  SEHJumpRuntime Jumps;
  if (CheckBoundCalls)
    Jumps.PrivateCall = [&](const LowOp &Op, size_t &Work) {
      return PrivateCalls.preservesFrame(Op, Work);
    };
  bool HasInterlocked = false, HasSleep = false;
  for (const auto &Imported : Image.Imports) {
    const bool Atomic = registration_abi::isCheckedRegistrationInterlockedSlot(
        Image, Imported.IATAddr);
    const bool Sleep = registration_abi::isCheckedRegistrationSleepSlot(
        Image, Imported.IATAddr);
    HasInterlocked |= Atomic;
    HasSleep |= Sleep;
    if (checkedJumpRuntime(Image, Imported.IATAddr, true) ||
        isCheckedControlFPSlot(Image, Imported.IATAddr) ||
        registration_abi::isCheckedRegistrationStartupInfoSlot(
            Image, Imported.IATAddr) ||
        registration_abi::isCheckedRegistrationHeapTerminationSlot(
            Image, Imported.IATAddr) ||
        Atomic || Sleep ||
        registration_abi::isCheckedRegistrationRaiseSlot(Image,
                                                         Imported.IATAddr))
      Jumps.ImportSlots.insert(Imported.IATAddr);
  }
  Jumps.Capture = [&](const LowOp &Op, const FrameState &State, bool Installed,
                      size_t &Work) {
    return prepareSEHJumpCapture(Image, Function, Op, State, Installed, Work);
  };
  Jumps.Resume = [&](const LowOp &Op, size_t &Work) {
    const auto Binding = checkedJumpRuntimeCall(Image, Function, Op, Work);
    return Binding && Binding->Routine == JumpRuntimeBinding::Kind::Resume;
  };
  Jumps.Raise = [&](const LowOp &Op, const FrameState &State, size_t &Work) {
    return registration_abi::hasClosedRegistrationRaise(Image, Op, State) &&
           registration_abi::chargeCalleeWork(Work, 10);
  };
  Jumps.ControlFP = [&](const LowOp &Op, size_t &Work) {
    return isCheckedControlFPCall(Image, Op, Work);
  };
  Jumps.StartupInfo = [&](const LowOp &Op, size_t &Work) {
    return registration_abi::isCheckedRegistrationStartupInfoCall(Image, Op,
                                                                  Work);
  };
  Jumps.HeapTermination = [&](const LowOp &Op, size_t &Work) {
    return registration_abi::isCheckedRegistrationHeapTerminationCall(Image, Op,
                                                                      Work);
  };
  if (HasInterlocked)
    Jumps.Interlocked = [&](const LowOp &Op, size_t &Work) {
      return registration_abi::isCheckedRegistrationInterlockedCall(Image, Op,
                                                                    Work);
    };
  if (HasSleep)
    Jumps.Sleep = [&](const LowOp &Op, size_t &Work) {
      return registration_abi::isCheckedRegistrationSleepCall(Image, Op, Work);
    };
  Jumps.ImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                          size_t &Work) {
    return registration_abi::hasSEHCallbackImageMemory(Image, Address, Width,
                                                       Write, Work);
  };
  const auto Cookies =
      Function.ExceptionMetadata
          ? getSEHCookieInputs(Image, *Function.ExceptionMetadata)
          : SEHCookieInputs{};
  const SEH4EntryCalls Shared(Image, Function);
  if (CumulativeWork &&
      !registration_abi::chargeCalleeWork(*CumulativeWork, Shared.WorkUsed))
    return {};
  RegistrationStateSolver Solver(
      Function, Cookies.CookieVA, Cookies.CheckVA, nullptr, nullptr, Stacks,
      LocalUnwinds, Controls, nullptr,
      CheckBoundCalls ? (ScopedProver ? ScopedProver : &Prove) : nullptr,
      &Jumps, nullptr, nullptr, nullptr,
      Shared.Entry ? &*Shared.Entry : nullptr, &Shared.Exits);
  auto States = Solver.run(CumulativeWork ? *CumulativeWork : 0);
  if (CumulativeWork) {
    *CumulativeWork = Solver.workUsed();
    return States;
  }
  if (Index && (!Index->chargeStateWork(Shared.WorkUsed) ||
                !Index->chargeStateWork(Solver.workUsed()))) {
    States.Complete = States.CallbackStatesComplete =
        States.RegistrationLifetimeComplete = false;
    States.SEHInvocationExitsComplete = States.SEHExceptionAccessesComplete =
        false;
    States.SEHInvocationExits.clear();
    States.SEHJumpResumptionsComplete = false;
    States.SEHJumpResumptions.clear();
    States.SEHJumpCapturesComplete = false;
    States.SEHJumpCaptures.clear();
    States.SEHStateStoresComplete = false;
    States.SEHStateStores.clear();
    States.SEHBoundCallsComplete = false;
    States.SEHBoundCalls.clear();
    States.SEHExceptionAccesses.clear();
    States.Diagnostics.push_back("SEH callee state proof budget exhausted");
  }
  return States;
}
} // namespace

RegistrationStateAnalysis analyzeCheckedX86SEHStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *Controls,
    RegistrationCallCalleeIndex *Index) {
  return solveCheckedX86SEHStates(Image, Function, Stacks, LocalUnwinds,
                                  Controls, Index);
}

OriginalImageSEHCallerProof registration_state::analyzeOriginalImageSEHCaller(
    const BinaryImage &Image, const LowFunc &Function, size_t &Work) {
  OriginalImageSEHCallerProof Result;
  if (!registration_abi::chargeCalleeWork(Work, 1) || Image.Arch != Arch::X86 ||
      Image.Format != BinaryFormat::COFF || Image.Bits != Bitness::Bits32 ||
      Image.IsRelocatable || !Function.hasCompleteLiftCoverage() ||
      !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx ||
      !registration_abi::chargeDecodedCallee(Work, Function))
    return Result;
  RegistrationCallCalleeIndex Index(Image, &Work);
  const auto Stacks = Index.stackContracts(Function);
  const auto Unwinds = Index.localUnwindContracts(Function);
  const auto Controls = Index.sehControlContracts(Function);
  if (!Stacks || !Unwinds || !Controls)
    return Result;
  bool Complete = true;
  std::set<std::pair<va_t, int>> OrdinaryInvocations;
  BoundCalleeProver Prove =
      [&](const LowOp &Op, const FrameState &Parent, size_t &Used,
          const std::map<int32_t, SEHJumpCapture> &Captures)
      -> std::optional<BoundCalleeEffect> {
    if (!registration_abi::chargeCalleeWork(
            Used, Result.Invocations.size() + OrdinaryInvocations.size() + 1))
      return std::nullopt;
    const auto Key = std::make_pair(Op.Addr, Op.Seq);
    auto Found = Result.Invocations.find(Key);
    if (Found != Result.Invocations.end()) {
      const auto &Proof = Found->second;
      if (Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Offset != Proof.Call.TargetVA ||
          Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset !=
              Proof.Call.StackOffset) {
        Complete = false;
        return std::nullopt;
      }
      for (const auto &[Offset, Expected] : Proof.Arguments) {
        const auto Actual = registration_abi::readCallerFrameWord(
            Parent, Offset, nullptr, Used);
        if (!Actual || *Actual != Expected) {
          Complete = false;
          return std::nullopt;
        }
      }
    } else {
      auto Proof =
          proveBoundOriginalImageSEHCallee(Image, Op, Parent, Used, nullptr);
      if (!Proof) {
        if (OrdinaryInvocations.size() >= limits::kMaxRegistrationEHRecords) {
          Complete = false;
          return std::nullopt;
        }
        OrdinaryInvocations.insert(Key);
        return proveBoundSEHCallee(Image, Op, Parent, Used, &Captures);
      }
      // A prior ordinary invocation may have different or unknown inputs.
      // A later exact query cannot remove that earlier occurrence from the
      // recipe merely because both paths preserve the same private frame.
      if (OrdinaryInvocations.count(Key))
        Complete = false;
      if (Result.Invocations.size() >= limits::kMaxRegistrationEHRecords) {
        Complete = false;
        return std::nullopt;
      }
      Found = Result.Invocations.emplace(Key, std::move(*Proof)).first;
    }
    BoundCalleeEffect Effect;
    Effect.ReturnValue = FrameValue::constant(Found->second.ReturnValue);
    Effect.NativeCall = Found->second.Call;
    return Effect;
  };
  Result.States = solveCheckedX86SEHStates(Image, Function, &*Stacks, &*Unwinds,
                                           &*Controls, nullptr, &Prove, &Work);
  Result.InvocationsComplete = Complete && Result.States.Complete &&
                               Result.States.CallbackStatesComplete &&
                               Result.States.RegistrationLifetimeComplete &&
                               Result.States.ChainOperationsComplete &&
                               Result.States.SEHBoundCallsComplete &&
                               (Function.ExceptionMetadata->Personality !=
                                    ExceptionPersonality::ExceptHandler4 ||
                                Result.States.SecurityCookiesComplete);
  for (const auto &[Key, Invocation] : Result.Invocations) {
    if (!registration_abi::chargeCalleeWork(
            Work, Result.States.SEHBoundCalls.size() + 1) ||
        !llvm::is_contained(Result.States.SEHBoundCalls, Invocation.Call))
      Result.InvocationsComplete = false;
  }
  return Result;
}

bool RegistrationCallCalleeIndex::chargeStateWork(size_t Amount) {
  return registration_abi::chargeCalleeWork(Work, Amount);
}

} // namespace neverd
