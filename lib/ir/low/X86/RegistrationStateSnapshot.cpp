//===- RegistrationStateSnapshot.cpp - SEH snapshot lookup ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Hashes source snapshots for lookup; full Domain equality proves identity.

#include "RegistrationStateSolver.h"

#include "llvm/ADT/Hashing.h"

namespace neverd::registration_state {

using llvm::hash_code;
using llvm::hash_combine;
using llvm::hash_combine_range;

static hash_code hash_value(const CallbackFrameAddress &Address) {
  return hash_combine(Address.Entry, Address.Offset);
}

static hash_code hash_value(const SEHExceptionAddress &Address) {
  return hash_combine(Address.Storage, Address.Filter, Address.Offset);
}

static hash_code hash_value(const SEHThreadAddress &Address) {
  return hash_combine(Address.RootAddress, Address.RootOpSeq, Address.Offset);
}

static hash_code hash_value(const FrameValue &Value) {
  return hash_combine(
      Value.Offset, Value.Constant, Value.PreviousChain, Value.MayBeFrame,
      Value.FrameOnlyFromCall, Value.SavedRegister, Value.ReturnPC,
      Value.SecurityCookie, Value.CookieFrameOffset, Value.CookieXOR,
      Value.EntryOffset, Value.CallbackAddress, Value.ExceptionObject,
      Value.SEHException, Value.JumpCapture, Value.FrameOnlyFromSEHScalar,
      Value.ImageAddress, Value.CookieImageXOR, Value.ImportTarget,
      Value.ThreadAddress);
}

static hash_code hash_value(const FrameState &Frame) {
  return hash_combine(
      hash_combine_range(Frame.Registers), Frame.ImportBindingsIntact,
      hash_combine_range(Frame.OtherRegisterBytes),
      Frame.OtherRegistersMayBeFrame, hash_combine_range(Frame.Cells),
      hash_combine_range(Frame.EntryCells),
      hash_combine_range(Frame.CallbackCells),
      hash_combine_range(Frame.InitializedCallbackBytes), Frame.CallbackEntry,
      Frame.SEHFilterEntry);
}

size_t FrameState::snapshotHash() const { return hash_value(*this); }

static hash_code hash_value(const SEHActivation &Activation) {
  return hash_combine(Activation.Entry, Activation.SearchFilter,
                      Activation.OrdinaryCall,
                      hash_combine_range(Activation.JumpCaptureCalls),
                      hash_combine_range(Activation.Callers));
}

static hash_code hash_value(const Domain::ParentStackRestore &Restore) {
  return hash_combine(Restore.Address, Restore.Seq, Restore.StackOffset);
}

static hash_code hash_value(const Domain::SEHCaller &Caller) {
  return hash_combine(Caller.Activation, Caller.Stack, Caller.OtherCallback,
                      Caller.OrdinaryCallStack, Caller.StackRestore);
}

static hash_code hash_value(const SEHJumpCapture &Capture) {
  return hash_combine(Capture.Call, Capture.ReturnPC, Capture.UnwindTarget,
                      Capture.BufferOffset, Capture.StackOffset, Capture.Level,
                      unsigned(Capture.Protocol), Capture.SecurityCookieVA,
                      hash_combine_range(Capture.Nonvolatile));
}

static hash_code hash_value(const CxxCatchContext &Context) {
  return hash_combine(Context.TryIndex, Context.CatchIndex,
                      Context.SavedStackOffset, Context.SavedCallback,
                      Context.SuspendedCallback, Context.SuspendedStackOffset,
                      hash_combine_range(Context.SuspendedCells),
                      hash_combine_range(Context.SuspendedInitializedBytes));
}

size_t Domain::snapshotHash() const {
  hash_code Catches = llvm::hash_value(CxxCatchStacks.size());
  for (const auto &Stack : CxxCatchStacks)
    Catches = hash_combine(Catches, hash_combine_range(Stack));
  return hash_combine(Activation, hash_combine_range(Levels), Frame,
                      RuntimeObject, hash_combine_range(InitializedFrameBytes),
                      Reached, Unknown, Parent, Callback, OtherCallback,
                      Catches, Uninstalled, Installed, AbandonedSEHGuard,
                      CanDispatch, CxxDispatchBindingsIntact, OrdinaryCallStack,
                      StackRestore, hash_combine_range(JumpCaptures),
                      hash_combine_range(SEHCallers), JumpBindingsIntact);
}

} // namespace neverd::registration_state
