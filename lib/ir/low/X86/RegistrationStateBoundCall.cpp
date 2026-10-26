//===- RegistrationStateBoundCall.cpp - Original SEH helper transfers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind original helper effects to one checked caller occurrence.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::transferBoundSEHCallee(
    size_t I, const Domain &Before, Domain &After, const LowOp &Op,
    BlockFacts &Facts, std::optional<FrameValue> &CallSP,
    std::optional<Domain> &CallException,
    std::optional<FrameValue> &CallResult) {
  const auto &Block = Function.Blocks[I];
  if (!CallSP && BoundCallees && !KnownCxx && !After.Unknown &&
      !Facts.Invalid && After.Parent && !After.Callback &&
      After.JumpBindingsIntact && !After.Frame.CallbackEntry &&
      After.Installed && !After.Uninstalled &&
      (!EH4 || (CompleteCookies && cookiesReady(After.Frame))) &&
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset ==
          0 &&
      Chain.RegistrationOffset == -16 && Chain.TryLevelOffset == -4 &&
      !Chain.RealignedFrame && Function.hasCompleteLiftCoverage() &&
      Op.Opcode == NdOp::CALL &&
      !FinallyTargets.count(Op.NumInputs && Op.Inputs[0].isConst()
                                ? Op.Inputs[0].Offset
                                : InvalidVA))
    if (const auto Source = Boundaries.find(Op.Addr);
        Op.Seq >= 0 && Source != Boundaries.end() &&
        Source->second.first == Block.Id &&
        Source->second.second.Control == LowInstructionControl::Call &&
        Op.NumInputs == 1 && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
        Source->second.second.Immediate == Op.Inputs[0].Offset &&
        size_t(&Op - Block.Ops.data()) >= Source->second.second.FirstOp &&
        size_t(&Op - Block.Ops.data()) - Source->second.second.FirstOp <
            Source->second.second.OpCount)
      if (auto Effect =
              (*BoundCallees)(Op, After.Frame, WorkUsed, After.JumpCaptures)) {
        const auto SP = parentStackOffset(After);
        if (SP && int64_t(*SP) + Effect->StackPopBytes <= INT32_MAX) {
          // A conditional helper proves its borrowed accesses in caller
          // coordinates. The caller owns the registration and cookie layout;
          // no borrowed object may expose those cells, including partial
          // overlaps or an address argument that the helper never dereferences.
          auto PrivateObject = [&](int64_t Begin, int64_t End) {
            return charge(1) && Begin >= *SP && End <= 0 &&
                   End - Begin <= UINT16_MAX &&
                   frameObjectIsDisjointFromAdministration(Begin, End);
          };
          auto PrivateExtent = [&](const RegistrationObjectExtent &Extent) {
            return PrivateObject(Extent.Begin, Extent.End);
          };
          if (!llvm::all_of(Effect->Reads, PrivateExtent) ||
              !llvm::all_of(Effect->Writes, PrivateExtent))
            return true;
          if (Effect->NativeCall)
            for (const auto &Argument : Effect->NativeCall->Arguments)
              if (Argument.ArgumentKind ==
                      RegistrationSEHBoundArgument::Kind::Object &&
                  !PrivateObject(Argument.Offset, int64_t(Argument.Offset) + 4))
                return true;
          Result.HasBoundSEHCallees = true;
          if (Effect->NativeCall) {
            const auto [It, New] = BoundCalls.emplace(
                std::make_pair(Op.Addr, Op.Seq), *Effect->NativeCall);
            if (!New) {
              // The same physical call can have several source invocations.
              // Different ordinary values lose the constant-return fact, not
              // an otherwise identical private argument/frame contract.
              if (It->second.ConstantReturn !=
                  Effect->NativeCall->ConstantReturn)
                It->second.ConstantReturn.reset();
              Effect->NativeCall->ConstantReturn = It->second.ConstantReturn;
              if (It->second != *Effect->NativeCall)
                Result.SEHBoundCallsComplete = false;
            }
          } else
            Result.SEHBoundCallsComplete = false;
          CallSP =
              FrameValue::frame(int32_t(int64_t(*SP) + Effect->StackPopBytes));
          if (Effect->HasOrdinaryReturn && hasX86ScalarCallOutput(Op))
            CallResult = Effect->ReturnValue;
          if (!Effect->Writes.empty()) {
            if (!charge(Before.Frame.cellCount() + 1))
              return false;
            if (!CallException)
              CallException = Before;
          }
          for (const auto &Write : Effect->Writes) {
            const auto Width = uint16_t(Write.End - Write.Begin);
            invalidateJumpCaptures(After, FrameValue::frame(Write.Begin),
                                   Width);
            After.Frame.store(Write.Begin, Width, {});
            // A fault can dispatch the caller after a partial helper effect.
            // Do not lend pre-call object contents to its filters.
            CallException->Frame.store(Write.Begin, Width, {});
            invalidateJumpCaptures(*CallException,
                                   FrameValue::frame(Write.Begin), Width);
          }
          for (const auto &Resume : Effect->JumpResumes) {
            const auto Capture =
                llvm::find_if(After.JumpCaptures, [&](const auto &Item) {
                  return Item.second.Call == Resume.CaptureCall;
                });
            if (!After.JumpBindingsIntact ||
                Capture == After.JumpCaptures.end() ||
                !hasSEHJumpCapture(After.Frame, Capture->second)) {
              Facts.Invalid = After.Unknown = true;
              After.Levels.clear();
              Result.SEHJumpResumptionsComplete = false;
              break;
            }
            BlockFacts ResumeFacts = Facts;
            resumeJumpCapture(After, ResumeFacts, Op, Capture->second,
                              Resume.Value, Op.Inputs[0].Offset,
                              Resume.Address);
            Facts.Invalid |= ResumeFacts.Invalid;
          }
          if (!Effect->HasOrdinaryReturn && !Effect->JumpResumes.empty())
            Facts.InvocationAtExit = true;
        }
      }
  return true;
}

} // namespace neverd::registration_state
