//===- TableSEHSourceScopeTests.cpp - Table SEH scopes for printed C -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The scopes a `__C_specific_handler` table gives the IR that LLVM C prints:
/// rows over one range, which nest by their order in the table, and the
/// termination handler calls of a local unwind, each of which runs outside
/// the scope it ends and inside the scopes around that one.  And the C that
/// IR prints: each block inside the scopes it runs in, with no `__try` around
/// a block that only jumps.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t ParentVA = 0x140001000;
constexpr va_t InnerFinallyVA = 0x140002000;
constexpr va_t OuterFinallyVA = 0x140002100;
constexpr va_t MiddleFinallyVA = 0x140002200;
constexpr va_t MayThrowVA = 0x140003000;
constexpr va_t PersonalityVA = 0x140004000;
constexpr va_t CounterVA = 0x140005000;
constexpr va_t GuardedBegin = ParentVA + 0x10;
constexpr va_t GuardedEnd = ParentVA + 0x30;
constexpr va_t UnwindCallVA = ParentVA + 0x24;
constexpr va_t ExceptVA = ParentVA + 0x40;
constexpr va_t TargetVA = ParentVA + 0x50;

MedOp branchTo(va_t From, va_t To) {
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = From;
  Branch.addInput(MedVar::makeConst(To, 8));
  return Branch;
}

MedOp callOf(va_t From, va_t Callee) {
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = From;
  Call.addInput(MedVar::makeConst(Callee, 8));
  return Call;
}

MedOp returnAt(va_t From) {
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = From;
  return Return;
}

MedBlock blockAt(int Id, va_t Begin, va_t End) {
  MedBlock Block;
  Block.Id = Id;
  Block.StartAddr = Begin;
  Block.EndAddr = End;
  return Block;
}

/// A termination handler that reads nothing of its parent: it counts a run.
/// A termination handler that stores \p Step and returns, or, unless
/// \p Returns, never does: a `__finally` that leaves by `longjmp`, here a
/// loop.
MedFunc terminationHandler(va_t Entry, const char *Name, uint64_t Step,
                           bool Returns = true) {
  MedFunc Handler;
  Handler.Entry = Entry;
  Handler.Name = Name;
  Handler.ReturnType = NdType::makeVoid();
  Handler.DoesNotReturn = !Returns;
  MedBlock Body = blockAt(0, Entry, Entry + 8);
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Addr = Entry;
  Store.addInput(MedVar::makeConst(CounterVA, 8));
  Store.addInput(MedVar::makeConst(Step, 4));
  Body.Ops.push_back(std::move(Store));
  if (Returns) {
    Body.Ops.push_back(returnAt(Entry + 4));
  } else {
    Body.Ops.push_back(branchTo(Entry + 4, Entry));
    Body.Preds = {0};
    Body.Succs = {0};
  }
  Handler.Blocks.push_back(std::move(Body));
  return Handler;
}

MedFunc personality() {
  MedFunc Personality;
  Personality.Entry = PersonalityVA;
  Personality.Name = "\01__C_specific_handler";
  MedBlock Body = blockAt(0, PersonalityVA, PersonalityVA + 1);
  Body.Ops.push_back(returnAt(PersonalityVA));
  Personality.Blocks.push_back(std::move(Body));
  return Personality;
}

SEHScopeRecord finallyRow(va_t Handler) {
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {GuardedBegin, GuardedEnd};
  Scope.Kind = SEHScopeKind::Finally;
  Scope.FilterOrFinallyVA = Handler;
  Scope.HandlerVA = Handler;
  return Scope;
}

SEHScopeRecord exceptRow() {
  SEHScopeRecord Scope;
  Scope.ParseStatus = ExceptionParseStatus::Complete;
  Scope.GuardedRange = {GuardedBegin, GuardedEnd};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = ExceptVA;
  Scope.ContinuationVA = ExceptVA;
  return Scope;
}

/// What the block that leaves the three scopes does once it has called
/// may_throw.
enum class Leave {
  /// It branches to the target: nothing unwinds.
  Branch,
  /// A local unwind to the target, spelled as MedIR spells it: a call of the
  /// inner and then of the outer termination handler at the runtime call's
  /// address, and a branch.
  LocalUnwind,
  /// The same, with a third call after the two that belongs to no handler.
  LocalUnwindThenCall,
  /// The two handler calls in the order the runtime does not run them.
  LocalUnwindOuterFirst,
  /// It branches to the target, which branches to itself: an empty loop
  /// after the three scopes.
  Spin,
};

/// How the table's language handler is reached.
enum class Cookie {
  /// `__C_specific_handler` itself.
  None,
  /// `__GSHandlerCheck_SEH`, which checks the frame's stack cookie and then
  /// calls it for exceptions and for unwinds, as its record says.
  Checked,
  /// The same, with a record that keeps exceptions from the handler: the
  /// table's `__except` never runs.
  CheckedForUnwindsOnly,
  /// The same, with a record that keeps unwinds from the handler: the table's
  /// `__finally` rows never run for one.
  CheckedForExceptionsOnly,
};

/// MSVC's table for
///   __try { __try { __try { body } __finally { inner } }
///           __except (1) { handler } } __finally { outer }
/// when the three bodies begin and end together: three rows over one range,
/// innermost first.  Block 1 calls may_throw inside it and block 2 leaves it.
/// With \p MiddleIsFinally the middle scope is a third `__finally`, which a
/// local unwind runs between the other two.
/// \p NeverReturns names a termination handler that does not return.
std::vector<MedFunc>
threeScopesOverOneRange(Leave How, bool MiddleIsFinally = false,
                        bool BodyCalls = true, Cookie Wrapped = Cookie::None,
                        llvm::StringRef NeverReturns = {}) {
  MedFunc Parent;
  Parent.Entry = ParentVA;
  Parent.Name = "three_scopes_parent";
  Parent.ReturnType = NdType::makeVoid();

  MedBlock Entry = blockAt(0, ParentVA, GuardedBegin);
  Entry.Ops.push_back(branchTo(ParentVA + 8, GuardedBegin));
  Entry.Succs = {1};

  MedBlock Body = blockAt(1, GuardedBegin, ParentVA + 0x20);
  if (BodyCalls)
    Body.Ops.push_back(callOf(GuardedBegin + 4, MayThrowVA));
  Body.Ops.push_back(branchTo(GuardedBegin + 8, ParentVA + 0x20));
  Body.Preds = {0};
  Body.Succs = {2};

  const bool Unwinds = How != Leave::Branch && How != Leave::Spin;
  MedBlock Leaving = blockAt(2, ParentVA + 0x20, GuardedEnd);
  if (Unwinds) {
    const bool OuterFirst = How == Leave::LocalUnwindOuterFirst;
    Leaving.Ops.push_back(
        callOf(UnwindCallVA, OuterFirst ? OuterFinallyVA : InnerFinallyVA));
    if (MiddleIsFinally)
      Leaving.Ops.push_back(callOf(UnwindCallVA, MiddleFinallyVA));
    Leaving.Ops.push_back(
        callOf(UnwindCallVA, OuterFirst ? InnerFinallyVA : OuterFinallyVA));
    if (How == Leave::LocalUnwindThenCall)
      Leaving.Ops.push_back(callOf(UnwindCallVA + 4, MayThrowVA));
  }
  Leaving.Ops.push_back(branchTo(ParentVA + 0x2c, TargetVA));
  Leaving.Preds = {1};
  Leaving.Succs = {4};

  MedBlock Except = blockAt(3, ExceptVA, TargetVA);
  Except.Ops.push_back(returnAt(ExceptVA + 8));

  MedBlock Target = blockAt(4, TargetVA, ParentVA + 0x60);
  Target.Preds = {2};
  if (How == Leave::Spin) {
    Target.Ops.push_back(branchTo(TargetVA + 8, TargetVA));
    Target.Preds.push_back(4);
    Target.Succs = {4};
  } else {
    Target.Ops.push_back(returnAt(TargetVA + 8));
  }

  Parent.Blocks.push_back(std::move(Entry));
  Parent.Blocks.push_back(std::move(Body));
  Parent.Blocks.push_back(std::move(Leaving));
  Parent.Blocks.push_back(std::move(Except));
  Parent.Blocks.push_back(std::move(Target));

  ExceptionFunction EH;
  EH.CodeRange = {ParentVA, ParentVA + 0x60};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = PersonalityVA;
  if (Wrapped != Cookie::None) {
    EH.Personality = ExceptionPersonality::GSHandlerCheckSEH;
    EH.PersonalityName = "__GSHandlerCheck_SEH";
    GSCookieInfo Record;
    Record.ParseStatus = ExceptionParseStatus::Complete;
    Record.CookieOffset = 0x40;
    Record.HasExceptionHandler = Wrapped != Cookie::CheckedForUnwindsOnly;
    Record.HasUnwindHandler = Wrapped != Cookie::CheckedForExceptionsOnly;
    Record.Payload = {uint8_t(0x40 | (Record.HasExceptionHandler ? 1 : 0) |
                              (Record.HasUnwindHandler ? 2 : 0)),
                      0, 0, 0};
    EH.GSCookie = std::move(Record);
  }
  EH.SEH.emplace();
  EH.SEH->Scopes = {finallyRow(InnerFinallyVA),
                    MiddleIsFinally ? finallyRow(MiddleFinallyVA) : exceptRow(),
                    finallyRow(OuterFinallyVA)};
  Parent.ExceptionMetadata = std::move(EH);

  if (Unwinds) {
    MedSEHLocalUnwindExit Exit;
    Exit.CallAddr = UnwindCallVA;
    Exit.CallSeq = 1;
    Exit.Target = TargetVA;
    Exit.OwnsTarget = true;
    Exit.Handlers = {{InnerFinallyVA, 0}};
    if (MiddleIsFinally)
      Exit.Handlers.push_back({MiddleFinallyVA, 1});
    Exit.Handlers.push_back({OuterFinallyVA, 2});
    Parent.SEHLocalUnwindExits.push_back(std::move(Exit));
  }

  const auto Handler = [&](va_t Entry, const char *Name, uint64_t Step) {
    return terminationHandler(Entry, Name, Step, NeverReturns != Name);
  };
  return {std::move(Parent), Handler(InnerFinallyVA, "inner_finally", 1),
          Handler(OuterFinallyVA, "outer_finally", 2),
          Handler(MiddleFinallyVA, "middle_finally", 3), personality()};
}

std::unique_ptr<llvm::Module> emitUnit(const std::vector<MedFunc> &Unit,
                                       llvm::LLVMContext &Context,
                                       bool ForPrintedC,
                                       LLVMSourceMap *Sources = nullptr) {
  MedLLVMEmitter Emitter;
  Emitter.setPreserveUnknownSourceValues(ForPrintedC);
  Emitter.setAnalysisRecoveryForSource(ForPrintedC);
  Emitter.setSourceMap(Sources);
  auto Module =
      Emitter.emit(Unit, Context, "table_seh_scopes", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::COFF);
  if (Module) {
    std::string Verification;
    llvm::raw_string_ostream OS(Verification);
    EXPECT_FALSE(llvm::verifyModule(*Module, &OS)) << Verification;
  }
  return Module;
}

std::string printed(const llvm::Function &Function) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  Function.print(OS);
  return Text;
}

/// The pads of the rows, found by what each runs.
struct Pads {
  /// A `__finally` row's pad, by the name of its termination handler.
  std::map<std::string, const llvm::BasicBlock *> Cleanup;
  const llvm::BasicBlock *Except = nullptr;
  const llvm::BasicBlock *cleanup(const char *Handler) const {
    const auto Found = Cleanup.find(Handler);
    return Found == Cleanup.end() ? nullptr : Found->second;
  }
};

const llvm::Function *cleanupCallee(const llvm::CleanupPadInst &Pad) {
  for (const llvm::User *User : Pad.users())
    if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(User))
      if (Call->getCalledFunction() &&
          !Call->getCalledFunction()->isIntrinsic())
        return Call->getCalledFunction();
  return nullptr;
}

Pads padsOf(const llvm::Function &Parent) {
  Pads Found;
  for (const llvm::BasicBlock &Block : Parent) {
    if (llvm::isa<llvm::CatchSwitchInst>(Block.getTerminator())) {
      EXPECT_EQ(Found.Except, nullptr);
      Found.Except = &Block;
    }
    const auto *Cleanup =
        llvm::dyn_cast<llvm::CleanupPadInst>(Block.getFirstNonPHIIt());
    if (!Cleanup)
      continue;
    const llvm::Function *Callee = cleanupCallee(*Cleanup);
    if (!Callee) {
      ADD_FAILURE() << "a cleanup pad runs no handler";
      continue;
    }
    EXPECT_TRUE(Found.Cleanup.emplace(Callee->getName().str(), &Block).second)
        << "two pads run " << Callee->getName().str();
  }
  return Found;
}

/// Where what a cleanup pad's handler raises, and its end, go next: the
/// pad of the scope around it, or null for the caller.
const llvm::BasicBlock *unwindOfCleanup(const llvm::BasicBlock &PadBlock) {
  const auto *Pad =
      llvm::cast<llvm::CleanupPadInst>(PadBlock.getFirstNonPHIIt());
  const llvm::BasicBlock *Next = nullptr;
  bool Seen = false;
  auto Note = [&](const llvm::BasicBlock *Dest) {
    EXPECT_TRUE(!Seen || Next == Dest) << "one pad unwinds to two places";
    Seen = true;
    Next = Dest;
  };
  for (const llvm::User *User : Pad->users()) {
    if (const auto *Ret = llvm::dyn_cast<llvm::CleanupReturnInst>(User))
      Note(Ret->getUnwindDest());
    else if (const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(User))
      Note(Invoke->getUnwindDest());
  }
  EXPECT_TRUE(Seen);
  return Next;
}

/// The calls of \p Callee that the function's own code makes, outside pads.
std::vector<const llvm::CallBase *> ownCallsOf(const llvm::Function &Parent,
                                               llvm::StringRef Callee) {
  std::vector<const llvm::CallBase *> Calls;
  for (const llvm::BasicBlock &Block : Parent)
    for (const llvm::Instruction &Inst : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (Call && Call->getCalledFunction() &&
          Call->getCalledFunction()->getName() == Callee &&
          !Call->getOperandBundle(llvm::LLVMContext::OB_funclet))
        Calls.push_back(Call);
    }
  return Calls;
}

// Rows over one range nest by their order, the earlier inside the later: the
// handler's scan meets them in that order.
TEST(TableSEHSourceScopes, RowsOverOneRangeNestInTableOrder) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::Branch);
  const ExceptionFunction &EH = *Unit.front().ExceptionMetadata;
  EXPECT_TRUE(
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::SourceIRLowering)
          .canLowerNativeIR());

  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  ASSERT_TRUE(Parent->hasPersonalityFn()) << printed(*Parent);

  const Pads Found = padsOf(*Parent);
  ASSERT_NE(Found.cleanup("inner_finally"), nullptr) << printed(*Parent);
  ASSERT_NE(Found.Except, nullptr) << printed(*Parent);
  ASSERT_NE(Found.cleanup("outer_finally"), nullptr) << printed(*Parent);
  EXPECT_EQ(unwindOfCleanup(*Found.cleanup("inner_finally")), Found.Except);
  EXPECT_EQ(llvm::cast<llvm::CatchSwitchInst>(Found.Except->getTerminator())
                ->getUnwindDest(),
            Found.cleanup("outer_finally"));
  EXPECT_EQ(unwindOfCleanup(*Found.cleanup("outer_finally")), nullptr);

  // The body's call is in all three: it unwinds to the innermost.
  const auto Calls = ownCallsOf(*Parent, "may_throw");
  ASSERT_EQ(Calls.size(), 1u) << printed(*Parent);
  const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Calls.front());
  ASSERT_NE(Invoke, nullptr) << printed(*Parent);
  EXPECT_EQ(Invoke->getUnwindDest(), Found.cleanup("inner_finally"));
}

// Native output writes the table again and keeps such a function as
// metadata; the capabilities that produce code still refuse the table.
TEST(TableSEHSourceScopes, NativeOutputLeavesRowsOverOneRangeUnlowered) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::Branch);
  const ExceptionFunction &EH = *Unit.front().ExceptionMetadata;
  for (WindowsEHNativeCapability Capability :
       {WindowsEHNativeCapability::IRLowering,
        WindowsEHNativeCapability::OutputPatch}) {
    const WindowsEHNativeSourceClassification Source =
        classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF,
                                      Capability);
    EXPECT_FALSE(Source.isEligible());
    EXPECT_EQ(Source.Reason,
              WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph);
  }

  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/false);
  ASSERT_NE(Module, nullptr);
  const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  EXPECT_FALSE(Parent->hasPersonalityFn()) << printed(*Parent);
}

// A table without such rows keeps the identity it had, and one with them has
// an identity only where the caller takes their order.
TEST(TableSEHSourceScopes, RepeatedRangesHaveAnIdentityOnlyOnRequest) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::Branch);
  const ExceptionFunction &Repeated = *Unit.front().ExceptionMetadata;
  for (uint32_t Row = 0; Row < 3; ++Row) {
    SCOPED_TRACE(Row);
    EXPECT_FALSE(
        windows_eh_semantics::getSEHScopeSemanticToken(Repeated, Arch::X64, Row)
            .has_value());
    EXPECT_TRUE(windows_eh_semantics::getSEHScopeSemanticToken(
                    Repeated, Arch::X64, Row, /*AllowRepeatedRanges=*/true)
                    .has_value());
  }

  // The same rows in the other order are another graph.
  ExceptionFunction Reordered = Repeated;
  std::swap(Reordered.SEH->Scopes[0], Reordered.SEH->Scopes[2]);
  const auto First = windows_eh_semantics::getSEHScopeSemanticToken(
      Repeated, Arch::X64, 1, /*AllowRepeatedRanges=*/true);
  const auto Second = windows_eh_semantics::getSEHScopeSemanticToken(
      Reordered, Arch::X64, 1, /*AllowRepeatedRanges=*/true);
  ASSERT_TRUE(First.has_value());
  ASSERT_TRUE(Second.has_value());
  EXPECT_NE(First->Digest, Second->Digest);

  ExceptionFunction Nested = Repeated;
  Nested.SEH->Scopes[0].GuardedRange.End = GuardedEnd - 0x10;
  Nested.SEH->Scopes[1].GuardedRange.End = GuardedEnd - 8;
  for (uint32_t Row = 0; Row < 3; ++Row) {
    SCOPED_TRACE(Row);
    const auto Plain =
        windows_eh_semantics::getSEHScopeSemanticToken(Nested, Arch::X64, Row);
    const auto Allowed = windows_eh_semantics::getSEHScopeSemanticToken(
        Nested, Arch::X64, Row, /*AllowRepeatedRanges=*/true);
    ASSERT_TRUE(Plain.has_value());
    ASSERT_TRUE(Allowed.has_value());
    EXPECT_EQ(Plain->Digest, Allowed->Digest);
    EXPECT_EQ(Plain->Region, Allowed->Region);
  }
}

// The runtime calls the handler of row k with its scan past k.  What the
// inner handler raises meets the __except and the outer __finally, and what
// the outer handler raises meets no scope of this frame.
TEST(TableSEHSourceScopes, LocalUnwindHandlerCallsLeaveTheScopesTheyEnd) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::LocalUnwind);
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  ASSERT_TRUE(Parent->hasPersonalityFn()) << printed(*Parent);
  const Pads Found = padsOf(*Parent);
  ASSERT_NE(Found.cleanup("inner_finally"), nullptr) << printed(*Parent);
  ASSERT_NE(Found.Except, nullptr) << printed(*Parent);
  ASSERT_NE(Found.cleanup("outer_finally"), nullptr) << printed(*Parent);

  const auto Inner = ownCallsOf(*Parent, "inner_finally");
  ASSERT_EQ(Inner.size(), 1u) << printed(*Parent);
  const auto *InnerInvoke = llvm::dyn_cast<llvm::InvokeInst>(Inner.front());
  ASSERT_NE(InnerInvoke, nullptr) << printed(*Parent);
  EXPECT_EQ(InnerInvoke->getUnwindDest(), Found.Except) << printed(*Parent);

  const auto Outer = ownCallsOf(*Parent, "outer_finally");
  ASSERT_EQ(Outer.size(), 1u) << printed(*Parent);
  EXPECT_TRUE(llvm::isa<llvm::CallInst>(Outer.front())) << printed(*Parent);

  // The body's call still unwinds to the innermost scope.
  const auto Calls = ownCallsOf(*Parent, "may_throw");
  ASSERT_EQ(Calls.size(), 1u);
  const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Calls.front());
  ASSERT_NE(Invoke, nullptr);
  EXPECT_EQ(Invoke->getUnwindDest(), Found.cleanup("inner_finally"));

  // Each handler call follows the end of the range it ends: an end marker
  // of the inner scope stands between the body and the inner handler's call.
  const llvm::BasicBlock *Before = InnerInvoke->getParent();
  bool LeftInnerScope = false;
  for (unsigned Step = 0; Step < 4 && Before && !LeftInnerScope; ++Step) {
    Before = Before->getSinglePredecessor();
    if (!Before)
      break;
    const auto *Marker =
        llvm::dyn_cast<llvm::InvokeInst>(Before->getTerminator());
    LeftInnerScope = Marker &&
                     Marker->getIntrinsicID() == llvm::Intrinsic::seh_try_end &&
                     Marker->getUnwindDest() == Found.cleanup("inner_finally");
  }
  EXPECT_TRUE(LeftInnerScope) << printed(*Parent);
}

// Three termination handlers of one unwind: the frame still protects the first
// two, which become two invokes at one source address.  The contract counts
// them apart, so the unit's optimization takes the module.
TEST(TableSEHSourceScopes, EachProtectedHandlerCallCountsInTheContract) {
  const std::vector<MedFunc> Unit =
      threeScopesOverOneRange(Leave::LocalUnwind, /*MiddleIsFinally=*/true);
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  ASSERT_TRUE(Parent->hasPersonalityFn()) << printed(*Parent);
  const Pads Found = padsOf(*Parent);
  const llvm::BasicBlock *Middle = Found.cleanup("middle_finally");
  const llvm::BasicBlock *Outer = Found.cleanup("outer_finally");
  ASSERT_NE(Found.cleanup("inner_finally"), nullptr) << printed(*Parent);
  ASSERT_NE(Middle, nullptr) << printed(*Parent);
  ASSERT_NE(Outer, nullptr) << printed(*Parent);

  const auto InnerCalls = ownCallsOf(*Parent, "inner_finally");
  const auto MiddleCalls = ownCallsOf(*Parent, "middle_finally");
  const auto OuterCalls = ownCallsOf(*Parent, "outer_finally");
  ASSERT_EQ(InnerCalls.size(), 1u) << printed(*Parent);
  ASSERT_EQ(MiddleCalls.size(), 1u) << printed(*Parent);
  ASSERT_EQ(OuterCalls.size(), 1u) << printed(*Parent);
  const auto *InnerInvoke =
      llvm::dyn_cast<llvm::InvokeInst>(InnerCalls.front());
  const auto *MiddleInvoke =
      llvm::dyn_cast<llvm::InvokeInst>(MiddleCalls.front());
  ASSERT_NE(InnerInvoke, nullptr) << printed(*Parent);
  ASSERT_NE(MiddleInvoke, nullptr) << printed(*Parent);
  EXPECT_EQ(InnerInvoke->getUnwindDest(), Middle) << printed(*Parent);
  EXPECT_EQ(MiddleInvoke->getUnwindDest(), Outer) << printed(*Parent);
  EXPECT_TRUE(llvm::isa<llvm::CallInst>(OuterCalls.front()))
      << printed(*Parent);

  auto Contracts =
      exception_rewrite::validateExceptionRewriteContracts(*Module);
  EXPECT_TRUE(static_cast<bool>(Contracts))
      << llvm::toString(Contracts.takeError());
}

// A call the exit does not account for, or handler calls in another order
// than the runtime's, are not placed by guess: the function stays unlowered.
TEST(TableSEHSourceScopes, LocalUnwindCallsThatMatchNoExitAreNotPlaced) {
  for (Leave How : {Leave::LocalUnwindThenCall, Leave::LocalUnwindOuterFirst}) {
    SCOPED_TRACE(How == Leave::LocalUnwindThenCall ? "call after the handlers"
                                                   : "outer handler first");
    const std::vector<MedFunc> Unit = threeScopesOverOneRange(How);
    llvm::LLVMContext Context;
    auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
    ASSERT_NE(Module, nullptr);
    const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
    ASSERT_NE(Parent, nullptr);
    EXPECT_FALSE(Parent->hasPersonalityFn()) << printed(*Parent);
    for (const llvm::BasicBlock &Block : *Parent)
      EXPECT_FALSE(Block.isEHPad()) << printed(*Parent);
  }
}

/// The LLVM C of \p Module, with \p Map filled from \p Sources when given.
std::string printedC(llvm::Module &Module, CSourceMap *Map = nullptr,
                     const LLVMSourceMap *Sources = nullptr) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.SourceMap = Map;
  Options.LLVMSources = Sources;
  EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  return Source;
}

/// The parent's definition in \p Source, from its name to its closing brace.
std::string parentIn(llvm::StringRef Source) {
  const size_t Start = Source.find("three_scopes_parent() {\n");
  if (Start == llvm::StringRef::npos)
    return {};
  return Source.slice(Start, Source.find("\n}\n", Start)).str();
}

size_t countOf(llvm::StringRef Text, llvm::StringRef Needle) {
  size_t Count = 0;
  for (size_t At = Text.find(Needle); At != llvm::StringRef::npos;
       At = Text.find(Needle, At + 1))
    ++Count;
  return Count;
}

/// The text printed under the label that holds \p Statement, to the next
/// label.
llvm::StringRef blockHolding(llvm::StringRef Function,
                             llvm::StringRef Statement) {
  const size_t At = Function.find(Statement);
  if (At == llvm::StringRef::npos)
    return {};
  size_t Label = 0;
  for (size_t Next = Function.find("\nL_"); Next < At;
       Next = Function.find("\nL_", Next + 1))
    Label = Next + 1;
  return Function.slice(Label, Function.find("\nL_", At));
}

// The range markers and the blocks a call's continuation or a split leaves
// empty print only a jump.  They get no `__try`, and no text where a jump
// can pass them: what is left is the statements, each in its scopes.
TEST(TableSEHSourceScopes, LLVMCPrintsNoTryAroundBlocksThatOnlyJump) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::LocalUnwind);
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const std::string Function = parentIn(printedC(*Module));
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(Function.find("neverd.seh: not lowered"), std::string::npos)
      << Function;
  EXPECT_EQ(Function.find("seh_try_"), std::string::npos) << Function;
  EXPECT_EQ(Function.find("seh_cont:"), std::string::npos) << Function;

  // The body's call runs in all three scopes.
  const llvm::StringRef Body = blockHolding(Function, "            may_throw(");
  ASSERT_FALSE(Body.empty()) << Function;
  EXPECT_EQ(countOf(Body, "__try {"), 3u) << Body.str();
  EXPECT_EQ(countOf(Body, "} __finally {"), 2u) << Body.str();
  EXPECT_EQ(countOf(Body, "} __except ("), 1u) << Body.str();
  // The inner handler's call runs in the two around its own, and the outer
  // handler's in none.
  const llvm::StringRef Inner = blockHolding(
      Function,
      "\n            inner_finally();\n            neverd_seh_normal");
  ASSERT_FALSE(Inner.empty()) << Function;
  EXPECT_EQ(countOf(Inner, "__try {"), 2u) << Inner.str();
  EXPECT_EQ(countOf(Inner, "inner_finally();"), 1u) << Inner.str();
  EXPECT_EQ(countOf(Inner, "} __except ("), 1u) << Inner.str();
  const llvm::StringRef Outer =
      blockHolding(Function, "\n    outer_finally();");
  ASSERT_FALSE(Outer.empty()) << Function;
  EXPECT_EQ(Outer.find("__try"), llvm::StringRef::npos) << Outer.str();
  // Nothing else has a nest.
  EXPECT_EQ(countOf(Function, "__try {"), 5u) << Function;
  // Execution enters the first block from above: it keeps its label and jump.
  const size_t First = Function.find("\nL_");
  ASSERT_NE(First, std::string::npos) << Function;
  EXPECT_TRUE(llvm::StringRef(Function).substr(First).starts_with(
      "\nL_frame_setup:\n    goto L_"))
      << Function;
}

// A scope none of whose blocks prints a statement still shows its clause, on
// the first of them.
TEST(TableSEHSourceScopes, LLVMCKeepsEveryClauseOfScopesThatOnlyJump) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(
      Leave::Branch, /*MiddleIsFinally=*/false, /*BodyCalls=*/false);
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const std::string Function = parentIn(printedC(*Module));
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(countOf(Function, "__try {"), 3u) << Function;
  EXPECT_EQ(countOf(Function, "} __finally {"), 2u) << Function;
  EXPECT_EQ(countOf(Function, "} __except ("), 1u) << Function;
  EXPECT_NE(Function.find("inner_finally();"), std::string::npos) << Function;
  EXPECT_NE(Function.find("outer_finally();"), std::string::npos) << Function;
}

// Blocks that only jump to one another in a ring are a loop, not a detour:
// the jump into it must not be followed round for ever, and the loop stays.
TEST(TableSEHSourceScopes, LLVMCKeepsALoopOfBlocksThatOnlyJump) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::Spin);
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const std::string Function = parentIn(printedC(*Module));
  ASSERT_FALSE(Function.empty());
  bool Loops = false;
  for (size_t At = Function.find("\nL_"); At != std::string::npos;
       At = Function.find("\nL_", At + 1)) {
    const size_t Colon = Function.find(":\n", At);
    const std::string Label = Function.substr(At + 1, Colon - At - 1);
    llvm::StringRef Rest = llvm::StringRef(Function).substr(Colon + 2);
    Loops |= Rest.consume_front("    goto " + Label + ";") &&
             (Rest.empty() || Rest.starts_with("\n"));
  }
  EXPECT_TRUE(Loops) << Function;
  EXPECT_EQ(Function.find("neverd.seh: not lowered"), std::string::npos)
      << Function;
  EXPECT_EQ(countOf(Function, "__try {"), 3u) << Function;
  // The blocks that lead into the loop only jump, and are passed as any.
  EXPECT_EQ(Function.find("seh_try_"), std::string::npos) << Function;
}

// A `__finally` that never returns, as one that leaves by `longjmp`, is a
// clause the printed C can show: the call, and that the path ends there, once.
// Native output, whose clauses the code generator writes again, still leaves
// such a function as metadata (here the rows over one range already do).
TEST(TableSEHSourceScopes, AFinallyThatDoesNotReturnIsLoweredForPrintedC) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(
      Leave::Branch, /*MiddleIsFinally=*/false, /*BodyCalls=*/true,
      Cookie::None, "outer_finally");
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  const llvm::Function *Outer = Module->getFunction("outer_finally");
  ASSERT_NE(Outer, nullptr);
  ASSERT_TRUE(Outer->doesNotReturn()) << printed(*Outer);
  ASSERT_TRUE(Parent->hasPersonalityFn()) << printed(*Parent);
  const Pads Found = padsOf(*Parent);
  ASSERT_NE(Found.cleanup("inner_finally"), nullptr) << printed(*Parent);
  ASSERT_NE(Found.Except, nullptr) << printed(*Parent);
  ASSERT_NE(Found.cleanup("outer_finally"), nullptr) << printed(*Parent);
  EXPECT_EQ(unwindOfCleanup(*Found.cleanup("inner_finally")), Found.Except);

  const std::string Function = parentIn(printedC(*Module));
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(Function.find("neverd.seh: not lowered"), std::string::npos)
      << Function;
  // Each clause that runs the handler says once that it does not return.
  size_t Clauses = 0;
  for (size_t At = Function.find("outer_finally("); At != std::string::npos;
       At = Function.find("outer_finally(", At + 1)) {
    ++Clauses;
    const size_t Next = Function.find('\n', At) + 1;
    const llvm::StringRef After = llvm::StringRef(Function).substr(Next);
    EXPECT_TRUE(After.ltrim().starts_with("__builtin_unreachable();"))
        << Function;
    EXPECT_FALSE(After.ltrim()
                     .drop_front(std::strlen("__builtin_unreachable();"))
                     .ltrim()
                     .starts_with("__builtin_unreachable();"))
        << Function;
  }
  EXPECT_NE(Clauses, 0u) << Function;
}

// A local unwind whose first handler never returns runs that handler outside
// the scope it ends and inside the ones around it, as any other does; what
// follows the call is never reached.
TEST(TableSEHSourceScopes, ALocalUnwindHandlerThatDoesNotReturnIsPlaced) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(
      Leave::LocalUnwind, /*MiddleIsFinally=*/false, /*BodyCalls=*/true,
      Cookie::None, "inner_finally");
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  ASSERT_TRUE(Parent->hasPersonalityFn()) << printed(*Parent);
  const Pads Found = padsOf(*Parent);
  ASSERT_NE(Found.cleanup("inner_finally"), nullptr) << printed(*Parent);
  ASSERT_NE(Found.Except, nullptr) << printed(*Parent);
  const auto Inner = ownCallsOf(*Parent, "inner_finally");
  ASSERT_EQ(Inner.size(), 1u) << printed(*Parent);
  const auto *InnerInvoke = llvm::dyn_cast<llvm::InvokeInst>(Inner.front());
  ASSERT_NE(InnerInvoke, nullptr) << printed(*Parent);
  EXPECT_EQ(InnerInvoke->getUnwindDest(), Found.Except) << printed(*Parent);

  const std::string Function = parentIn(printedC(*Module));
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(Function.find("neverd.seh: not lowered"), std::string::npos)
      << Function;
}

// `__GSHandlerCheck_SEH` checks the frame's stack cookie and hands the table
// to `__C_specific_handler`: printed C shows the same scopes as for the table
// alone.  Its identity is another one, and native output, which would have to
// write the cookie record again, leaves the function as metadata.
TEST(TableSEHSourceScopes, ATableBehindACookieCheckIsLoweredForPrintedC) {
  const std::vector<MedFunc> Plain = threeScopesOverOneRange(Leave::Branch);
  const std::vector<MedFunc> Unit =
      threeScopesOverOneRange(Leave::Branch, /*MiddleIsFinally=*/false,
                              /*BodyCalls=*/true, Cookie::Checked);
  const ExceptionFunction &EH = *Unit.front().ExceptionMetadata;
  EXPECT_TRUE(
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::SourceIRLowering)
          .isEligible());
  for (WindowsEHNativeCapability Capability :
       {WindowsEHNativeCapability::IRLowering,
        WindowsEHNativeCapability::OutputPatch}) {
    const WindowsEHNativeSourceClassification Source =
        classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF,
                                      Capability);
    EXPECT_FALSE(Source.isEligible());
    EXPECT_EQ(Source.Reason, WindowsEHNativeSourceReason::GSWrappedPersonality);
  }
  // The wrapper of another architecture is not this one until it is read.
  for (const Arch Other : {Arch::AArch64, Arch::ARM}) {
    const WindowsEHNativeSourceClassification Source =
        classifyWindowsEHNativeSource(
            EH, Other, BinaryFormat::COFF,
            WindowsEHNativeCapability::SourceIRLowering);
    EXPECT_FALSE(Source.isEligible());
    EXPECT_EQ(Source.Reason,
              WindowsEHNativeSourceReason::UnsupportedArchitecture);
  }

  for (uint32_t Row = 0; Row < 3; ++Row) {
    SCOPED_TRACE(Row);
    const auto Bare = windows_eh_semantics::getSEHScopeSemanticToken(
        *Plain.front().ExceptionMetadata, Arch::X64, Row,
        /*AllowRepeatedRanges=*/true, /*AllowCookieCheck=*/true);
    const auto Checked = windows_eh_semantics::getSEHScopeSemanticToken(
        EH, Arch::X64, Row, /*AllowRepeatedRanges=*/true,
        /*AllowCookieCheck=*/true);
    ASSERT_TRUE(Bare.has_value());
    ASSERT_TRUE(Checked.has_value());
    EXPECT_NE(Bare->Digest, Checked->Digest);
    // The plain table's identity is what it was without the allowance.
    const auto BareBefore = windows_eh_semantics::getSEHScopeSemanticToken(
        *Plain.front().ExceptionMetadata, Arch::X64, Row,
        /*AllowRepeatedRanges=*/true);
    ASSERT_TRUE(BareBefore.has_value());
    EXPECT_EQ(Bare->Digest, BareBefore->Digest);
    EXPECT_FALSE(windows_eh_semantics::getSEHScopeSemanticToken(
                     EH, Arch::X64, Row, /*AllowRepeatedRanges=*/true)
                     .has_value());
  }
  // Another cookie slot is another frame.
  ExceptionFunction Moved = EH;
  Moved.GSCookie->CookieOffset += 8;
  EXPECT_NE(windows_eh_semantics::getSEHScopeSemanticToken(
                EH, Arch::X64, 0, /*AllowRepeatedRanges=*/true,
                /*AllowCookieCheck=*/true)
                ->Digest,
            windows_eh_semantics::getSEHScopeSemanticToken(
                Moved, Arch::X64, 0, /*AllowRepeatedRanges=*/true,
                /*AllowCookieCheck=*/true)
                ->Digest);

  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  ASSERT_TRUE(Parent->hasPersonalityFn()) << printed(*Parent);
  const Pads Found = padsOf(*Parent);
  ASSERT_NE(Found.cleanup("inner_finally"), nullptr) << printed(*Parent);
  ASSERT_NE(Found.Except, nullptr) << printed(*Parent);
  ASSERT_NE(Found.cleanup("outer_finally"), nullptr) << printed(*Parent);
  EXPECT_EQ(unwindOfCleanup(*Found.cleanup("inner_finally")), Found.Except);
  const std::string Function = parentIn(printedC(*Module));
  ASSERT_FALSE(Function.empty());
  EXPECT_EQ(Function.find("neverd.seh: not lowered"), std::string::npos)
      << Function;
  EXPECT_NE(Function.find("__try {"), std::string::npos) << Function;
  EXPECT_NE(Function.find("__except"), std::string::npos) << Function;

  llvm::LLVMContext NativeContext;
  auto Native = emitUnit(Unit, NativeContext, /*ForPrintedC=*/false);
  ASSERT_NE(Native, nullptr);
  const llvm::Function *NativeParent =
      Native->getFunction("three_scopes_parent");
  ASSERT_NE(NativeParent, nullptr);
  EXPECT_FALSE(NativeParent->hasPersonalityFn()) << printed(*NativeParent);
}

// The wrapper calls `__C_specific_handler` for an exception only when its
// record's EHANDLER bit is set and for an unwind only when its UHANDLER bit
// is.  A table with a row the record keeps from the handler runs that row
// never: its `__except` or `__finally` must not print as one that runs, so
// the function says it is not lowered.
TEST(TableSEHSourceScopes, LLVMCSaysAWrappedTableIsNotLowered) {
  for (const Cookie Record :
       {Cookie::CheckedForUnwindsOnly, Cookie::CheckedForExceptionsOnly}) {
    SCOPED_TRACE(static_cast<int>(Record));
    const std::vector<MedFunc> Unit = threeScopesOverOneRange(
        Leave::Branch, /*MiddleIsFinally=*/false, /*BodyCalls=*/true, Record);
    const WindowsEHNativeSourceClassification Source =
        classifyWindowsEHNativeSource(
            *Unit.front().ExceptionMetadata, Arch::X64, BinaryFormat::COFF,
            WindowsEHNativeCapability::SourceIRLowering);
    EXPECT_FALSE(Source.isEligible());
    EXPECT_EQ(Source.Reason, WindowsEHNativeSourceReason::GSWrappedPersonality);
    llvm::LLVMContext Context;
    auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
    ASSERT_NE(Module, nullptr);
    const llvm::Function *Parent = Module->getFunction("three_scopes_parent");
    ASSERT_NE(Parent, nullptr);
    EXPECT_FALSE(Parent->hasPersonalityFn()) << printed(*Parent);
    const std::string Source_ = printedC(*Module);
    const std::string Function = parentIn(Source_);
    ASSERT_FALSE(Function.empty()) << Source_;
    EXPECT_NE(Function.find("neverd.seh: not lowered"), std::string::npos)
        << Function;
    EXPECT_EQ(Function.find("__try {"), std::string::npos) << Function;
    EXPECT_NE(Function.find("may_throw("), std::string::npos) << Function;
  }
  // A table of termination handlers alone needs no exceptions passed on.
  const std::vector<MedFunc> Finallys = threeScopesOverOneRange(
      Leave::Branch, /*MiddleIsFinally=*/true, /*BodyCalls=*/true,
      Cookie::CheckedForUnwindsOnly);
  EXPECT_TRUE(
      classifyWindowsEHNativeSource(*Finallys.front().ExceptionMetadata,
                                    Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::SourceIRLowering)
          .isEligible());
}

// The render that marks source positions drops the same blocks as the
// ordinary one, so the two still agree and a call keeps its anchor.
TEST(TableSEHSourceScopes, LLVMCSourceMapAgreesWithTheTextThatDropsBlocks) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::LocalUnwind);
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  LLVMSourceMap Sources;
  Sources.Functions[ParentVA] = Parent;
  va_t Address = ParentVA;
  for (llvm::BasicBlock &Block : *Parent)
    for (llvm::Instruction &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
          Call && Call->getCalledFunction() &&
          !Call->getCalledFunction()->isIntrinsic())
        Sources.Observations.push_back(
            {ParentVA, {Address += 4, 0}, &Instruction});
  ASSERT_FALSE(Sources.Observations.empty());

  const std::string Ordinary = printedC(*Module);
  CSourceMap Map;
  Map.RecordCallExpressions = true;
  const std::string Mapped = printedC(*Module, &Map, &Sources);
  EXPECT_EQ(Ordinary, Mapped);
  // A render that disagreed with the ordinary one would publish nothing.
  EXPECT_FALSE(Map.Definitions.empty());
  // The last handler of the unwind is a plain call.  The body's call and the
  // inner handler's are invokes, each the terminator of its block, in the
  // body and in the clauses that print a handler: every one of them keeps its
  // anchor, so a click on any call finds its address.
  const std::string Function = parentIn(Mapped);
  ASSERT_FALSE(Function.empty()) << Mapped;
  const size_t Base = Mapped.find(Function);
  for (const char *Callee :
       {"outer_finally(", "may_throw(", "inner_finally("}) {
    size_t Calls = 0;
    for (size_t At = Function.find(Callee); At != std::string::npos;
         At = Function.find(Callee, At + 1)) {
      ++Calls;
      const size_t Offset = Base + At;
      EXPECT_TRUE(llvm::any_of(Map.Anchors,
                               [&](const auto &Anchor) {
                                 return Anchor.Span.Begin <= Offset &&
                                        Offset < Anchor.Span.End;
                               }))
          << Callee << " at " << At << "\n"
          << Function;
    }
    EXPECT_NE(Calls, 0u) << Callee << "\n" << Function;
  }
}

// What the emitter itself observes reaches the page: the body's call keeps its
// instruction when the lowering makes it an invoke, and the handlers of the
// local unwind, which have no instruction of their own, name the runtime call
// they stand for.  The calls a `__finally` clause prints are the clause, not
// an instruction of the function, and name none.
TEST(TableSEHSourceScopes, LLVMCSourceMapNamesTheUnwindCallForItsHandlers) {
  std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::LocalUnwind);
  // The body's call is the first operation decoded at its address.
  MedOp &BodyCall = Unit.front().Blocks[1].Ops.front();
  ASSERT_EQ(BodyCall.Opcode, NdOp::CALL);
  BodyCall.OriginSeq = 0;
  llvm::LLVMContext Context;
  LLVMSourceMap Sources;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true, &Sources);
  ASSERT_NE(Module, nullptr);
  ASSERT_NE(Module->getFunction("three_scopes_parent"), nullptr);

  const std::string Ordinary = printedC(*Module);
  CSourceMap Map;
  Map.RecordCallExpressions = true;
  const std::string Mapped = printedC(*Module, &Map, &Sources);
  EXPECT_EQ(Ordinary, Mapped);
  EXPECT_FALSE(Map.Definitions.empty());
  const std::string Function = parentIn(Mapped);
  ASSERT_FALSE(Function.empty()) << Mapped;
  const size_t Base = Mapped.find(Function);
  // The address a call printed at \p At came from, or 0.
  const auto AddressAt = [&](size_t At) -> va_t {
    for (const auto &Anchor : Map.Anchors)
      if (Anchor.Span.Begin <= Base + At && Base + At < Anchor.Span.End &&
          !Anchor.Occurrences.empty())
        return Anchor.Occurrences.front().Address;
    return 0;
  };
  size_t BodyCalls = 0, UnwindCalls = 0;
  for (const auto &[Callee, Address] :
       {std::pair<const char *, va_t>{"may_throw(", GuardedBegin + 4},
        {"inner_finally(", UnwindCallVA},
        {"outer_finally(", UnwindCallVA}})
    for (size_t At = Function.find(Callee); At != std::string::npos;
         At = Function.find(Callee, At + 1)) {
      if (AddressAt(At) == 0)
        continue;
      EXPECT_EQ(AddressAt(At), Address) << Callee << "\n" << Function;
      (Address == UnwindCallVA ? UnwindCalls : BodyCalls) += 1;
    }
  EXPECT_EQ(BodyCalls, 1u) << Function;
  // The inner handler's call is an invoke, the outer one a plain call.
  EXPECT_EQ(UnwindCalls, 2u) << Function;

  // Only the handlers: another call a unit puts at that address without an
  // instruction of its own names nothing.
  std::vector<MedFunc> Odd = threeScopesOverOneRange(Leave::LocalUnwind);
  std::vector<MedOp> &Leaving = Odd.front().Blocks[2].Ops;
  Leaving.insert(Leaving.begin(), callOf(UnwindCallVA, MayThrowVA));
  llvm::LLVMContext OddContext;
  LLVMSourceMap OddSources;
  const auto OddModule =
      emitUnit(Odd, OddContext, /*ForPrintedC=*/true, &OddSources);
  ASSERT_NE(OddModule, nullptr);
  size_t Named = 0;
  for (const LLVMSourceObservation &Observation : OddSources.Observations) {
    if (Observation.Occurrence.Address != UnwindCallVA)
      continue;
    ++Named;
    const auto *Call =
        llvm::dyn_cast_or_null<llvm::CallBase>(Observation.Value);
    ASSERT_NE(Call, nullptr);
    ASSERT_NE(Call->getCalledFunction(), nullptr);
    EXPECT_TRUE(Call->getCalledFunction()->getName().ends_with("_finally"))
        << Call->getCalledFunction()->getName().str();
  }
  EXPECT_EQ(Named, 2u);
}

// A cleanup whose body branches prints as labelled blocks of its own.  The
// lowering writes none, but an optimizer may: an invoke there is marked too.
TEST(TableSEHSourceScopes, LLVMCSourceMapMarksInvokesOfACleanupThatBranches) {
  const std::vector<MedFunc> Unit = threeScopesOverOneRange(Leave::Branch);
  llvm::LLVMContext Context;
  auto Module = emitUnit(Unit, Context, /*ForPrintedC=*/true);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Parent = Module->getFunction("three_scopes_parent");
  ASSERT_NE(Parent, nullptr);
  llvm::InvokeInst *Handler = nullptr;
  for (llvm::BasicBlock &Block : *Parent)
    if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
        Invoke && Invoke->getCalledFunction() &&
        Invoke->getCalledFunction()->getName() == "inner_finally" &&
        Block.isEHPad())
      Handler = Invoke;
  ASSERT_NE(Handler, nullptr) << printed(*Parent);
  llvm::BasicBlock *Continue = Handler->getNormalDest();
  ASSERT_FALSE(llvm::isa<llvm::PHINode>(Continue->front())) << printed(*Parent);
  // pad: if (*flag) call; else skip;  call: invoke handler -> continue
  auto *Flag = new llvm::GlobalVariable(
      *Module, llvm::Type::getInt8Ty(Context), /*isConstant=*/false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "cleanup_flag");
  llvm::BasicBlock *Pad = Handler->getParent();
  llvm::BasicBlock *Call =
      Pad->splitBasicBlock(Handler->getIterator(), "seh.cleanup.call");
  llvm::BasicBlock *Skip =
      llvm::BasicBlock::Create(Context, "seh.cleanup.skip", Parent, Continue);
  llvm::IRBuilder<>(Skip).CreateBr(Continue);
  Pad->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> AtPad(Pad);
  AtPad.CreateCondBr(
      AtPad.CreateIsNotNull(AtPad.CreateLoad(llvm::Type::getInt8Ty(Context),
                                             Flag, /*isVolatile=*/true)),
      Call, Skip);
  ASSERT_FALSE(llvm::verifyFunction(*Parent, &llvm::errs()));

  LLVMSourceMap Sources;
  Sources.Functions[ParentVA] = Parent;
  va_t Address = ParentVA;
  for (llvm::BasicBlock &Block : *Parent)
    for (llvm::Instruction &Instruction : Block)
      if (const auto *Site = llvm::dyn_cast<llvm::CallBase>(&Instruction);
          Site && Site->getCalledFunction() &&
          !Site->getCalledFunction()->isIntrinsic())
        Sources.Observations.push_back(
            {ParentVA, {Address += 4, 0}, &Instruction});

  const std::string Ordinary = printedC(*Module);
  CSourceMap Map;
  Map.RecordCallExpressions = true;
  const std::string Mapped = printedC(*Module, &Map, &Sources);
  EXPECT_EQ(Ordinary, Mapped);
  EXPECT_FALSE(Map.Definitions.empty());
  const std::string Function = parentIn(Mapped);
  ASSERT_FALSE(Function.empty()) << Mapped;
  // The clause printed its blocks under labels of its own.
  EXPECT_NE(Function.find("seh_cleanup"), std::string::npos) << Function;
  const size_t Base = Mapped.find(Function);
  size_t Calls = 0;
  for (size_t At = Function.find("inner_finally("); At != std::string::npos;
       At = Function.find("inner_finally(", At + 1)) {
    ++Calls;
    const size_t Offset = Base + At;
    EXPECT_TRUE(llvm::any_of(Map.Anchors,
                             [&](const auto &Anchor) {
                               return Anchor.Span.Begin <= Offset &&
                                      Offset < Anchor.Span.End;
                             }))
        << "inner_finally( at " << At << "\n"
        << Function;
  }
  EXPECT_NE(Calls, 0u) << Function;
}

} // namespace
