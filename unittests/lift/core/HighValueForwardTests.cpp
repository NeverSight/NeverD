//===- HighValueForwardTests.cpp - Folding values into their uses ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <string>

namespace {
using namespace neverd;

ExprPtr input() {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = 0;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, false));
}

/// A temporary as MedToHigh leaves it: a signed integer by default.
ExprPtr local(int Id, TypeRef Type = NdType::makeInt(8, true)) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, std::move(Type));
}

ExprPtr constant(uint64_t N) { return HighExpr::makeConst(N, 8); }

ExprPtr op(NdOp Op, ExprPtr A, ExprPtr B, bool Bool = false) {
  auto E = HighExpr::makeBinop(Op, std::move(A), std::move(B));
  E->Type = Bool ? NdType::makeInt(1, false) : NdType::makeInt(8, true);
  return E;
}

HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}

HighStmt result(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

HighStmt when(ExprPtr Cond, std::vector<HighStmt> Body) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Cond = std::move(Cond);
  S.Body = std::move(Body);
  return S;
}

HighFunc function(const std::string &Name, std::vector<HighStmt> Body) {
  HighFunc F;
  F.Name = Name;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8, false);
  F.Params = {{"arg0", NdType::makeInt(8, false)}};
  F.Body = std::move(Body);
  return F;
}

std::string emit(const std::vector<HighFunc> &Functions,
                 Arch TheArch = Arch::X64) {
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = TheArch;
  EXPECT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  return Source;
}

std::string declarationOf(const std::string &Source, const std::string &Name) {
  const std::string Suffix = " " + Name + ";";
  size_t At = Source.find(Suffix);
  if (At == std::string::npos)
    return {};
  const size_t Start = Source.rfind('\n', At) + 1;
  return Source.substr(Start, At + Suffix.size() - Start);
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-signedness", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-signedness", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-signedness", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined",
        "-fsanitize-trap=undefined",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    ASSERT_EQ(Result, 0) << Error << '\n' << Source;
  }
}

ExprPtr renamed(int Tag) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Tag;
  V.RenameTag = Tag;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, true));
}

HighStmt loop(ExprPtr Cond, std::vector<HighStmt> Body) {
  HighStmt S;
  S.Kind = StmtKind::While;
  S.Cond = std::move(Cond);
  S.Body = std::move(Body);
  return S;
}

ExprPtr typedParameter(int Id, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Type->Size;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, std::move(Type));
}

TEST(HighValueForward, IndexedLoadKeepsSnapshotAcrossCoalescedLoopCopies) {
  for (bool MixedKinds : {false, true}) {
    SCOPED_TRACE(MixedKinds);
    const auto Word = NdType::makeInt(4, false);
    const auto IndexType = NdType::makeInt(8, false);
    auto Value = [&](int Tag, bool Backedge = false) {
      MedVar V;
      V.Kind = MixedKinds && Backedge ? MedVar::Temp : MedVar::Reg;
      V.Id = 50000 + Tag;
      V.RenameTag = Tag;
      V.Size = Tag == 0 ? 8 : 4;
      V.TheArch = Arch::X64;
      return HighExpr::makeVar(V, Tag == 0 ? IndexType : Word);
    };
    auto Array = [] {
      return typedParameter(0, NdType::makePtr(NdType::makeInt(4, true)));
    };
    auto Count = [] { return typedParameter(1, NdType::makeInt(4, true)); };
    auto Add = [](ExprPtr A, ExprPtr B, TypeRef Type) {
      auto E = op(NdOp::INT_ADD, std::move(A), std::move(B));
      E->Type = std::move(Type);
      return E;
    };
    auto Next = [&] { return Add(Value(0), constant(1), IndexType); };
    auto Saved = [] {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = 24;
      V.SSAVer = 1;
      V.Size = 4;
      V.TheArch = Arch::X64;
      return HighExpr::makeVar(V, NdType::makeInt(4, false));
    };
    auto Offset = op(NdOp::INT_MULT, Value(0), constant(4));
    auto Load = HighExpr::makeLoad(op(NdOp::INT_ADD, Array(), Offset), Word);
    auto SetIndex = assign(Value(0, true), Next());
    auto SetSum = assign(Value(1, true), Add(Value(1), Saved(), Word));
    SetIndex.IsPhiCopy = SetSum.IsPhiCopy = true;
    HighStmt Break;
    Break.Kind = StmtKind::Break;
    HighFunc F = function(
        "indexed_sum",
        {when(
             op(NdOp::INT_SLESSEQUAL, Count(), HighExpr::makeConst(0, 4), true),
             {result(HighExpr::makeConst(0, 4))}),
         assign(Value(0), constant(0)),
         assign(Value(1), HighExpr::makeConst(0, 4)),
         loop(HighExpr::makeConst(1, 1),
              {assign(Saved(), Load),
               assign(Value(2), Add(Value(1), Saved(), Word)),
               when(op(NdOp::INT_EQUAL, Count(), Next(), true), {Break}),
               SetIndex, SetSum}),
         result(Value(2))});
    F.ReturnType = Word;
    F.Params = {{"arg0", NdType::makePtr(NdType::makeInt(4, true))},
                {"arg1", NdType::makeInt(4, true)}};
    const std::string Source = emit({F});
    compileAndRun(Source + R"(
int main(void) {
  const int32_t a[] = {-5, 7, 0, 9, -3};
  const int32_t b[] = {13, -2, -17, 6, 41};
  for (unsigned which = 0; which < 2; ++which) {
    const int32_t *values = which ? b : a;
    uint32_t expected = 0;
    for (int count = 1; count <= 5; ++count) {
      expected += (uint32_t)values[count - 1];
      if (indexed_sum((int32_t *)values, count) != expected) return 1;
    }
  }
  return indexed_sum(0, 0) == 0 && indexed_sum(0, -1) == 0 ? 0 : 2;
}
)");
  }
}

TEST(HighValueForward, TypedIndexedLoadKeepsSnapshotBeforeMemoryStore) {
  const auto Word = NdType::makeInt(8, false);
  auto Address = [&] {
    return op(NdOp::INT_ADD, typedParameter(0, NdType::makePtr(Word)),
              op(NdOp::INT_MULT, typedParameter(1, Word), constant(8)));
  };
  HighStmt Write;
  Write.Kind = StmtKind::Store;
  Write.StoreAddr = Address();
  Write.StoreVal = constant(7);
  HighFunc F = function(
      "indexed_store",
      {assign(local(1, Word), HighExpr::makeLoad(Address(), Word)), Write,
       result(op(NdOp::INT_ADD, local(1, Word),
                 HighExpr::makeLoad(Address(), Word)))});
  F.Params = {{"arg0", NdType::makePtr(Word)}, {"arg1", Word}};
  const std::string Source = emit({F});
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t initial = 0; initial < 40; ++initial) {
    uint64_t values[] = {91, initial, 83};
    if (indexed_store(values, 1) != initial + 7 || values[0] != 91 ||
        values[1] != 7 || values[2] != 83) return 1;
  }
  return 0;
}
)");
}

TEST(HighValueForward, StableTypedIndexedLoadStillFoldsMultipleUses) {
  const auto Word = NdType::makeInt(8, false);
  auto Address = op(NdOp::INT_ADD, typedParameter(0, NdType::makePtr(Word)),
                    op(NdOp::INT_MULT, typedParameter(1, Word), constant(8)));
  HighFunc F =
      function("stable_index",
               {assign(local(1, Word), HighExpr::makeLoad(Address, Word)),
                result(op(NdOp::INT_ADD, local(1, Word), local(1, Word)))});
  F.Params = {{"arg0", NdType::makePtr(Word)}, {"arg1", Word}};
  const std::string Source = emit({F});
  EXPECT_EQ(declarationOf(Source, "t1"), "") << Source;
  EXPECT_NE(Source.find("arg0[arg1]"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t initial = 0; initial < 40; ++initial) {
    uint64_t values[] = {91, initial, 83};
    for (uint64_t index = 0; index < 3; ++index)
      if (stable_index(values, index) != values[index] * 2) return 1;
    if (values[0] != 91 || values[1] != initial || values[2] != 83) return 2;
  }
  return 0;
}
)");
}

TEST(HighValueForward, TypedMemberLoadKeepsSnapshotAcrossMutatingCall) {
  const auto Word = NdType::makeInt(8, false);
  auto Counter = NdType::makeNamedRecord("Counter", 24);
  Counter->FieldDisplayNames = {"before", "value", "after"};
  Counter->FieldDisplayOffsets = {0, 8, 16};
  Counter->FieldDisplayTypes = {Word, Word, Word};
  const auto Pointer = NdType::makePtr(Counter);
  auto Argument = [&] { return typedParameter(0, Pointer); };
  auto Address = HighExpr::makeBinop(NdOp::INT_ADD, Argument(), constant(8));
  Address->Type = NdType::makePtr(Word);
  HighStmt Change;
  Change.Kind = StmtKind::Call;
  Change.CallExpr = HighExpr::makeCall("mutate_counter", 0x2000, {Argument()});
  Change.CallExpr->Type = NdType::makeInt(4, true);
  HighFunc F =
      function("member_snapshot",
               {assign(local(17, Word), HighExpr::makeLoad(Address, Word)),
                Change, result(local(17, Word))});
  F.ReturnType = Word;
  F.Params = {{"arg0", Pointer}};
  const std::string Source = emit({F});
  // The named-record spelling is a display type. Supply and independently
  // check its concrete C layout before the generated function uses it.
  compileAndRun(std::string(R"(
#include <stdint.h>
#include <stddef.h>
typedef struct Counter {
  uint64_t before;
  uint64_t value;
  uint64_t after;
} Counter;
_Static_assert(sizeof(Counter) == 24, "Counter size");
_Static_assert(offsetof(Counter, value) == 8, "Counter value offset");
_Static_assert(offsetof(Counter, after) == 16, "Counter trailing canary offset");
int mutate_counter(uintptr_t address);
)") + Source + R"(
static uint64_t replacement;
static unsigned calls;
int mutate_counter(uintptr_t address) {
  Counter *counter = (Counter *)address;
  ++calls;
  counter->value = replacement;
  return 0;
}
int main(void) {
  static const uint64_t initial[] = {
    UINT64_C(0), UINT64_C(1), UINT64_C(7), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_MAX - UINT64_C(1), UINT64_MAX
  };
  static const uint64_t changed[] = {
    UINT64_MAX, UINT64_C(0), UINT64_C(11), UINT64_C(0x8000000000000000),
    UINT64_C(0x123456789abcdef0), UINT64_C(19), UINT64_C(1)
  };
  for (size_t i = 0; i < sizeof(initial) / sizeof(initial[0]); ++i) {
    Counter counter = {UINT64_C(0x13579bdf2468ace0), initial[i],
                       UINT64_C(0xfedcba9876543210)};
    replacement = changed[i];
    calls = 0;
    if (member_snapshot(&counter) != initial[i]) return 1;
    if (counter.value != changed[i] || calls != 1) return 2;
    if (counter.before != UINT64_C(0x13579bdf2468ace0) ||
        counter.after != UINT64_C(0xfedcba9876543210)) return 3;
  }
  return 0;
}
)");
}

TEST(HighValueForward, TypedIndexedLoadEvaluatesCallAddressOnce) {
  const auto Word = NdType::makeInt(8, false);
  auto Index = HighExpr::makeCall("next_index", 0x2100, {});
  Index->Type = NdType::makeInt(4, true);
  auto Offset = op(NdOp::INT_MULT, Index, constant(8));
  Offset->Type = Word;
  auto Address =
      op(NdOp::INT_ADD, typedParameter(0, NdType::makePtr(Word)), Offset);
  Address->Type = NdType::makePtr(Word);
  auto Sum = op(NdOp::INT_ADD, local(23, Word), local(23, Word));
  Sum->Type = Word;
  HighFunc F =
      function("indexed_call_snapshot",
               {assign(local(23, Word), HighExpr::makeLoad(Address, Word)),
                result(Sum)});
  F.ReturnType = Word;
  F.Params = {{"arg0", NdType::makePtr(Word)}};
  const std::string Source = emit({F});
  // The unknown external call is an int-returning ABI boundary. Its result
  // changes on each invocation, independently of the array being read.
  compileAndRun(std::string(R"(
#include <stdint.h>
int next_index(void);
)") + Source + R"(
static unsigned calls;
static unsigned first_index;
int next_index(void) {
  const unsigned index = (first_index + calls) % 3;
  ++calls;
  return (int)index;
}
int main(void) {
  for (unsigned row = 0; row < 2; ++row) {
    for (unsigned index = 0; index < 3; ++index) {
      uint64_t values[] = {UINT64_C(0x13579bdf2468ace0), 11 + row,
                           23 + row * 3, 37 + row * 7,
                           UINT64_C(0xfedcba9876543210)};
      const uint64_t original[] = {UINT64_C(0x13579bdf2468ace0), 11 + row,
                                  23 + row * 3, 37 + row * 7,
                                  UINT64_C(0xfedcba9876543210)};
      const uint64_t expected = original[index + 1] * UINT64_C(2);
      first_index = index;
      calls = 0;
      const uint64_t actual = indexed_call_snapshot(values + 1);
      if (calls != 1) return 1;
      if (actual != expected) return 2;
      for (unsigned i = 0; i < 5; ++i)
        if (values[i] != original[i]) return 3;
    }
  }
  return 0;
}
)");
}

TEST(HighValueForward, TypedMemberNarrowingKeepsUnsignedByteView) {
  for (bool CastReturn : {false, true}) {
    SCOPED_TRACE(CastReturn);
    const auto Word = NdType::makeInt(8, false);
    const auto Byte = NdType::makeInt(1, false);
    auto Counter = NdType::makeNamedRecord("Counter", 24);
    Counter->FieldDisplayNames = {"before", "value", "after"};
    Counter->FieldDisplayOffsets = {0, 8, 16};
    Counter->FieldDisplayTypes = {Word, Word, Word};
    const auto Pointer = NdType::makePtr(Counter);
    auto Address = op(NdOp::INT_ADD, typedParameter(0, Pointer), constant(8));
    Address->Type = NdType::makePtr(Word);
    auto Saved = [&] {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = 29;
      V.Size = 1;
      V.TheArch = Arch::X64;
      return HighExpr::makeVar(V, Byte);
    };
    auto Narrow = std::make_shared<HighExpr>();
    Narrow->Kind = ExprKind::Cast;
    Narrow->CastTo = Narrow->Type = Byte;
    Narrow->Operands.push_back(HighExpr::makeLoad(Address, Word));
    auto Widen = HighExpr::makeUnary(NdOp::INT_ZEXT, Saved());
    Widen->Type = Word;
    if (CastReturn) {
      Widen->Kind = ExprKind::Cast;
      Widen->CastTo = Word;
    }
    HighFunc F =
        function("narrowed_member", {assign(Saved(), Narrow), result(Widen)});
    F.ReturnType = Word;
    F.Params = {{"arg0", Pointer}};
    const std::string Source = emit({F});
    compileAndRun(std::string(R"(
#include <stdint.h>
#include <stddef.h>
typedef struct Counter {
  uint64_t before;
  uint64_t value;
  uint64_t after;
} Counter;
_Static_assert(sizeof(Counter) == 24, "Counter size");
_Static_assert(offsetof(Counter, value) == 8, "Counter value offset");
_Static_assert(offsetof(Counter, after) == 16, "Counter trailing canary offset");
)") + Source + R"(
int main(void) {
  static const uint64_t inputs[] = {
    UINT64_C(0), UINT64_C(1), UINT64_C(0x7f), UINT64_C(0x80),
    UINT64_C(0xff), UINT64_C(0x100), UINT64_C(0x1ff),
    UINT64_C(0xffffffff), UINT64_C(0x100000000),
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
    UINT64_C(0x8000000000000080), UINT64_MAX - UINT64_C(1), UINT64_MAX
  };
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    Counter counter = {UINT64_C(0x13579bdf2468ace0), inputs[i],
                       UINT64_C(0xfedcba9876543210)};
    const uint64_t expected = (uint64_t)(uint8_t)inputs[i];
    if (narrowed_member(&counter) != expected) return 1;
    if (counter.before != UINT64_C(0x13579bdf2468ace0) ||
        counter.value != inputs[i] ||
        counter.after != UINT64_C(0xfedcba9876543210)) return 2;
  }
  return 0;
}
)");
  }
}

TEST(HighValueForward, LoopConditionKeepsAValueTheLoopChanges) {
  // n = count + 1 before the loop, and the loop changes count under the
  // same name: the condition must keep reading n.
  HighFunc F = function(
      "bound",
      {assign(renamed(3), input()),
       assign(local(1), op(NdOp::INT_ADD, renamed(3), constant(1))),
       assign(renamed(2), constant(0)),
       loop(op(NdOp::INT_LESS, renamed(2), local(1), true),
            {assign(renamed(2), op(NdOp::INT_ADD, renamed(2), constant(1))),
             assign(renamed(3), op(NdOp::INT_ADD, renamed(3), constant(5)))}),
       result(renamed(2))});
  const std::string Source = emit({F});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t x = 0; x < 40; ++x)
    if (bound(x) != x + 1)
      return 1;
  return 0;
}
)");
}

TEST(HighValueForward, LoopConditionFoldsAValueTheLoopKeeps) {
  // The loop never assigns count, so n may print as count + 1.
  HighFunc F = function(
      "fixed",
      {assign(renamed(3), input()),
       assign(local(1), op(NdOp::INT_ADD, renamed(3), constant(1))),
       assign(renamed(2), constant(0)),
       loop(op(NdOp::INT_LESS, renamed(2), local(1), true),
            {assign(renamed(2), op(NdOp::INT_ADD, renamed(2), constant(1)))}),
       result(renamed(2))});
  const std::string Source = emit({F});
  EXPECT_EQ(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t x = 0; x < 40; ++x)
    if (fixed(x) != x + 1)
      return 1;
  return 0;
}
)");
}

ExprPtr frameSlot(uint64_t Distance) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = 900;
  V.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeBinop(NdOp::INT_SUB,
                             HighExpr::makeVar(V, NdType::makeInt(8, false)),
                             constant(Distance));
}

HighStmt store(ExprPtr Address, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Store;
  S.StoreAddr = std::move(Address);
  S.StoreVal = std::move(Value);
  return S;
}

TEST(HighValueForward, OutParameterPointersKeepTheirFieldsAcrossSlotUpdates) {
  auto Record = NdType::makeNamedRecord("OUT_RECORD", 12);
  Record->FieldDisplayNames = {"first", "middle", "last"};
  Record->FieldDisplayOffsets = {0, 4, 8};
  Record->FieldDisplayTypes.assign(3, NdType::makeInt(4));
  class RecordDebug : public NullDebugContext {
  public:
    explicit RecordDebug(TypeRef Record) : Record(std::move(Record)) {}
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x2000)
        return std::nullopt;
      FunctionSym Function;
      Function.Addr = Address;
      Function.Name = "next_record";
      Function.ReturnType = NdType::makeVoid();
      Function.Params = {{"out", NdType::makePtr(NdType::makePtr(Record))}};
      return Function;
    }
    bool hasInfo() const override { return true; }

  private:
    TypeRef Record;
  } Debug(Record);
  const auto Next = [] {
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall("next_record", 0x2000, {frameSlot(16)});
    Call.CallExpr->Type = NdType::makeVoid();
    return Call;
  };
  const auto Field = [](uint64_t Offset) {
    return HighExpr::makeLoad(op(NdOp::INT_ADD, local(1), constant(Offset)),
                              NdType::makeInt(4));
  };
  HighFunc Function = function(
      "saved_record",
      {Next(),
       assign(local(1), HighExpr::makeLoad(frameSlot(16), NdType::makeInt(8))),
       Next(), result(op(NdOp::INT_ADD, Field(4), Field(8)))});
  Function.Params.clear();
  Function.FrameSize = 64;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
    CRecordDefinitions Definitions;
    Definitions.Text = "typedef struct OUT_RECORD { int32_t first, middle, "
                       "last; } OUT_RECORD;\n";
    Definitions.Records = {{"OUT_RECORD", false}};
    Definitions.Tags = {{"OUT_RECORD", false}};
    return Definitions;
  };
  ASSERT_TRUE(HighCEmitter().emit({Function}, Out, Options, &Debug));
  Out.flush();
  EXPECT_NE(Source.find("->middle"), std::string::npos) << Source;
  EXPECT_NE(Source.find("->last"), std::string::npos) << Source;
  compileAndRun(Source + R"(
void next_record(OUT_RECORD **out) {
  static OUT_RECORD records[] = {
      {1, 2, 3}, {7, 11, 13}, {5, -2, -3}, {17, 19, 23}};
  static unsigned index;
  *out = &records[index++ % 4];
}
int main(void) {
  const uint64_t negative_words = (uint64_t)(uint32_t)-2 + (uint32_t)-3;
  return saved_record() != 5 || saved_record() != negative_words ||
         saved_record() != 5;
}
)");
}

/// Saves x in a slot, reads it back, then stores 5 to the slot at
/// \p Overwritten before returning what it read.
HighFunc reloaded(const std::string &Name, uint64_t Overwritten) {
  HighFunc F = function(
      Name, {store(frameSlot(16), input()),
             assign(local(1), HighExpr::makeLoad(frameSlot(16),
                                                 NdType::makeInt(8, false))),
             store(frameSlot(Overwritten), constant(5)), result(local(1))});
  F.FrameSize = 64;
  return F;
}

TEST(HighValueForward, SlotLoadStaysBeforeAStoreToItsSlot) {
  const std::string Source = emit({reloaded("same_slot", 16)});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) { return same_slot(7) == 7 && same_slot(9) == 9 ? 0 : 1; }
)");
}

TEST(HighValueForward, SlotLoadStaysBeforeAnAssignmentToItsSlot) {
  HighFunc F = reloaded("assigned_slot", 16);
  F.Body[2] =
      assign(HighExpr::makeLoad(frameSlot(16), NdType::makeInt(8, false)),
             constant(5));
  const std::string Source = emit({F});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) { return assigned_slot(7) == 7 && assigned_slot(9) == 9 ? 0 : 1; }
)");
}

TEST(HighValueForward, SlotLoadFoldsPastAStoreToAnotherSlot) {
  const std::string Source = emit({reloaded("other_slot", 32)});
  EXPECT_EQ(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) { return other_slot(7) == 7 && other_slot(9) == 9 ? 0 : 1; }
)");
}

TEST(HighValueForward, SlotLoadStaysBeforeAStoreComputedFromTheFrame) {
  // Two words at -40 and -32 are one array.  A store to the first plus a
  // run-time offset writes the second when the offset is 8, whether the
  // address goes through a pointer to the first word or is computed in
  // place, so the read of the second word stays before the store.
  const auto Word = NdType::makeInt(8, false);
  for (bool ThroughPointer : {true, false}) {
    std::vector<HighStmt> Body = {store(frameSlot(40), constant(11)),
                                  store(frameSlot(32), constant(22))};
    ExprPtr Base = frameSlot(40);
    if (ThroughPointer) {
      Body.push_back(assign(local(2, Word), frameSlot(40)));
      Base = local(2, Word);
    }
    Body.push_back(
        assign(local(1, Word), HighExpr::makeLoad(frameSlot(32), Word)));
    Body.push_back(store(op(NdOp::INT_ADD, Base, input()), constant(99)));
    Body.push_back(result(local(1, Word)));
    HighFunc F = function("array_slot", std::move(Body));
    F.FrameSize = 64;
    const std::string Source = emit({F});
    compileAndRun(Source + R"(
int main(void) { return array_slot(8) == 22 && array_slot(0) == 22 ? 0 : 1; }
)");
  }
}

TEST(HighValueForward, AnAtomicAddACopyCarriesStaysBeforeALoadAfterIt) {
  // The atomic add returns the old *arg0, the copy widens it, and the load
  // after the add reads the sum.  Folded through the copy,
  // `*arg0 ^ (uint64_t)__atomic_fetch_add(arg0, 1, ...)` would read *arg0
  // unsequenced beside the add (x64o209_aacqrel).
  const auto Half = NdType::makeInt(4, false);
  const auto Word = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(Half);
  auto Narrow = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.Size = 4;
    V.TheArch = Arch::X64;
    return HighExpr::makeVar(V, Half);
  };
  auto Widen = [&](ExprPtr Value) {
    auto E = HighExpr::makeUnary(NdOp::INT_ZEXT, std::move(Value));
    E->Type = Word;
    return E;
  };
  auto Add = HighExpr::makeBinop(NdOp::ATOMIC_ADD, typedParameter(0, Pointer),
                                 HighExpr::makeConst(1, 4));
  Add->Type = Half;
  Add->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  auto Sum = op(NdOp::INT_XOR, local(3, Word), local(2, Word));
  Sum->Type = Word;
  HighFunc F = function(
      "add_then_read",
      {assign(Narrow(1), Add), assign(local(2, Word), Widen(Narrow(1))),
       assign(local(3, Word),
              Widen(HighExpr::makeLoad(typedParameter(0, Pointer), Half))),
       result(Sum)});
  F.Params = {{"arg0", Pointer}};
  // This checks HighIR effect sequencing on the executing host. The writer
  // preserves target-native atomics, so x86 LOCK assembly cannot execute in
  // an ARM64 C fixture; both hosts must execute the same memory/result oracle.
  const auto NativeArch =
      llvm::Triple(llvm::sys::getDefaultTargetTriple()).isAArch64()
          ? Arch::AArch64
          : Arch::X64;
  const std::string Source = emit({F}, NativeArch);
  compileAndRun(Source + R"(
int main(void) {
  uint32_t value = 5;
  return add_then_read(&value) == (6 ^ 5) && value == 6 ? 0 : 1;
}
)");
}

TEST(HighValueForward, AnIndirectCallReadsItsTargetFromASlot) {
  // The function pointer is kept in a frame slot and called through it: the
  // slot is read like any other, and the frame that holds it is declared.
  auto Call = HighExpr::makeCall("indirect", 0, {constant(5)});
  Call->IsIndirectCall = true;
  Call->IndirectTarget =
      HighExpr::makeLoad(frameSlot(40), NdType::makeInt(8, false));
  Call->Type = NdType::makeInt(8, false);
  HighFunc F =
      function("through_slot", {store(frameSlot(40), input()), result(Call)});
  F.FrameSize = 64;
  const std::string Source = emit({F});
  compileAndRun(Source + R"(
static uint64_t twice(uint64_t x) { return 2 * x; }
int main(void) {
  return through_slot((uint64_t)(uintptr_t)twice) == 10 ? 0 : 1;
}
)");
}

TEST(HighValueForward, AForwardedConversionComparesInItsOwnType) {
  // t1 = (int8_t)wide, a byte of a 128-bit vector, declared uint8_t: its
  // definition prints in place as a signed byte, and the unsigned compare
  // `x < t1` (pmaxub's) must still compare unsigned bytes.
  MedVar Wide;
  Wide.Kind = MedVar::Param;
  Wide.Id = 0;
  Wide.Size = 16;
  Wide.TheArch = Arch::X64;
  MedVar Byte;
  Byte.Kind = MedVar::Param;
  Byte.Id = 1;
  Byte.Size = 1;
  Byte.TheArch = Arch::X64;
  const TypeRef U8 = NdType::makeInt(1, false);
  auto Lane = std::make_shared<HighExpr>();
  Lane->Kind = ExprKind::Cast;
  Lane->Type = Lane->CastTo = NdType::makeInt(1, true);
  Lane->Operands = {HighExpr::makeVar(Wide, NdType::makeInt(16, false))};
  MedVar T;
  T.Kind = MedVar::Temp;
  T.Id = 1;
  T.Size = 1;
  T.TheArch = Arch::X64;
  auto Less = HighExpr::makeBinop(NdOp::INT_LESS, HighExpr::makeVar(Byte, U8),
                                  HighExpr::makeVar(T, U8));
  Less->Type = NdType::makeInt(1, false);
  auto Result = HighExpr::makeUnary(NdOp::INT_ZEXT, Less);
  Result->Type = NdType::makeInt(8, false);
  HighFunc F = function(
      "byte_below", {assign(HighExpr::makeVar(T, U8), Lane), result(Result)});
  F.Params = {{"arg0", NdType::makeInt(16, false)}, {"arg1", U8}};
  const std::string Source = emit({F});
  compileAndRun(Source + R"(
int main(void) {
  const unsigned __int128 lanes = (unsigned __int128)0xA0A0A0A0A0A0A0A0ull;
  if (byte_below(lanes, 5) != 1 || byte_below(lanes, 0xB0) != 0)
    return 1;
  return byte_below(lanes, 0x9F) != 1;
}
)");
}

TEST(HighValueForward, AnExtendedLowByteKeepsItsConversion) {
  // t1 = (int64_t)(int8_t)x is read once: the use reads the sign-extended
  // low byte, not x, which the views around it merely surround.
  auto Low = HighExpr::makeBinop(NdOp::SUBBYTES, input(), constant(0));
  Low->Type = NdType::makeInt(1, true);
  auto Extended = HighExpr::makeUnary(NdOp::INT_SEXT, Low);
  Extended->Type = NdType::makeInt(8, true);
  HighFunc F =
      function("extended", {assign(local(1), Extended),
                            result(op(NdOp::INT_ADD, local(1), constant(1)))});
  const std::string Source = emit({F});
  compileAndRun(Source + R"(
int main(void) {
  const uint64_t xs[] = {0, 0x7F, 0x80, 0xFF, 0x1FF, 0x12345680};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i)
    if (extended(xs[i]) != (uint64_t)((int64_t)(int8_t)xs[i] + 1))
      return 1;
  return 0;
}
)");
}

TEST(HighValueForward, CopyKeepsTheValueItsSourceLoses) {
  // t1 copies v3, then v3 changes under the same name; t1 must keep the old
  // value, in straight-line code and around a loop.
  HighFunc Straight =
      function("copy_straight",
               {assign(renamed(3), input()), assign(local(1), renamed(3)),
                assign(renamed(3), op(NdOp::INT_ADD, renamed(3), constant(5))),
                result(op(NdOp::INT_MULT, local(1), renamed(3)))});
  HighFunc Looped = function(
      "copy_looped",
      {assign(renamed(3), input()), assign(local(1), renamed(3)),
       loop(op(NdOp::INT_LESS, renamed(3), constant(20), true),
            {assign(renamed(3), op(NdOp::INT_ADD, renamed(3), constant(7)))}),
       result(op(NdOp::INT_SUB, renamed(3), local(1)))});
  const std::string Source = emit({Straight, Looped});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
static uint64_t looped_ref(uint64_t x) { uint64_t c = x; while (x < 20) x += 7; return x - c; }
int main(void) {
  for (uint64_t x = 0; x < 30; ++x) {
    if (copy_straight(x) != x * (x + 5)) return 1;
    if (copy_looped(x) != looped_ref(x)) return 2;
  }
  return 0;
}
)");
}

HighStmt branch(ExprPtr Cond, std::vector<HighStmt> Then,
                std::vector<HighStmt> Else) {
  HighStmt S;
  S.Kind = StmtKind::IfElse;
  S.Cond = std::move(Cond);
  S.Body = std::move(Then);
  S.ElseBody = std::move(Else);
  return S;
}

TEST(HighValueForward, JoinOnlyHiddenCodeReadsStaysHidden) {
  // v3 joins t1 - 5 and 0.  In join_unread only a branch whose arms assign
  // a value nothing reads tests v3, so that branch prints nothing, and v3
  // with t1 must not print either: v3 without t1 names an undeclared
  // variable.  In join_read the return reads v3, which prints with t1.
  auto Body = [](HighStmt Last) {
    std::vector<HighStmt> Stmts{
        assign(local(1), op(NdOp::INT_MULT, input(), constant(3))),
        branch(op(NdOp::INT_AND, input(), constant(1)),
               {assign(renamed(3), op(NdOp::INT_SUB, local(1), constant(5)))},
               {assign(renamed(3), constant(0))})};
    Stmts.push_back(std::move(Last));
    return Stmts;
  };
  std::vector<HighStmt> Unread = Body(
      branch(op(NdOp::INT_EQUAL, renamed(3), constant(7), true),
             {assign(local(4), constant(1))}, {assign(local(4), constant(2))}));
  Unread.push_back(result(input()));
  HighFunc Hidden = function("join_unread", std::move(Unread));
  HighFunc Read = function("join_read", Body(result(renamed(3))));
  const std::string Source = emit({Hidden, Read});
  const size_t ReadAt = Source.find("join_read(");
  ASSERT_NE(ReadAt, std::string::npos) << Source;
  EXPECT_EQ(Source.substr(0, ReadAt).find(" - 5"), std::string::npos) << Source;
  EXPECT_NE(Source.find(" - 5", ReadAt), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t x = 0; x < 30; ++x) {
    if (join_unread(x) != x) return 1;
    if (join_read(x) != ((x & 1) ? x * 3 - 5 : 0)) return 2;
  }
  return 0;
}
)");
}

TEST(HighValueForward, StatusTestTakesTheCallItReads) {
  // t1 = probe(x) is read once, by the test right after it: the test calls
  // probe.  In tested_against_local the test also reads t2, so the call
  // keeps its statement.
  auto Probe = [] {
    auto Call = HighExpr::makeCall("probe", 0x2000, {input()});
    Call->Type = NdType::makeInt(8, true);
    return Call;
  };
  HighFunc Tested =
      function("tested", {assign(local(1), Probe()),
                          when(op(NdOp::INT_SLESS, local(1), constant(0), true),
                               {result(constant(1))}),
                          result(constant(2))});
  HighFunc Local =
      function("tested_against_local",
               {assign(local(2), op(NdOp::INT_ADD, input(), constant(1))),
                assign(local(1), Probe()),
                when(op(NdOp::INT_SLESS, local(1), local(2), true),
                     {result(constant(1))}),
                result(local(2))});
  const std::string Source = emit({Tested, Local});
  const size_t LocalAt = Source.find("tested_against_local(");
  ASSERT_NE(LocalAt, std::string::npos) << Source;
  EXPECT_NE(Source.substr(0, LocalAt).find("if (probe(arg0) < 0)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("t1 = probe(arg0);", LocalAt), std::string::npos)
      << Source;
  compileAndRun(Source + R"(
static unsigned calls;
int64_t probe(uint64_t x) { ++calls; return (int64_t)x - 10; }
int main(void) {
  for (uint64_t x = 0; x < 20; ++x) {
    calls = 0;
    if (tested(x) != (x < 10 ? 1 : 2) || calls != 1) return 1;
    if (tested_against_local(x) != 1 || calls != 2) return 2;
  }
  return 0;
}
)");
}

} // namespace
