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
#include "RegistrationSEHPrivateCall.h"
#include "RegistrationStateSolver.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::registration_state {

namespace {
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
} // namespace

std::optional<BoundCalleeEffect>
proveBoundSEHCallee(const BinaryImage &Image, const LowOp &Op,
                    const FrameState &Parent, size_t &Work,
                    const std::map<int32_t, SEHJumpCapture> *Captures) {
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4)
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
  const auto Call = readImmutableCodeBytes(Image, Op.Addr, 5);
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
  for (int32_t Arg = 8; Arg <= 128; Arg += 4) {
    const auto Cell = Parent.Cells.find(*SP + Arg - 8);
    if (Cell == Parent.Cells.end())
      continue;
    const auto &Value = Cell->second;
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
    const auto Object = Parent.Cells.find(*Value.Offset);
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
    if (Object == Parent.Cells.end() || Object->second.MayBeFrame ||
        *Value.Offset < *SP + 128 || Offset > INT32_MAX - 4 || Offset < 136 ||
        *Value.Offset >= -24 || int64_t(*Value.Offset) + 4 > -24)
      continue;
    Inputs.Arguments.emplace(Arg, FrameValue::frame(int32_t(Offset)));
    Inputs.Objects.push_back({int32_t(Offset), int32_t(Offset + 4)});
    Inputs.ObjectCells.emplace(int32_t(Offset), Object->second);
  }
  if (Inputs.Objects.empty() && Inputs.JumpBuffers.empty())
    return std::nullopt;
  Inputs.ProveCall = [&](const LowOp &Call, const FrameState &State) {
    return registration_abi::hasConditionalCallFrame(Image, Call, State,
                                                     Inputs.Work);
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
  const auto Stacks = Index.stackContracts(Function);
  const auto Unwinds = Index.localUnwindContracts(Function);
  const auto Controls = Index.sehControlContracts(Function);
  if (!Stacks || !Unwinds || !Controls)
    return std::nullopt;
  SEHJumpRuntime Jumps;
  for (const auto &Imported : Image.Imports)
    if (checkedJumpRuntime(Image, Imported.IATAddr, true) ||
        isCheckedControlFPSlot(Image, Imported.IATAddr) ||
        registration_abi::checkedRegistrationImportStackPop(
            Image, Imported.IATAddr) == 16)
      Jumps.ImportSlots.insert(Imported.IATAddr);
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
  Jumps.ImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                          size_t &Used) {
    return registration_abi::hasSEHCallbackImageMemory(Image, Address, Width,
                                                       Write, Used);
  };
  const auto Cookies = getSEHCookieInputs(Image, *EH);
  RegistrationStateSolver Solver(Function, Cookies.CookieVA, Cookies.CheckVA,
                                 nullptr, nullptr, &*Stacks, &*Unwinds,
                                 &*Controls, &Inputs, nullptr, &Jumps);
  const auto States = Solver.run();
  if (!registration_abi::chargeCalleeWork(Inputs.Work, Solver.workUsed()))
    return std::nullopt;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      !Inputs.Complete ||
      (EH->Personality == ExceptionPersonality::ExceptHandler4 &&
       !States.SecurityCookiesComplete))
    return std::nullopt;
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
} // namespace neverd::registration_state

namespace neverd {
using namespace registration_state;

RegistrationStateAnalysis analyzeCheckedX86SEHStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *Controls,
    RegistrationCallCalleeIndex *Index) {
  static thread_local unsigned Depth = 0;
  const bool CheckBoundCalls = Depth < 4;
  ++Depth;
  auto RestoreDepth = llvm::scope_exit([&] { --Depth; });
  BoundCalleeProver Prove =
      [&](const LowOp &Op, const FrameState &State, size_t &Work,
          const std::map<int32_t, SEHJumpCapture> &Captures) {
        return proveBoundSEHCallee(Image, Op, State, Work, &Captures);
      };
  SEHPrivateCallProof PrivateCalls(Image, Function);
  SEHJumpRuntime Jumps;
  if (CheckBoundCalls)
    Jumps.PrivateCall = [&](const LowOp &Op, size_t &Work) {
      return PrivateCalls.preservesFrame(Op, Work);
    };
  for (const auto &Imported : Image.Imports)
    if (checkedJumpRuntime(Image, Imported.IATAddr, true) ||
        isCheckedControlFPSlot(Image, Imported.IATAddr) ||
        registration_abi::checkedRegistrationImportStackPop(
            Image, Imported.IATAddr) == 16)
      Jumps.ImportSlots.insert(Imported.IATAddr);
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
  Jumps.ImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                          size_t &Work) {
    return registration_abi::hasSEHCallbackImageMemory(Image, Address, Width,
                                                       Write, Work);
  };
  const auto Cookies =
      Function.ExceptionMetadata
          ? getSEHCookieInputs(Image, *Function.ExceptionMetadata)
          : SEHCookieInputs{};
  RegistrationStateSolver Solver(Function, Cookies.CookieVA, Cookies.CheckVA,
                                 nullptr, nullptr, Stacks, LocalUnwinds,
                                 Controls, nullptr,
                                 CheckBoundCalls ? &Prove : nullptr, &Jumps);
  auto States = Solver.run();
  if (Index && !Index->chargeStateWork(Solver.workUsed())) {
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

bool RegistrationCallCalleeIndex::chargeStateWork(size_t Amount) {
  return registration_abi::chargeCalleeWork(Work, Amount);
}

} // namespace neverd
