//===- ItaniumHandlerBodiesTests.cpp - Catch bodies from landing pads -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A C++ view spells an Itanium region's clauses as `catch` bodies taken from
/// the landing pad: the pad's code as it runs for each clause's selector.
/// These tests pin the runs that read as handlers, the cleanup scopes a pad's
/// no-match path becomes, and the shapes that keep their pad in place.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t kEntry = 0x401000;
constexpr va_t kCall = kEntry + 0x4;
constexpr va_t kAfter = kEntry + 0x9;
constexpr va_t kPad = kEntry + 0x40;
constexpr va_t kRisky = 0x402000;
constexpr va_t kDtor = 0x402100;
constexpr va_t kBeginCatch = 0x403000;
constexpr va_t kEndCatch = 0x403010;
constexpr va_t kResume = 0x403020;

MedVar var(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  V.TheArch = Arch::X64;
  return V;
}

ExprPtr use(int Id, uint16_t Size = 8) {
  return HighExpr::makeVar(var(Id, Size), NdType::makeInt(Size, false));
}

ExprPtr exception() {
  MedVar V;
  V.Kind = MedVar::EHException;
  V.Id = -1;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, false));
}

ExprPtr selector() {
  MedVar V;
  V.Kind = MedVar::EHSelector;
  V.Id = -1;
  V.Size = 4;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(4));
}

ExprPtr constant(uint64_t Value, uint16_t Size = 8) {
  auto E = HighExpr::makeConst(Value, Size, ConstantAddressProvenance::Scalar);
  E->Type = NdType::makeInt(Size);
  return E;
}

ExprPtr call(const std::string &Name, va_t Target, std::vector<ExprPtr> Args,
             bool Void = false) {
  auto E = HighExpr::makeCall(Name, Target, std::move(Args));
  E->Type = Void ? NdType::makeVoid() : NdType::makeInt(8, false);
  return E;
}

HighStmt assign(int Id, ExprPtr Value, va_t Addr = 0) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = Addr;
  S.Dst = use(Id);
  S.Val = std::move(Value);
  return S;
}

HighStmt statement(ExprPtr Call, va_t Addr = 0) {
  HighStmt S;
  S.Kind = StmtKind::Call;
  S.Addr = Addr;
  S.CallExpr = std::move(Call);
  return S;
}

HighStmt ret(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

HighStmt anchor(va_t Addr) {
  HighStmt S;
  S.Kind = StmtKind::Block;
  S.Addr = Addr;
  return S;
}

HighStmt branch(ExprPtr Cond, std::vector<HighStmt> Then) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Cond = std::move(Cond);
  S.Body = std::move(Then);
  return S;
}

ExprPtr equals(ExprPtr L, uint64_t R) {
  auto E = HighExpr::makeBinop(NdOp::INT_EQUAL, std::move(L), constant(R, 4));
  E->Type = NdType::makeInt(1, false);
  return E;
}

ExprPtr beginCatch(ExprPtr Object) {
  return call("__cxa_begin_catch", kBeginCatch, {std::move(Object)});
}

HighStmt endCatch() {
  return statement(call("__cxa_end_catch", kEndCatch, {}, true));
}

HighStmt resume(ExprPtr Object) {
  auto E = call("_Unwind_Resume", kResume, {std::move(Object)}, true);
  E->DoesNotReturn = true;
  return statement(std::move(E));
}

/// The LSDA of one call site at kCall landing on kPad, whose action chain
/// names \p Filters in order (0 is a cleanup), with a type entry per filter.
ItaniumEHInfo lsda(const std::vector<int64_t> &Filters) {
  ItaniumEHInfo LSDA;
  LSDA.IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {kCall, kAfter};
  Site.LandingPadVA = kPad;
  Site.FirstActionOffset = 0;
  LSDA.CallSites.push_back(Site);
  for (size_t I = 0; I < Filters.size(); ++I) {
    ItaniumAction Action;
    Action.TableOffset = I * 2;
    Action.TypeFilter = Filters[I];
    if (I + 1 < Filters.size())
      Action.NextActionOffset = (I + 1) * 2;
    LSDA.Actions.push_back(Action);
  }
  return LSDA;
}

HighEHClause clause(int64_t Filter, const std::string &TypeName) {
  HighEHClause Clause;
  Clause.Kind = HighEHClauseKind::ItaniumCatch;
  Clause.TypeFilter = Filter;
  Clause.TypeName = TypeName;
  Clause.TypeDescriptorVA = TypeName.empty() ? 0 : 0x404000 + Filter * 0x10;
  Clause.LandingPadVAs = {kPad};
  Clause.HandlerVA = kPad;
  return Clause;
}

/// `try { v1 = risky(); } <clauses>; return v1;` followed by \p Pad.
HighFunc function(const std::vector<HighEHClause> &Clauses,
                  std::vector<HighStmt> Pad,
                  const std::vector<int64_t> &Chain) {
  HighFunc Func;
  Func.Entry = kEntry;
  Func.Name = "guarded";
  Func.ReturnType = NdType::makeInt(8);
  ExceptionFunction EH;
  EH.CodeRange = {kEntry, kPad + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium = lsda(Chain);
  Func.ExceptionMetadata = std::move(EH);
  HighStmt Try;
  Try.Kind = StmtKind::ItaniumTry;
  Try.Addr = kCall;
  Try.EHRange = {kCall, kAfter};
  Try.EHIsReducible = true;
  Try.Body.push_back(assign(1, call("risky", kRisky, {}), kCall));
  Try.EHClauses = Clauses;
  Try.EHClauseBodies.resize(Clauses.size());
  Func.Body.push_back(std::move(Try));
  Func.Body.push_back(ret(use(1)));
  Pad.insert(Pad.begin(), anchor(kPad));
  for (HighStmt &S : Pad)
    Func.Body.push_back(std::move(S));
  return Func;
}

std::string emit(const HighFunc &Func, bool Structured = true) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.EmitComments = false;
  Options.StructuredExceptionSyntax = Structured;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  return Source;
}

/// The definition of `guarded` in \p Source.
std::string body(const std::string &Source) {
  const size_t Begin = Source.find("guarded(void) {");
  return Begin == std::string::npos ? Source : Source.substr(Begin);
}

TEST(ItaniumHandlerBodies, OneClausePadBecomesItsCatch) {
  // L_pad: v2 = __cxa_begin_catch(exc); v3 = *(v2 + 8); __cxa_end_catch();
  //        return -v3;
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(2, beginCatch(exception())));
  Pad.push_back(
      assign(3, HighExpr::makeLoad(
                    HighExpr::makeBinop(NdOp::INT_ADD, use(2), constant(8)),
                    NdType::makeInt(8, false))));
  Pad.push_back(endCatch());
  Pad.push_back(ret(HighExpr::makeBinop(NdOp::INT_SUB, constant(0), use(3))));
  const HighFunc Func =
      function({clause(1, "_ZTISt13runtime_error")}, std::move(Pad), {1});
  const std::string Source = body(emit(Func));
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} catch (std::runtime_error &e) {"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("&e"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__cxa_begin_catch"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__cxa_end_catch"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("eh_exception"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("L_401040"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, SelectorTestsAreDecidedPerClause) {
  // v6 = exc; v7 = sel;
  // if (v7 == 2) { begin_catch(v6); end_catch(); return 20; }
  // begin_catch(v6); end_catch(); return 10;
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(6, exception()));
  Pad.push_back(assign(7, selector()));
  std::vector<HighStmt> Second;
  Second.push_back(statement(beginCatch(use(6))));
  Second.push_back(endCatch());
  Second.push_back(ret(constant(20)));
  Pad.push_back(branch(equals(use(7, 4), 2), std::move(Second)));
  Pad.push_back(statement(beginCatch(use(6))));
  Pad.push_back(endCatch());
  Pad.push_back(ret(constant(10)));
  const HighFunc Func =
      function({clause(2, "_ZTIi"), clause(1, "")}, std::move(Pad), {2, 1});
  const std::string Source = body(emit(Func));
  const size_t Int = Source.find("} catch (int &) {");
  const size_t All = Source.find("} catch (...) {");
  ASSERT_NE(Int, std::string::npos) << Source;
  ASSERT_NE(All, std::string::npos) << Source;
  EXPECT_NE(Source.find("return 0x14;", Int), std::string::npos) << Source;
  EXPECT_LT(Source.find("return 0x14;", Int), All) << Source;
  EXPECT_NE(Source.find("return 10;", All), std::string::npos) << Source;
  EXPECT_EQ(Source.find("eh_selector"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("v7 =="), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, ANoMatchCleanupWrapsTheRegion) {
  // The chain ends in a cleanup: with no clause matched the pad destroys an
  // object and resumes.  That is a catch-all around the try that rethrows,
  // and the handler, which an exception it throws must not destroy again,
  // keeps only its own code.
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(6, exception()));
  std::vector<HighStmt> Caught;
  Caught.push_back(statement(beginCatch(use(6))));
  Caught.push_back(endCatch());
  Caught.push_back(ret(constant(7)));
  Pad.push_back(branch(equals(selector(), 1), std::move(Caught)));
  Pad.push_back(statement(call("held_dtor", kDtor, {}, true)));
  Pad.push_back(resume(use(6)));
  const HighFunc Func = function({clause(1, "_ZTIi")}, std::move(Pad), {1, 0});
  const std::string Source = body(emit(Func));
  const size_t Outer = Source.find("try {");
  const size_t Inner = Source.find("try {", Outer + 1);
  const size_t Int = Source.find("} catch (int &) {");
  const size_t Cleanup = Source.find("} catch (...) {");
  ASSERT_NE(Inner, std::string::npos) << Source;
  ASSERT_NE(Int, std::string::npos) << Source;
  ASSERT_NE(Cleanup, std::string::npos) << Source;
  EXPECT_LT(Int, Cleanup) << Source;
  EXPECT_GT(Source.find("held_dtor(", Cleanup), Cleanup) << Source;
  EXPECT_NE(Source.find("throw;", Cleanup), std::string::npos) << Source;
  // The handler does not run the cleanup.
  EXPECT_GT(Source.find("held_dtor("), Cleanup) << Source;
  EXPECT_EQ(Source.find("_Unwind_Resume"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, APadAnotherTryAlsoDispatchesStaysInPlace) {
  // The call site's chain names a clause of an enclosing try, which these
  // handlers do not spell: the region keeps its pad.
  std::vector<HighStmt> Pad;
  Pad.push_back(statement(beginCatch(exception())));
  Pad.push_back(endCatch());
  Pad.push_back(ret(constant(1)));
  const HighFunc Func = function({clause(1, "_ZTIi")}, std::move(Pad), {1, 2});
  const std::string Source = body(emit(Func));
  EXPECT_EQ(Source.find("} catch ("), std::string::npos) << Source;
  EXPECT_NE(Source.find("/* catch ("), std::string::npos) << Source;
  EXPECT_NE(Source.find("__cxa_begin_catch"), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_401040"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, PlainCKeepsThePadAndItsUnwinderValues) {
  // C has no catch: the pad stays, labelled, and its copies of the values the
  // unwinder hands it read them rather than an unknown.
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(6, exception()));
  Pad.push_back(statement(call("held_dtor", kDtor, {}, true)));
  Pad.push_back(resume(use(6)));
  HighFunc Func = function({clause(1, "_ZTIi")}, std::move(Pad), {0});
  // A cleanup-only call site has no region.
  Func.Body.erase(Func.Body.begin());
  Func.Body.insert(Func.Body.begin(),
                   assign(1, call("risky", kRisky, {}), kCall));
  const std::string Source = body(emit(Func, /*Structured=*/false));
  EXPECT_NE(Source.find("L_401040: /* landing pad"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("held_dtor("), std::string::npos) << Source;
  EXPECT_NE(Source.find("_Unwind_Resume"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
}

constexpr va_t kOther = kEntry + 0x10;
constexpr va_t kLater = kEntry + 0x20;
constexpr va_t kRisky2 = 0x402200;
constexpr va_t kRelease = 0x402300;

/// A call the instruction at \p Site makes.
ExprPtr callAt(const std::string &Name, va_t Target, va_t Site,
               bool Void = false) {
  ExprPtr E = call(Name, Target, {}, Void);
  E->CallSiteAddr = Site;
  return E;
}

/// Call sites [Site, Site + 5) landing on \p Pad with no action: a pad of 0
/// is none, and kPad's sites only clean up.
ItaniumEHInfo cleanupSites(const std::vector<std::pair<va_t, va_t>> &Sites) {
  ItaniumEHInfo LSDA;
  LSDA.IsCallSiteAddressForm = true;
  for (const auto &[Begin, Pad] : Sites) {
    ItaniumCallSite Site;
    Site.GuardedRange = {Begin, Begin + 5};
    Site.LandingPadVA = Pad;
    LSDA.CallSites.push_back(Site);
  }
  return LSDA;
}

/// A function of \p Body followed by the pad at kPad: \p Pad, or by default
/// one that destroys an object and resumes the unwind.
HighFunc cleanupFunction(ItaniumEHInfo LSDA, std::vector<HighStmt> Body,
                         std::vector<HighStmt> Pad = {}) {
  HighFunc Func;
  Func.Entry = kEntry;
  Func.Name = "guarded";
  Func.ReturnType = NdType::makeInt(8);
  ExceptionFunction EH;
  EH.CodeRange = {kEntry, kPad + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium = std::move(LSDA);
  Func.ExceptionMetadata = std::move(EH);
  Func.Body = std::move(Body);
  Func.Body.push_back(anchor(kPad));
  if (Pad.empty()) {
    Pad.push_back(assign(6, exception()));
    Pad.push_back(statement(call("held_dtor", kDtor, {}, true)));
    Pad.push_back(resume(use(6)));
  }
  for (HighStmt &S : Pad)
    Func.Body.push_back(std::move(S));
  return Func;
}

/// Where \p Source's try at or after \p From opens, its catch-all opens,
/// and that handler ends.
struct Scope {
  size_t Try = std::string::npos;
  size_t Catch = std::string::npos;
  size_t End = std::string::npos;
};
Scope scopeAfter(const std::string &Source, size_t From = 0) {
  Scope S;
  S.Try = Source.find("try {", From);
  if (S.Try != std::string::npos)
    S.Catch = Source.find("} catch (...) {", S.Try);
  if (S.Catch != std::string::npos)
    S.End = Source.find("throw;", S.Catch);
  return S;
}

TEST(ItaniumHandlerBodies, ACleanupOnlyPadBecomesACatchAllThatRethrows) {
  // v1 = risky(); return v1;  L_pad: v6 = exc; held_dtor(); resume(v6);
  std::vector<HighStmt> Body;
  Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  Body.push_back(ret(use(1)));
  const HighFunc Func =
      cleanupFunction(cleanupSites({{kCall, kPad}}), std::move(Body));
  const std::string Source = body(emit(Func));
  const Scope S = scopeAfter(Source);
  ASSERT_NE(S.End, std::string::npos) << Source;
  EXPECT_LT(Source.find("risky()"), S.Catch) << Source;
  EXPECT_GT(Source.find("held_dtor("), S.Catch) << Source;
  EXPECT_LT(Source.find("held_dtor("), S.End) << Source;
  EXPECT_GT(Source.find("return v1;"), S.End) << Source;
  EXPECT_EQ(Source.find("_Unwind_Resume"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("L_401040"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("eh_exception"), std::string::npos) << Source;
  // C has no try: the pad stays.
  const std::string C = body(emit(Func, /*Structured=*/false));
  EXPECT_NE(C.find("L_401040: /* landing pad"), std::string::npos) << C;
}

TEST(ItaniumHandlerBodies, ACleanupScopeHoldsNoCallItsPadDoesNotCover) {
  // risky() and risky2() reach the pad; other() between them reaches none, so
  // a handler must not destroy the object when it throws.
  std::vector<HighStmt> Body;
  Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  Body.push_back(assign(2, callAt("other", kRelease, kOther), kOther));
  Body.push_back(assign(3, callAt("risky2", kRisky2, kLater), kLater));
  Body.push_back(ret(use(3)));
  const HighFunc Func = cleanupFunction(
      cleanupSites({{kCall, kPad}, {kOther, 0}, {kLater, kPad}}),
      std::move(Body));
  const std::string Source = body(emit(Func));
  const Scope First = scopeAfter(Source);
  ASSERT_NE(First.End, std::string::npos) << Source;
  const Scope Second = scopeAfter(Source, First.End);
  ASSERT_NE(Second.End, std::string::npos) << Source;
  EXPECT_LT(Source.find("risky()"), First.Catch) << Source;
  const size_t Other = Source.find("other()");
  EXPECT_GT(Other, First.End) << Source;
  EXPECT_LT(Other, Second.Try) << Source;
  EXPECT_GT(Source.find("risky2()"), Second.Try) << Source;
  EXPECT_LT(Source.find("risky2()"), Second.Catch) << Source;
}

TEST(ItaniumHandlerBodies, ACopiedCallKeepsTheCoverOfItsInstruction) {
  // A tail duplicated over a jump takes the jump's address, here one the
  // pad's call site covers; the call it copies is made at kOther, which no
  // pad covers, so it stays out of the scope.
  std::vector<HighStmt> Body;
  Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  Body.push_back(
      statement(callAt("release", kRelease, kOther, true), kCall + 2));
  Body.push_back(ret(use(1)));
  const HighFunc Func = cleanupFunction(
      cleanupSites({{kCall, kPad}, {kOther, 0}}), std::move(Body));
  const std::string Source = body(emit(Func));
  const Scope S = scopeAfter(Source);
  ASSERT_NE(S.End, std::string::npos) << Source;
  EXPECT_LT(Source.find("risky()"), S.Catch) << Source;
  EXPECT_GT(Source.find("release()"), S.End) << Source;
  EXPECT_EQ(scopeAfter(Source, S.End).Try, std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, ACopyWithoutAnAddressKeepsItsCallsInstruction) {
  // Work copied into both arms has no address of its own; its call is still
  // the one instruction the pad covers.
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Addr = kEntry;
  Branch.Cond = equals(use(4, 4), 0);
  Branch.Body.push_back(assign(1, callAt("risky", kRisky, kCall), 0));
  Branch.ElseBody.push_back(assign(1, callAt("risky", kRisky, kCall), 0));
  Branch.ElseBody.push_back(
      assign(2, callAt("other", kRelease, kOther), kOther));
  std::vector<HighStmt> Body;
  Body.push_back(std::move(Branch));
  Body.push_back(ret(use(1)));
  const HighFunc Func = cleanupFunction(
      cleanupSites({{kCall, kPad}, {kOther, 0}}), std::move(Body));
  const std::string Source = body(emit(Func));
  const Scope First = scopeAfter(Source);
  ASSERT_NE(First.End, std::string::npos) << Source;
  const Scope Second = scopeAfter(Source, First.End);
  ASSERT_NE(Second.End, std::string::npos) << Source;
  EXPECT_GT(Source.find("other()"), Second.End) << Source;
  EXPECT_EQ(Source.find("L_401040"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, AnExceptionSpilledThroughAFrameAddressIsResumed) {
  // A frame too large for an immediate offset keeps the address of the slot
  // the pad spills the exception to in another slot:
  //   *(*(fp + 0x40)) = exc; held_dtor(); v7 = *(fp + 0x40); v8 = *v7;
  //   _Unwind_Resume(v8);
  auto SlotAddress = [] {
    return HighExpr::makeLoad(
        HighExpr::makeBinop(NdOp::INT_ADD, use(10), constant(0x40)),
        NdType::makeInt(8, false));
  };
  HighStmt Spill;
  Spill.Kind = StmtKind::Store;
  Spill.StoreAddr = SlotAddress();
  Spill.StoreVal = exception();
  std::vector<HighStmt> Pad;
  Pad.push_back(std::move(Spill));
  Pad.push_back(statement(call("held_dtor", kDtor, {}, true)));
  Pad.push_back(assign(7, SlotAddress()));
  Pad.push_back(
      assign(8, HighExpr::makeLoad(use(7), NdType::makeInt(8, false))));
  Pad.push_back(resume(use(8)));
  std::vector<HighStmt> Body;
  Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  Body.push_back(ret(use(1)));
  const HighFunc Func = cleanupFunction(cleanupSites({{kCall, kPad}}),
                                        std::move(Body), std::move(Pad));
  const std::string Source = body(emit(Func));
  const Scope S = scopeAfter(Source);
  ASSERT_NE(S.End, std::string::npos) << Source;
  EXPECT_GT(Source.find("held_dtor("), S.Catch) << Source;
  EXPECT_EQ(Source.find("_Unwind_Resume"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("eh_exception"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("v7 ="), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, ACallOfNoInstructionKeepsTheCleanupPad) {
  // A call no instruction makes cannot be told covered or not.
  std::vector<HighStmt> Body;
  Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  Body.push_back(statement(call("release", kRelease, {}, true), kOther));
  Body.push_back(ret(use(1)));
  const HighFunc Func =
      cleanupFunction(cleanupSites({{kCall, kPad}}), std::move(Body));
  const std::string Source = body(emit(Func));
  EXPECT_EQ(Source.find("} catch (...) {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_401040: /* landing pad"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("_Unwind_Resume"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, ABranchWhoseArmsCallThroughThePadIsOneScope) {
  // if (v4) v1 = risky(); else v1 = risky2();  Both calls reach the pad.
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Addr = kEntry;
  Branch.Cond = equals(use(4, 4), 0);
  Branch.Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  Branch.ElseBody.push_back(
      assign(1, callAt("risky2", kRisky2, kLater), kLater));
  std::vector<HighStmt> Body;
  Body.push_back(std::move(Branch));
  Body.push_back(ret(use(1)));
  const HighFunc Func = cleanupFunction(
      cleanupSites({{kCall, kPad}, {kLater, kPad}}), std::move(Body));
  const std::string Source = body(emit(Func));
  const Scope S = scopeAfter(Source);
  ASSERT_NE(S.End, std::string::npos) << Source;
  EXPECT_LT(S.Try, Source.find("if (")) << Source;
  EXPECT_LT(Source.find("risky2()"), S.Catch) << Source;
  EXPECT_EQ(scopeAfter(Source, S.End).Try, std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, AJumpIntoACleanupScopeKeepsThePad) {
  // if (v4) goto L_later;  v1 = risky();  L_later: v2 = risky2();
  // C++ jumps into no try block.
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = kLater;
  std::vector<HighStmt> Body;
  Body.push_back(branch(equals(use(4, 4), 0), {}));
  Body.back().Body.push_back(std::move(Jump));
  Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  Body.push_back(assign(2, callAt("risky2", kRisky2, kLater), kLater));
  Body.push_back(ret(use(2)));
  const HighFunc Func = cleanupFunction(
      cleanupSites({{kCall, kPad}, {kLater, kPad}}), std::move(Body));
  const std::string Source = body(emit(Func));
  EXPECT_EQ(Source.find("} catch (...) {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_401040: /* landing pad"), std::string::npos)
      << Source;
}

TEST(ItaniumHandlerBodies, ACoveredCallOnlyAJumpReachesKeepsThePad) {
  // v2 = risky2(); goto L_call; if (v4 == 0) { L_call: v1 = risky(); }
  // The branch is entered only by the jump to its body, so no walk over
  // ordinary statements reaches the call in it; the pad stays.
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = kCall;
  std::vector<HighStmt> Inside;
  Inside.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
  std::vector<HighStmt> Body;
  Body.push_back(assign(2, callAt("risky2", kRisky2, kLater), kLater));
  Body.push_back(std::move(Jump));
  Body.push_back(branch(equals(use(4, 4), 0), std::move(Inside)));
  Body.push_back(ret(use(1)));
  const HighFunc Func = cleanupFunction(
      cleanupSites({{kCall, kPad}, {kLater, kPad}}), std::move(Body));
  const std::string Source = body(emit(Func));
  EXPECT_EQ(Source.find("} catch (...) {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_401040: /* landing pad"), std::string::npos)
      << Source;
}

TEST(ItaniumHandlerBodies, NothingFallsOutOfAnEndlessLoop) {
  // while (1) { v1 = risky(); if (v1 == 0) return v1; }  return 7;
  // With the pad gone the loop ends the function; the return after it, which
  // nothing reaches, is not printed.  A loop a break leaves still continues.
  for (const bool Breaks : {false, true}) {
    HighStmt Loop;
    Loop.Kind = StmtKind::While;
    Loop.Addr = kEntry;
    Loop.Cond = constant(1, 1);
    Loop.Body.push_back(assign(1, callAt("risky", kRisky, kCall), kCall));
    std::vector<HighStmt> Leave;
    if (Breaks) {
      HighStmt Break;
      Break.Kind = StmtKind::Break;
      Leave.push_back(std::move(Break));
    } else {
      Leave.push_back(ret(use(1)));
    }
    Loop.Body.push_back(branch(equals(use(1), 0), std::move(Leave)));
    std::vector<HighStmt> Body;
    Body.push_back(std::move(Loop));
    Body.push_back(ret(constant(7)));
    const HighFunc Func =
        cleanupFunction(cleanupSites({{kCall, kPad}}), std::move(Body));
    const std::string Source = body(emit(Func));
    EXPECT_NE(Source.find("} catch (...) {"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("return 7;") != std::string::npos, Breaks) << Source;
  }
}

TEST(ItaniumHandlerBodies, ACatchParameterPointerIsItsValue) {
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(2, beginCatch(exception())));
  Pad.push_back(
      assign(3, HighExpr::makeLoad(
                    HighExpr::makeBinop(NdOp::INT_ADD, use(2), constant(8)),
                    NdType::makeInt(8, false))));
  Pad.push_back(endCatch());
  Pad.push_back(ret(use(3)));
  const HighFunc Func =
      function({clause(1, "P15CxxEhProbeError")}, std::move(Pad), {1});
  const std::string Source = body(emit(Func));
  EXPECT_NE(Source.find("} catch (CxxEhProbeError *e) {"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("&e"), std::string::npos) << Source;
}

} // namespace
