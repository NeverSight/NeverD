#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <stdexcept>

using namespace neverd;

namespace neverd {
void coalesceBranchEntryStatements(HighFunc &Func);
void eliminateUnusedValues(std::vector<HighStmt> &);
void structureIfElse(HighFunc &, int, const MedFunc * = nullptr);
void foldStructuredContinuations(HighFunc &, const MedFunc * = nullptr);
} // namespace neverd

namespace {
ExprPtr parameter(unsigned Id, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Type->Size;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, Type);
}

HighFunc returning(llvm::StringRef Name, ExprPtr Value,
                   std::vector<TypeRef> Parameters = {}) {
  HighFunc Function;
  Function.Name = Name.str();
  Function.ReturnType = Value->Type;
  for (size_t I = 0; I < Parameters.size(); ++I)
    Function.Params.push_back({"arg" + std::to_string(I), Parameters[I]});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Value);
  Function.Body.push_back(std::move(Return));
  return Function;
}

ExprPtr call(SourceCallTypeHint Hint, TypeRef Carrier,
             std::vector<ExprPtr> Arguments = {}) {
  auto Result = HighExpr::makeCall(Hint.TargetName, Hint.TargetAddress,
                                   std::move(Arguments));
  Result->Type = std::move(Carrier);
  Result->SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Hint));
  return Result;
}

SourceCallTypeHint native(llvm::StringRef Name, TypeRef Return,
                          std::vector<TypeRef> Arguments) {
  SourceCallTypeHint Hint;
  Hint.TargetName = Name.str();
  Hint.Signature.ReturnType = std::move(Return);
  for (const auto &Type : Arguments)
    Hint.Signature.Parameters.push_back({"", Type});
  return Hint;
}

std::string emit(const std::vector<HighFunc> &Functions, bool Includes = true) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = Includes;
  Options.EmitComments = false;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  return Result;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      "-O1",
      "-fno-inline",
      "-fblocks",
      "-Werror=implicit-function-declaration",
      "-Werror=return-type",
      SourcePath,
      "-o",
      BinaryPath};
  std::string Error;
  const int Compiled = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Compiled, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << "\n"
                         << Source;
  const int Ran = llvm::sys::ExecuteAndWait(
      BinaryPath, {BinaryPath}, std::nullopt, Redirects, 30, 0, &Error);
  ASSERT_EQ(Ran, 0) << Error << "\n" << Source;
}

TEST(HighCSourceCalls, ExactNarrowZeroSuppliesOnlyPointerNullArguments) {
  auto Binding = native("fixture_pointer_consumer", NdType::makeVoid(),
                        {NdType::makePtr(NdType::makeVoid())});
  Binding.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;

  const auto Render = [&](uint64_t Value) {
    HighFunc Function;
    Function.Name = "null_pointer_caller";
    Function.ReturnType = NdType::makeVoid();
    HighStmt Statement;
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr = call(Binding, NdType::makeVoid(),
                              {HighExpr::makeConst(Value, 4)});
    Function.Body = {std::move(Statement)};
    return emit({Function});
  };

  const auto Null = Render(0);
  EXPECT_NE(Null.find("fixture_pointer_consumer((void*)0)"),
            std::string::npos)
      << Null;
  EXPECT_EQ(Null.find("bad source call"), std::string::npos) << Null;

  const auto Nonnull = Render(1);
  EXPECT_NE(Nonnull.find("bad source call"), std::string::npos) << Nonnull;
}

TEST(HighCSourceCalls, ConditionalCopiesKeepTheirReachingDefinitions) {
  const auto Integer = NdType::makeInt(8);
  MedVar Temporary;
  Temporary.Kind = MedVar::Temp;
  Temporary.Id = 19;
  Temporary.Size = 8;
  auto Value = HighExpr::makeVar(Temporary, Integer);
  auto Function =
      returning("conditional_copy", Value, {Integer, Integer, Integer});
  HighStmt Initial;
  Initial.Kind = StmtKind::Assign;
  Initial.Dst = Value;
  Initial.Val = parameter(0, Integer);
  HighStmt Replacement = Initial;
  Replacement.Val = parameter(2, Integer);
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = parameter(1, Integer);
  Branch.Body = {Replacement};
  Function.Body.insert(Function.Body.begin(), {Initial, Branch});
  compileAndRun(emit({Function}) + R"(
int main(void) {
    return conditional_copy(17, 0, 91) == 17 &&
           conditional_copy(17, 1, 91) == 91 ? 0 : 1;
}
)");
}

TEST(HighCSourceCalls, BytePointerCarriersAllowMachineBitOperations) {
  const auto Pointer = NdType::makePtr(NdType::makeInt(1));
  auto Shift = HighExpr::makeBinop(NdOp::INT_RIGHT, parameter(0, Pointer),
                                   HighExpr::makeConst(4, 8));
  Shift->Type = NdType::makeInt(8, false);
  auto Function = returning("pointer_bits", Shift, {Pointer});
  compileAndRun(emit({Function}) + R"(
int main(void) {
    int8_t bytes[64];
    return pointer_bits(bytes) == ((uintptr_t)bytes >> 4) ? 0 : 1;
}
)");
}

TEST(HighCSourceCalls, NegatedFloatingComparisonsPreserveNaNsAndPrecedence) {
  const auto Floating = NdType::makeFloat(8);
  const auto Boolean = NdType::makeInt(1, false);
  std::vector<HighFunc> Functions;
  for (const auto &[Op, Name] :
       {std::pair{NdOp::FLOAT_EQUAL, "not_equal"},
        std::pair{NdOp::FLOAT_NOTEQUAL, "not_unequal"},
        std::pair{NdOp::FLOAT_LESS, "not_less"},
        std::pair{NdOp::FLOAT_LESSEQUAL, "not_less_equal"}}) {
    auto Compare =
        HighExpr::makeBinop(Op, parameter(0, Floating), parameter(1, Floating));
    Compare->Type = Boolean;
    auto Negated = HighExpr::makeUnary(NdOp::BOOL_NOT, Compare);
    Negated->Type = Boolean;
    Functions.push_back(returning(Name, Negated, {Floating, Floating}));
  }
  auto Xor = HighExpr::makeBinop(NdOp::BOOL_XOR, parameter(0, Boolean),
                                 parameter(1, Boolean));
  Xor->Type = Boolean;
  auto Negated = HighExpr::makeUnary(NdOp::BOOL_NOT, Xor);
  Negated->Type = Boolean;
  Functions.push_back(returning("not_xor", Negated, {Boolean, Boolean}));
  compileAndRun(emit(Functions) + R"(
int main(void) {
    double values[] = {0.0, -0.0, 1.0, 2.5, -2.5,
                       __builtin_nan(""), __builtin_inf(), -__builtin_inf()};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            double a = values[i], b = values[j];
            if (not_equal(a, b) != !(a == b) ||
                not_unequal(a, b) != !(a != b) ||
                not_less(a, b) != !(a < b) ||
                not_less_equal(a, b) != !(a <= b)) return 1;
        }
    for (unsigned a = 0; a < 2; ++a)
        for (unsigned b = 0; b < 2; ++b)
            if (not_xor(a, b) != !(a ^ b)) return 2;
    return 0;
}
)");
}

TEST(HighCSourceCalls, ReorderedComparisonsKeepTheWideOperand) {
  std::vector<HighFunc> Functions;
  const auto Wide = NdType::makeInt(8, false);
  for (const auto &[Op, Name] : {std::pair{NdOp::INT_LESS, "u_lt"},
                                 std::pair{NdOp::INT_LESSEQUAL, "u_le"},
                                 std::pair{NdOp::INT_SLESS, "s_lt"},
                                 std::pair{NdOp::INT_SLESSEQUAL, "s_le"}}) {
    auto Compare =
        HighExpr::makeBinop(Op, HighExpr::makeConst(61, 4), parameter(0, Wide));
    Compare->Type = NdType::makeInt(1, false);
    Functions.push_back(returning(Name, Compare, {Wide}));
    const auto Narrow = NdType::makeInt(4);
    for (bool Swap : {false, true}) {
      auto Left = parameter(0, Narrow), Right = parameter(1, Wide);
      if (Swap)
        std::swap(Left, Right);
      auto Mixed = HighExpr::makeBinop(Op, Left, Right);
      Mixed->Type = NdType::makeInt(1, false);
      Functions.push_back(
          returning(std::string(Name) + (Swap ? "_swap" : "_bits"), Mixed,
                    {Narrow, Wide}));
    }
  }
  compileAndRun(emit(Functions) + R"(
int main(void) {
    uint64_t values[] = {0, 60, 61, 62, UINT32_MAX,
                        UINT64_C(0x100000001), UINT64_C(0x10000003d),
                        UINT64_C(0x7fffffffffffffff),
                        UINT64_C(0x8000000000000000), UINT64_MAX};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint64_t x = values[i];
        if (u_lt(x) != (UINT64_C(61) < x) ||
            u_le(x) != (UINT64_C(61) <= x) ||
            s_lt(x) != (INT64_C(61) < (int64_t)x) ||
            s_le(x) != (INT64_C(61) <= (int64_t)x)) return 1;
        int32_t small[] = {0, 61, -1, INT32_MIN, INT32_MAX};
        for (unsigned j = 0; j < sizeof(small) / sizeof(small[0]); ++j) {
            int32_t n = small[j];
            uint64_t z = (uint32_t)n;
            if (u_lt_bits(n, x) != (z < x) || u_le_bits(n, x) != (z <= x) ||
                u_lt_swap(n, x) != (x < z) || u_le_swap(n, x) != (x <= z) ||
                s_lt_bits(n, x) != ((int64_t)z < (int64_t)x) ||
                s_le_bits(n, x) != ((int64_t)z <= (int64_t)x) ||
                s_lt_swap(n, x) != ((int64_t)x < (int64_t)z) ||
                s_le_swap(n, x) != ((int64_t)x <= (int64_t)z)) return 2;
        }
    }
    return 0;
}
)");
}

TEST(HighCSourceCalls, SwiftValueWitnessDestroyReloadsRuntimeTableEntry) {
  const auto Hint = swiftValueWitnessSourceCallHint(
      Arch::X64, SourceCallTypeHint::SwiftValueWitnessKind::Destroy);
  ASSERT_TRUE(Hint);
  auto Destroy =
      HighExpr::makeCall("indirect_call", 0,
                         {parameter(0, Hint->Signature.Parameters[0].Type),
                          parameter(1, Hint->Signature.Parameters[1].Type)});
  Destroy->IsIndirectCall = true;
  Destroy->SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Hint);
  HighFunc Function;
  Function.Name = "destroy_value";
  Function.ReturnType = NdType::makeVoid();
  Function.Params = {{"value", Hint->Signature.Parameters[0].Type},
                     {"metadata", Hint->Signature.Parameters[1].Type}};
  Function.SourceTypeHint = Hint->Signature;
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = Destroy;
  Function.Body = {Call};
  const auto Source = emit({Function});
  EXPECT_NE(Source.find("__attribute__((swiftcall))"), std::string::npos);
  EXPECT_NE(Source.find("sizeof(void *)))[1]"), std::string::npos);
  compileAndRun(Source + R"(
static void *expected_metadata;
static unsigned calls;
static void __attribute__((swiftcall)) witness(void *value, void *metadata) {
    if (metadata != expected_metadata) __builtin_trap();
    ++*(unsigned *)value;
    ++calls;
}
int main(void) {
    void *table[2] = {0, (void *)&witness};
    void *metadata_words[2] = {table, 0};
    expected_metadata = &metadata_words[1];
    unsigned value = 41;
    destroy_value(&value, expected_metadata);
    return value == 42 && calls == 1 ? 0 : 1;
}
)");

  auto Forged = *Hint;
  Forged.TargetName = "other";
  Destroy->SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Forged));
  EXPECT_NE(emit({Function}, false)
                .find("bad source call: invalid Swift value-witness binding"),
            std::string::npos);
}

TEST(HighCSourceCalls,
     SwiftValueWitnessInitializeWithCopyReturnsRuntimeDestination) {
  using OperationKind = SourceCallTypeHint::SwiftValueWitnessKind;
  const std::pair<OperationKind, unsigned> Cases[] = {
      {OperationKind::InitializeBufferWithCopyOfBuffer, 0},
      {OperationKind::InitializeWithCopy, 2},
      {OperationKind::AssignWithCopy, 3},
      {OperationKind::InitializeWithTake, 4},
      {OperationKind::AssignWithTake, 5},
  };
  for (const auto &[Operation, ExpectedSlot] : Cases) {
    const auto Hint = swiftValueWitnessSourceCallHint(Arch::X64, Operation);
    ASSERT_TRUE(Hint);
    auto Copy =
        HighExpr::makeCall("indirect_call", 0,
                           {parameter(0, Hint->Signature.Parameters[0].Type),
                            parameter(1, Hint->Signature.Parameters[1].Type),
                            parameter(2, Hint->Signature.Parameters[2].Type)});
    Copy->IsIndirectCall = true;
    // HighIR retains the integer machine carrier even though the source ABI
    // gives the same return register a pointer type.
    Copy->Type = NdType::makeInt(8);
    Copy->SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Hint);
    HighFunc Function;
    Function.Name = "copy_value";
    Function.ReturnType = Copy->Type;
    Function.Params = {{"destination", Hint->Signature.Parameters[0].Type},
                       {"source", Hint->Signature.Parameters[1].Type},
                       {"metadata", Hint->Signature.Parameters[2].Type}};
    Function.SourceTypeHint = Hint->Signature;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Copy;
    Function.Body = {Return};
    const auto Source = emit({Function});
    const auto Slot = swiftValueWitnessSlot(Operation);
    ASSERT_TRUE(Slot);
    EXPECT_EQ(*Slot, ExpectedSlot);
    EXPECT_NE(
        Source.find("sizeof(void *)))[" + std::to_string(ExpectedSlot) + "]"),
        std::string::npos);
    compileAndRun(Source + R"(
static void *expected_metadata;
static void *__attribute__((swiftcall))
witness_copy(void *destination, void *source, void *metadata) {
    if (metadata != expected_metadata) __builtin_trap();
    *(unsigned *)destination = *(unsigned *)source;
    return destination;
}
int main(void) {
    void *table[8] = {0};
)" + "    table[" +
                  std::to_string(ExpectedSlot) +
                  "] = (void *)&witness_copy;\n" + R"(
    void *metadata_words[2] = {table, 0};
    expected_metadata = &metadata_words[1];
    unsigned source = 73, destination = 0;
    uintptr_t result = copy_value(&destination, &source, expected_metadata);
    return (void *)result == &destination && destination == source ? 0 : 1;
}
)");
  }
}

TEST(HighCSourceCalls, SwiftSinglePayloadWitnessesPreserveUnsignedTagABI) {
  using Operation = SourceCallTypeHint::SwiftValueWitnessKind;
  std::vector<HighFunc> Functions;
  for (const auto Op : {Operation::GetEnumTagSinglePayload,
                        Operation::StoreEnumTagSinglePayload}) {
    const auto Hint = swiftValueWitnessSourceCallHint(Arch::X64, Op);
    ASSERT_TRUE(Hint);
    HighFunc Function;
    Function.Name = Hint->TargetName;
    Function.ReturnType = Hint->Signature.ReturnType;
    Function.SourceTypeHint = Hint->Signature;
    std::vector<ExprPtr> Arguments;
    for (const auto &P : Hint->Signature.Parameters) {
      Arguments.push_back(parameter(Arguments.size(), P.Type));
      Function.Params.push_back({P.Name, P.Type});
    }
    auto Call = call(*Hint, Hint->Signature.ReturnType, std::move(Arguments));
    Call->IsIndirectCall = true;
    HighStmt Statement;
    if (Op == Operation::GetEnumTagSinglePayload) {
      EXPECT_EQ(Hint->Signature.ReturnType->Size, 4U);
      EXPECT_FALSE(Hint->Signature.ReturnType->IsSigned);
      Statement.Kind = StmtKind::Return;
      Statement.RetVal = Call;
    } else {
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = Call;
    }
    Function.Body = {Statement};
    Functions.push_back(Function);
  }
  compileAndRun(emit(Functions) + R"(
static void *expected_metadata;
static unsigned reads, writes;
static uint32_t __attribute__((swiftcall))
get_tag(void *value, uint32_t empty_cases, void *metadata) {
    if (metadata != expected_metadata || empty_cases != 0x87654321u)
        __builtin_trap();
    ++reads;
    return *(uint32_t *)value;
}
static void __attribute__((swiftcall))
store_tag(void *value, uint32_t tag, uint32_t empty_cases, void *metadata) {
    if (metadata != expected_metadata || empty_cases != 0x87654321u)
        __builtin_trap();
    ++writes;
    *(uint32_t *)value = tag;
}
int main(void) {
    void *table[8] = {0};
    table[6] = (void *)&get_tag;
    table[7] = (void *)&store_tag;
    void *metadata_words[2] = {table, 0};
    expected_metadata = &metadata_words[1];
    uint32_t value = 0;
    storeEnumTagSinglePayload(&value, 0xfedcba98u, 0x87654321u, expected_metadata);
    uint32_t tag = getEnumTagSinglePayload(&value, 0x87654321u, expected_metadata);
    return tag == 0xfedcba98u && reads == 1 && writes == 1 ? 0 : 1;
}
)");
}

TEST(HighCSourceCalls, GuardedPhiCleanupKeepsTargetLabelsExecutable) {
  const auto Integer = NdType::makeInt(4);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 10;
  Variable.Size = 4;
  auto Value = HighExpr::makeVar(Variable);
  auto Function = returning("guarded_value", Value, {Integer});
  auto Return = Function.Body.back();
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  HighStmt Enter;
  Enter.Kind = StmtKind::If;
  Enter.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0, Integer),
                                   HighExpr::makeConst(0, 4));
  Enter.Body = {Jump};
  HighStmt Defined;
  Defined.Kind = StmtKind::Assign;
  Defined.Dst = Value;
  Defined.Val = HighExpr::makeConst(42, 4);
  HighStmt Copy = Defined;
  ++Variable.Id;
  Copy.Val = HighExpr::makeVar(Variable);
  Copy.IsPhiCopy = true;
  Copy.Addr = Jump.GotoTarget;
  HighStmt Define;
  Define.Kind = StmtKind::IfElse;
  Define.Cond = parameter(0, Integer);
  Define.Body = {Defined};
  Define.ElseBody = {Copy};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Cond = parameter(0, Integer);
  Use.Body = {Return};
  Return.RetVal = HighExpr::makeConst(7, 4);
  Function.Body = {Enter, Define, Use, Return};
  EXPECT_FALSE(analyzeHighSourceFlow(Function, true).Items.empty());
  ASSERT_TRUE(eliminateHighDeadPhiCopies(Function));
  EXPECT_EQ(Function.Body[1].ElseBody[0].Addr, Jump.GotoTarget);
  const auto Report = analyzeHighSourceFlow(Function, true);
  ASSERT_TRUE(Report.Complete);
  EXPECT_TRUE(Report.Items.empty());
  compileAndRun(R"(
#if defined(__clang__)
#pragma clang diagnostic error "-Wc2x-extensions"
#endif
)" + emit({Function}) +
                R"(
int main(void) {
    for (int condition = -256; condition <= 256; ++condition)
        if (guarded_value(condition) != (condition ? 42 : 7)) return 1;
    return 0;
}
)");
}

TEST(HighCSourceCalls, ConditionalDefaultPreservesResultsAndCallCounts) {
  const auto Integer = NdType::makeInt(4);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 10;
  Variable.Size = 4;
  auto Value = HighExpr::makeVar(Variable);
  auto Function = returning("find_or_create", Value, {Integer, Integer});
  auto Return = Function.Body.back();
  Return.Addr = 0x1040;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = Return.Addr;
  HighStmt Entry;
  Entry.Kind = StmtKind::If;
  Entry.Addr = 0x1000;
  Entry.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0, Integer),
                                   HighExpr::makeConst(0, 4));
  Entry.Body = {Jump};
  Entry.Body[0].GotoTarget = 0x1030;
  HighStmt Lookup;
  Lookup.Kind = StmtKind::Assign;
  Lookup.Addr = 0x1008;
  Lookup.Dst = Value;
  Lookup.Val = call(native("lookup_once", Integer, {Integer}), Integer,
                    {parameter(1, Integer)});
  HighStmt Found;
  Found.Kind = StmtKind::If;
  Found.Addr = 0x1010;
  Found.Cond =
      HighExpr::makeBinop(NdOp::INT_NOTEQUAL, Value, HighExpr::makeConst(0, 4));
  Found.Body = {Jump};
  auto Create = Lookup;
  Create.Addr = 0x1018;
  Create.Val = call(native("create_once", Integer, {}), Integer);
  auto Default = Lookup;
  Default.Addr = 0x1030;
  Default.Val = HighExpr::makeConst(0, 4);
  Jump.Addr = 0x1020;
  Function.Body = {Entry, Lookup, Found, Create, Jump, Default, Return};
  structureIfElse(Function, 10);
  compileAndRun(emit({Function}) + R"(
static unsigned lookups, creations;
int32_t lookup_once(int32_t value) { ++lookups; return value; }
int32_t create_once(void) { ++creations; return 19; }
int main(void) {
    for (int key = -32; key <= 32; ++key) {
        for (int value = -32; value <= 32; ++value) {
            unsigned before_lookup = lookups, before_create = creations;
            if (find_or_create(key, value) != (key ? (value ? value : 19) : 0))
                return 1;
            if (lookups - before_lookup != (key != 0)) return 2;
            if (creations - before_create != (key != 0 && value == 0)) return 3;
        }
    }
    return 0;
}
)");
}

TEST(HighCSourceCalls, ConditionalRunsPreserveSharedLoopPhiContinuations) {
  const auto Integer = NdType::makeInt(8);
  for (unsigned Mode = 0; Mode < 5; ++Mode)
    for (va_t EdgeAddress : {va_t(0), va_t(0x2108), va_t(0x2180)}) {
      SCOPED_TRACE(EdgeAddress);
      SCOPED_TRACE(Mode);
      auto Local = [&](int Id) {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.Id = Id;
        V.Size = 8;
        return HighExpr::makeVar(V, Integer);
      };
      auto Assign = [](va_t Address, ExprPtr Destination, ExprPtr Value) {
        HighStmt S;
        S.Kind = StmtKind::Assign;
        S.Addr = Address;
        S.Dst = std::move(Destination);
        S.Val = std::move(Value);
        return S;
      };
      auto Jump = [](va_t Target) {
        HighStmt S;
        S.Kind = StmtKind::Goto;
        S.GotoTarget = Target;
        return S;
      };
      const auto Accumulator = Local(10), Count = Local(11), Result = Local(12);
      auto Function =
          returning("shared_loop_phi", Result, {Integer, Integer, Integer});
      Function.Entry = 0x1000;
      auto Return = Function.Body.back();
      Return.Addr = 0x2200;
      HighStmt Choose;
      Choose.Kind = StmtKind::If;
      Choose.Addr = Function.Entry;
      Choose.Cond = parameter(0, Integer);
      Choose.Body = {Jump(0x2000)};
      HighStmt Loop;
      Loop.Kind = StmtKind::While;
      Loop.LoopHeaderAddr = 0x2100;
      Loop.Cond = Mode == 1 ? parameter(0, Integer)
                            : HighExpr::makeConst(Mode == 3 ? 0 : 1, 1);
      HighStmt Stop;
      Stop.Kind = StmtKind::If;
      Stop.Addr = 0x2108;
      Stop.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, Count,
                                      HighExpr::makeConst(0, 8));
      HighStmt Break;
      Break.Kind = StmtKind::Break;
      Stop.Body = {Break};
      Loop.Body = {Assign(0x2100, Accumulator,
                          HighExpr::makeBinop(NdOp::INT_ADD, Accumulator,
                                              HighExpr::makeConst(3, 8))),
                   Assign(0x2104, Count,
                          HighExpr::makeBinop(NdOp::INT_SUB, Count,
                                              HighExpr::makeConst(1, 8))),
                   Stop};
      if (Mode == 2)
        Loop.Body.insert(
            Loop.Body.begin(),
            Assign(0x20f0, Accumulator,
                   HighExpr::makeBinop(NdOp::INT_ADD, Accumulator,
                                       HighExpr::makeConst(100, 8))));
      auto Phi = Assign(EdgeAddress, Result, Accumulator);
      Phi.IsPhiCopy = true;
      Function.Body = {Assign(0, Count, parameter(1, Integer)),
                       Choose,
                       Assign(0x1004, Accumulator, HighExpr::makeConst(1, 8)),
                       Jump(0x2100),
                       Assign(0x2000, Accumulator, HighExpr::makeConst(7, 8)),
                       Loop,
                       Phi,
                       Return};
      if (Mode == 4) {
        HighStmt Initialize;
        Initialize.Kind = StmtKind::IfElse;
        Initialize.Cond = parameter(0, Integer);
        Initialize.Body = {
            Assign(0x2000, Accumulator, HighExpr::makeConst(7, 8))};
        Initialize.ElseBody = {
            Assign(0x2004, Accumulator, HighExpr::makeConst(1, 8))};
        HighStmt Enter;
        Enter.Kind = StmtKind::IfElse;
        Enter.Addr = 0x1000;
        Enter.Cond = parameter(2, Integer);
        Enter.Body = {Assign(0, Count, parameter(1, Integer)), Initialize,
                      Jump(0x2100)};
        auto EarlyReturn = Return;
        EarlyReturn.Addr = 0x1800;
        EarlyReturn.RetVal = HighExpr::makeConst(0, 8);
        Function.Body = {Enter, EarlyReturn, Loop, Phi, Return};
      }
      for (bool Structured : {false, true}) {
        SCOPED_TRACE(Structured);
        if (Structured) {
          structureIfElse(Function, 6);
          foldStructuredContinuations(Function);
        }
        coalesceBranchEntryStatements(Function);
        EXPECT_TRUE(analyzeHighSourceFlow(Function, true).Complete);
        if (Structured && (Mode == 0 || Mode == 4))
          walkStmts(Function.Body, [](const HighStmt &S) {
            EXPECT_NE(S.Kind, StmtKind::Goto);
          });
        compileAndRun("#define TEST_LOOP_MODE " + std::to_string(Mode) + "\n" +
                      emit({Function}) + R"(
int main(void) {
    for (int selected = 0; selected < 2; ++selected)
        for (int count = 1; count < 100; ++count)
            for (int enter = 0; enter < 2; ++enter) {
                int64_t expected = (selected ? 7 : 1) + count * 3;
                if (TEST_LOOP_MODE == 1 && !selected) expected = 4;
                if (TEST_LOOP_MODE == 2)
                    expected += 100 * (count - (selected ? 0 : 1));
                if (TEST_LOOP_MODE == 3) expected = selected ? 7 : 4;
                if (TEST_LOOP_MODE == 4 && !enter) expected = 0;
                if (shared_loop_phi(selected, count, enter) != expected) return 1;
            }
    return 0;
}
)");
      }
    }
}

TEST(HighCSourceCalls, SharedNativeEntryExecutesCallAndPhiExactlyOnce) {
  const auto Integer = NdType::makeInt(4);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 10;
  Variable.Size = 4;
  const auto Local = HighExpr::makeVar(Variable, Integer);
  auto Function = returning("entry_group", Local, {Integer});
  auto Return = Function.Body.back();
  Return.Addr = 0x1300;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x1220;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Addr = 0x1200;
  Branch.Cond = parameter(0, Integer);
  Branch.Body = {Jump};
  HighStmt Initial;
  Initial.Kind = StmtKind::Assign;
  Initial.Addr = 0x1204;
  Initial.Dst = Local;
  Initial.Val = HighExpr::makeConst(7, 4);
  HighStmt Skip = Jump;
  Skip.GotoTarget = Return.Addr;
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.Addr = 0x1220;
  Call.CallExpr = call(native("observe_entry", NdType::makeVoid(), {Integer}),
                       NdType::makeVoid(), {HighExpr::makeConst(5, 4)});
  HighStmt Phi = Initial;
  Phi.Addr = Call.Addr;
  Phi.Val = HighExpr::makeConst(42, 4);
  Phi.IsPhiCopy = true;
  Function.Body = {Branch, Initial, Skip, Call, Phi, Return};
  coalesceBranchEntryStatements(Function);
  compileAndRun(emit({Function}) + R"(
static int calls;
void observe_entry(int32_t value) { calls = calls * 10 + value; }
int main(void) {
    if (entry_group(0) != 7 || calls != 0) return 1;
    if (entry_group(1) != 42 || calls != 5) return 2;
    if (entry_group(0) != 7 || calls != 5) return 3;
    return 0;
}
)");
}

TEST(HighCSourceCalls, CallbackDeclaratorsPreserveArgumentsAndReturnTypes) {
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Callback =
      NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
  EXPECT_EQ(typeToC(Callback), "void (*)(void*)");
  EXPECT_EQ(declarationToC(Callback, "fn"), "void (*fn)(void*)");
  EXPECT_EQ(declarationToC(NdType::makePtr(Callback), "slot"),
            "void (**slot)(void*)");
  EXPECT_EQ(
      declarationToC(NdType::makePtr(NdType::makeFunc(Callback)), "factory"),
      "void (*(*factory)(void))(void*)");
  auto Cycle = NdType::makePtr();
  Cycle->Pointee = Cycle;
  EXPECT_THROW(typeToC(Cycle), std::invalid_argument);
  Cycle->Pointee.reset();
  EXPECT_THROW(typeToC(NdType::makePtr(NdType::makeFunc(nullptr))),
               std::invalid_argument);
  auto Identity =
      returning("callback_identity", parameter(0, Callback), {Callback});
  auto Forward =
      returning("callback_forward",
                call(native("callback_identity", Callback, {Callback}),
                     Callback, {parameter(0, Callback)}),
                {Callback});
  auto Apply =
      returning("callback_apply", parameter(1, Pointer), {Callback, Pointer});
  HighStmt Invoke;
  Invoke.Kind = StmtKind::Call;
  Invoke.CallExpr =
      call(native("consume_callback", NdType::makeVoid(), {Callback, Pointer}),
           NdType::makeVoid(), {parameter(0, Callback), parameter(1, Pointer)});
  Apply.Body.insert(Apply.Body.begin(), Invoke);
  const auto Source = emit({Forward, Identity, Apply});
  EXPECT_NE(Source.find("void (*callback_identity(void (*)(void*)))(void*);"),
            std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  auto WrongIdentity = Identity;
  WrongIdentity.Params[0].Type =
      NdType::makePtr(NdType::makeFunc(NdType::makeInt(8), {Pointer}));
  EXPECT_NE(emit({Forward, WrongIdentity})
                .find("bad source call: native parameter disagrees"),
            std::string::npos);
  compileAndRun(Source + R"(
void consume_callback(void (*fn)(void*), void *context) { fn(context); }
static void add(void *p) { ++*(int*)p; }
static void subtract(void *p) { --*(int*)p; }
int main(void) {
  int value = 7;
  for (int i = 0; i < 128; ++i) {
    void (*fn)(void*) = callback_forward(i % 3 ? add : subtract);
    if (fn != (i % 3 ? add : subtract)) return 1;
    int expected = value + (i % 3 ? 1 : -1);
    if (callback_apply(fn, &value) != &value || value != expected) return 2;
  }
  return 0;
}
)");
}

TEST(HighCSourceCalls, RuntimeCallbackPreservesPredicateAndContextOperands) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.Arch = Arch::X64;
  Image.ImportPtrSlots[0x2180] = "_swift_once";
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  const auto Pointer = Hint->Signature.Parameters[0].Type;
  const auto Callback = Hint->Signature.Parameters[1].Type;
  auto Forward = returning("once_forward", parameter(2, Pointer),
                           {Pointer, Callback, Pointer});
  HighStmt Invoke;
  Invoke.Kind = StmtKind::Call;
  Invoke.CallExpr = call(
      *Hint, NdType::makeVoid(),
      {parameter(0, Pointer), parameter(1, Callback), parameter(2, Pointer)});
  Forward.Body.insert(Forward.Body.begin(), Invoke);
  const auto Source = emit({Forward});
  EXPECT_NE(
      Source.find("extern void swift_once(void*, void (*)(void*), void*);"),
      std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  // The portable model checks the call boundary. It does not claim to test
  // the runtime's concurrency algorithm or ownership of rebuilt predicates.
  compileAndRun(Source + R"(
static int calls, observed[2];
static uintptr_t predicates[2];
static void initialize(void *context) { ++*(int*)context; }
void swift_once(void *predicate, void (*fn)(void*), void *context) {
  ++calls;
  if (!*(uintptr_t*)predicate) {
    fn(context);
    *(uintptr_t*)predicate = UINTPTR_MAX;
  }
}
int main(void) {
  for (int i = 0; i < 128; ++i) {
    int index = i % 2;
    if (once_forward(&predicates[index], initialize, &observed[index]) !=
        &observed[index]) return 1;
  }
  return calls != 128 || observed[0] != 1 || observed[1] != 1 ||
         predicates[0] != UINTPTR_MAX || predicates[1] != UINTPTR_MAX;
}
)");
}

TEST(HighCSourceCalls, SwiftOnceAddressorUsesRebuiltHelper) {
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor;
  Hint.TargetAddress = 0x1234;
  Hint.TargetName = "_$s4Test5valueSo8NSObjectCvau";
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Hint.Signature, Arch::X64, Error));
  auto Address = call(Hint, Hint.Signature.ReturnType);
  Address->CallTarget.clear();
  const auto Source = emit({returning("read_value", Address)});
  EXPECT_NE(Source.find("extern uintptr_t neverd_swift_once_accessor_1234("),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("neverd_swift_once_accessor_1234()"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
}

TEST(HighCSourceCalls, NoncontiguousOrNestedEntriesRemainAmbiguous) {
  for (bool Nested : {false, true}) {
    auto Function = returning("ambiguous_entry", HighExpr::makeConst(1, 4));
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1220;
    HighStmt First;
    First.Kind = StmtKind::Block;
    First.Addr = 0x1220;
    HighStmt Second = First;
    Second.IsPhiCopy = true;
    HighStmt Separator;
    Separator.Kind = StmtKind::Block;
    Separator.Addr = 0x1230;
    if (Nested) {
      Separator.Body.push_back(Second);
      Function.Body = {Jump, First, Separator, Function.Body.back()};
    } else {
      Function.Body = {Jump, First, Separator, Second, Function.Body.back()};
    }
    coalesceBranchEntryStatements(Function);
    unsigned Entries = 0;
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      Entries += Statement.Addr == 0x1220;
    });
    EXPECT_EQ(Entries, 2U);
  }
}

TEST(HighCSourceCalls, ConditionalEntryEvaluatesBeforeItsTakenEdgeCopies) {
  for (bool ExplicitElse : {false, true}) {
    const auto Integer = NdType::makeInt(4);
    MedVar Variable;
    Variable.Kind = MedVar::Temp;
    Variable.Id = 10;
    Variable.Size = 4;
    auto Local = HighExpr::makeVar(Variable, Integer);
    auto Function = returning("branch_phi_entry", Local, {Integer});
    auto Return = Function.Body.back();
    Return.Addr = 0x1300;
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1200;
    HighStmt Exit = Jump;
    Exit.GotoTarget = Return.Addr;
    HighStmt Copy;
    Copy.Kind = StmtKind::Assign;
    Copy.IsPhiCopy = true;
    Copy.Addr = Jump.GotoTarget;
    Copy.Dst = Local;
    Copy.Val = HighExpr::makeConst(11, 4);
    HighStmt Branch;
    Branch.Kind = ExplicitElse ? StmtKind::IfElse : StmtKind::If;
    Branch.Addr = Jump.GotoTarget;
    Branch.Cond = parameter(0, Integer);
    Branch.Body = {Copy, Exit};
    Copy.Val = HighExpr::makeConst(23, 4);
    Function.Body = {Jump};
    if (ExplicitElse) {
      Branch.ElseBody = {Copy, Exit};
      Function.Body.push_back(Branch);
    } else {
      Function.Body.push_back(Branch);
      Function.Body.push_back(Copy);
    }
    Function.Body.push_back(Return);
    coalesceBranchEntryStatements(Function);
    compileAndRun(emit({Function}) + R"(
int main(void) {
    if (branch_phi_entry(0) != 23) return 1;
    if (branch_phi_entry(1) != 11) return 2;
    if (branch_phi_entry(-1) != 11) return 3;
    return 0;
}
)");
  }
}

TEST(HighCSourceCalls, ConditionalEntryDoesNotHideOtherNestedInstructions) {
  for (bool LaterCopy : {false, true}) {
    auto Function = returning("ambiguous_branch", HighExpr::makeConst(1, 4));
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1200;
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Addr = Jump.GotoTarget;
    Branch.Cond = HighExpr::makeConst(1, 1);
    HighStmt Inner;
    Inner.Kind = StmtKind::Assign;
    Inner.Addr = Branch.Addr;
    Inner.IsPhiCopy = LaterCopy;
    MedVar Local;
    Local.Kind = MedVar::Temp;
    Local.Id = 10;
    Local.Size = 4;
    Inner.Dst = HighExpr::makeVar(Local, NdType::makeInt(4));
    Inner.Val = HighExpr::makeConst(23, 4);
    if (LaterCopy) {
      HighStmt Separator;
      Separator.Kind = StmtKind::Block;
      Separator.Addr = 0x1210;
      Branch.Body.push_back(Separator);
    }
    Branch.Body.push_back(Inner);
    Function.Body = {Jump, Branch, Function.Body.back()};
    coalesceBranchEntryStatements(Function);
    unsigned Entries = 0;
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      Entries += Statement.Addr == Branch.Addr;
    });
    EXPECT_EQ(Entries, 2U);
  }
}

TEST(HighCSourceCalls,
     NativeForwardPrototypesPreserveFloatingArgumentAndResultBits) {
  std::vector<HighFunc> Functions;
  for (unsigned Width : {4U, 8U}) {
    auto Bits = NdType::makeInt(Width, false);
    auto Float = NdType::makeFloat(Width);
    const std::string Name = "round_trip_" + std::to_string(Width);
    const std::string Helper = "_helper_" + std::to_string(Width);
    Functions.push_back(returning(
        Name, call(native(Helper, Float, {Float}), Bits, {parameter(0, Bits)}),
        {Bits}));
    Functions.push_back(returning(Helper, parameter(0, Float), {Float}));
  }
  const auto Source = emit(Functions);
  EXPECT_EQ(Source.find("extern int helper"), std::string::npos);
  EXPECT_NE(Source.find("float helper_4(float);"), std::string::npos);
  EXPECT_NE(Source.find("double helper_8(double);"), std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  const uint32_t a[] = {0,0x80000000U,1,0x3fa00000U,0x7fc00042U,0x7f800000U};
  const uint64_t b[] = {0,0x8000000000000000ULL,1,0x3ff4000000000000ULL,
                        0x7ff8000000000042ULL,0x7ff0000000000000ULL};
  for (unsigned i=0; i<sizeof(a)/sizeof(a[0]); ++i)
    if (round_trip_4(a[i]) != a[i]) return 1;
  for (unsigned i=0; i<sizeof(b)/sizeof(b[0]); ++i)
    if (round_trip_8(b[i]) != b[i]) return 2;
  return 0;
})");
}

TEST(HighCSourceCalls,
     MessageCallCompilesAndExecutesMixedPointerFloatIntegerSignature) {
  auto U64 = NdType::makeInt(8, false);
  auto I32 = NdType::makeInt(4);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Double = NdType::makeFloat(8);
  auto Hint = native("objc_msgSend", Double, {Pointer, Pointer, Double, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "compute:count:";
  auto Expression = call(Hint, U64,
                         {parameter(0, U64), parameter(1, U64),
                          parameter(2, U64), parameter(3, I32)});
  const auto Source =
      emit({returning("send", Expression, {U64, U64, U64, I32})}, false);
  EXPECT_EQ(Source.find("extern int objc_msgSend"), std::string::npos);
  EXPECT_NE(Source.find("(*)(id, SEL, double, int32_t)"), std::string::npos);
  // A portable dispatch double tests the generated host ABI and exact operands.
  // The separate macOS executable corpus exercises the actual Objective-C
  // runtime.
  compileAndRun(R"(
#include <stdint.h>
typedef void *id;
typedef void *SEL;
static double implementation(id receiver, SEL selector, double value, int32_t count) {
  return (receiver == (id)(uintptr_t)0x1234 && selector == (SEL)(uintptr_t)0x5678)
          ? value + count : -999;
}
static double (*objc_msgSend)(id, SEL, double, int32_t) = implementation;
)" + Source + R"(
int main(void) {
  for (int32_t count=-3; count<4; ++count) {
    double input=1.25, expected=input+count;
    uint64_t bits=__builtin_bit_cast(uint64_t,input);
    if (send(0x1234,0x5678,bits,count) != __builtin_bit_cast(uint64_t,expected)) return 1;
  }
  return 0;
})");
}

TEST(HighCSourceCalls, RuntimeImportsCompileAndPreserveObjectAndVoidEffects) {
  auto U64 = NdType::makeInt(8, false);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Retain = native("objc_retain", Pointer, {Pointer});
  Retain.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Retain.TargetAddress = 0xdeadbeef;
  auto Release = native("objc_release", NdType::makeVoid(), {Pointer});
  Release.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Release.TargetAddress = 0xdeadbef7;
  auto Function = returning("retain_and_release",
                            call(Retain, U64, {parameter(0, U64)}), {U64});
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = call(Release, NdType::makeVoid(), {parameter(0, U64)});
  Function.Body.insert(Function.Body.begin(), Statement);
  const auto Source = emit({Function});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("DEADBEEF"), std::string::npos);
  EXPECT_NE(Source.find("void* objc_retain(void*);"), std::string::npos);
  EXPECT_NE(Source.find("void objc_release(void*);"), std::string::npos);
  compileAndRun(Source + R"(
static void *last_released;
static int retained, released;
void *objc_retain(void *object) { ++retained; return object; }
void objc_release(void *object) { ++released; last_released = object; }
int main(void) {
  const uintptr_t values[] = {0, 0x12345678, UINT64_C(0xfedcba9876543210)};
  for (unsigned i = 0; i < 3; ++i)
    if (retain_and_release(values[i]) != values[i] ||
        (uintptr_t)last_released != values[i]) return 1;
  return retained != 3 || released != 3;
})");
}

TEST(HighCSourceCalls, RuntimeImportsDoNotBindToLiftedVeneersWithTheSameName) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Runtime = native("objc_opt_self", Pointer, {Pointer});
  Runtime.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Runtime.TargetAddress = 0xc772f8;
  auto Veneer = returning("_objc_opt_self",
                          call(Runtime, Pointer, {parameter(0, Pointer)}),
                          {Pointer});
  Veneer.Entry = 0x915d3c;

  const auto Source = emit({Veneer});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern void* objc_opt_self(void*);"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("void* objc_opt_self_2(void* arg0)"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("objc_opt_self((void*)(uintptr_t)("),
            std::string::npos)
      << Source;
  compileAndRun(Source + R"(
void *objc_opt_self(void *object) {
  return (void *)((uintptr_t)object + 7);
}
int main(void) {
  return (uintptr_t)objc_opt_self_2((void *)(uintptr_t)35) != 42;
}
)");
}

TEST(HighCSourceCalls, RuntimeWeakCallsUsePublicObjectStorageTypes) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Storage = NdType::makePtr(Pointer);
  auto Store = native("objc_storeWeak", Pointer, {Storage, Pointer});
  Store.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  auto Load = native("objc_loadWeak", Pointer, {Storage});
  Load.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  const std::vector<HighFunc> Functions{
      returning(
          "weak_store",
          call(Store, Pointer, {parameter(0, Storage), parameter(1, Pointer)}),
          {Storage, Pointer}),
      returning("weak_load", call(Load, Pointer, {parameter(0, Storage)}),
                {Storage})};
  const auto Source = emit(Functions);
  EXPECT_NE(Source.find("#include <objc/runtime.h>"), std::string::npos);
  EXPECT_EQ(Source.find("extern void* objc_storeWeak"), std::string::npos);
  EXPECT_EQ(Source.find("extern void* objc_loadWeak"), std::string::npos);
  compileAndRun(R"(
#include <stdint.h>
typedef struct objc_object *id;
id objc_storeWeak(id *location, id object) { *location = object; return object; }
id objc_loadWeak(id *location) { return *location; }
)" + emit(Functions, false) +
                R"(
int main(void) {
  id slot = 0;
  id value = (id)(uintptr_t)1234;
  if (weak_store((void **)&slot, value) != value || weak_load((void **)&slot) != value) return 1;
  return weak_store((void **)&slot, 0) != 0 || weak_load((void **)&slot) != 0;
}
)");
}

TEST(HighCSourceCalls, RuntimeReferencesUseEscapedNamesAndNoOriginalAddresses) {
  auto U64 = NdType::makeInt(8, false);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  std::vector<HighFunc> Functions;
  const SourceCallTypeHint::Kind Kinds[] = {
      SourceCallTypeHint::Kind::RuntimeSelector,
      SourceCallTypeHint::Kind::RuntimeClass,
      SourceCallTypeHint::Kind::RuntimeMetaclass,
      SourceCallTypeHint::Kind::RuntimeIvarOffset};
  for (size_t I = 0; I < 4; ++I) {
    auto Hint =
        native("quote\"slash\\line\n\?\?/9", I == 3 ? U64 : Pointer, {});
    Hint.CallKind = Kinds[I];
    Hint.OwnerClass = "Owner";
    Hint.TargetAddress = 0xdeadbeef;
    Functions.push_back(
        returning("reference_" + std::to_string(I), call(Hint, U64)));
  }
  const auto Source = emit(Functions, false);
  EXPECT_EQ(Source.find("DEADBEEF"), std::string::npos);
  EXPECT_EQ(Source.find("extern int"), std::string::npos);
  EXPECT_NE(Source.find("\\012\\?\\?/9"), std::string::npos);
  compileAndRun(R"(
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef void *Class;
static const char wanted[]="quote\"slash\\line\n\?\?/9";
static void *sel_registerName(const char *s) { return (void *)(uintptr_t)(strcmp(s,wanted)==0 ? 11 : 0); }
static void *objc_getClass(const char *s) { return (void *)(uintptr_t)(strcmp(s,"Owner")==0 ? 31 : strcmp(s,wanted)==0 ? 12 : 0); }
static void *objc_getMetaClass(const char *s) { return (void *)(uintptr_t)(strcmp(s,wanted)==0 ? 13 : 0); }
static void *class_getInstanceVariable(Class c,const char *s) { return (void *)(uintptr_t)(c==(void *)(uintptr_t)31 && strcmp(s,wanted)==0 ? 32 : 0); }
static ptrdiff_t ivar_getOffset(void *v) { return v==(void *)(uintptr_t)32 ? 24 : -1; }
)" + Source + R"(
int main(void) { return reference_0()!=11 || reference_1()!=12 || reference_2()!=13 || reference_3()!=24; }
)");
}

TEST(HighCSourceCalls, RejectsWrongArgumentCountWidthAndIncompleteBlocks) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("objc_msgSend", I32, {Pointer, Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "step:";
  auto Missing = call(Hint, I32, {HighExpr::makeConst(0, 8)});
  EXPECT_NE(emit({returning("missing", Missing)}, false)
                .find("bad source call: argument count"),
            std::string::npos);
  auto WrongWidth = call(Hint, I32,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          HighExpr::makeConst(1, 8)});
  EXPECT_NE(emit({returning("width", WrongWidth)}, false)
                .find("bad source call: argument carrier"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  EXPECT_NE(emit({returning("block", call(Hint, I32,
                                          {HighExpr::makeConst(0, 8),
                                           HighExpr::makeConst(0, 8),
                                           HighExpr::makeConst(1, 4)}))},
                 false)
                .find("bad source call: block invocation"),
            std::string::npos);
}

TEST(HighCSourceCalls,
     Super2UsesTheCurrentClassRecordAndAnExplicitRuntimeDeclaration) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("objc_msgSendSuper2", I32, {Pointer, Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
  Hint.Selector = "step:";
  auto Expr = call(Hint, I32,
                   {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                    HighExpr::makeConst(1, 4)});
  auto Source = emit({returning("super_send", Expr)});
  EXPECT_NE(Source.find("#include <objc/message.h>"), std::string::npos);
  EXPECT_NE(Source.find("extern void objc_msgSendSuper2(void);"),
            std::string::npos);
  EXPECT_NE(Source.find("(*)(struct objc_super *, SEL, int32_t)"),
            std::string::npos);
  EXPECT_EQ(Source.find("extern int objc_msgSend"), std::string::npos);
}

TEST(HighCSourceCalls, NativeTargetAddressSurvivesProjectionRenaming) {
  auto Type = NdType::makeInt(4);
  auto Hint = native("original_symbol", Type, {Type});
  Hint.TargetAddress = 0x1400;
  auto Caller =
      returning("invoke", call(Hint, Type, {parameter(0, Type)}), {Type});
  Caller.Entry = 0x1300;
  auto Helper = returning("neverd_objc_imp_1400", parameter(0, Type), {Type});
  Helper.Entry = 0x1400;
  const auto Source = emit({Caller, Helper});
  EXPECT_EQ(Source.find("original_symbol"), std::string::npos);
  EXPECT_NE(Source.find("int32_t neverd_objc_imp_1400(int32_t);"),
            std::string::npos);
  compileAndRun(Source + "int main(void) { return invoke(-37) != -37; }\n");
}

TEST(HighCSourceCalls, NativeTargetAddressesDisambiguateRepeatedSymbols) {
  const auto I32 = NdType::makeInt(4);
  auto First = returning("OUTLINED_FUNCTION_0", parameter(0, I32), {I32});
  First.Entry = 0x1400;
  auto Second = returning("OUTLINED_FUNCTION_0", parameter(0, I32), {I32});
  Second.Entry = 0x1500;
  auto Hint = native("OUTLINED_FUNCTION_0", I32, {I32});
  Hint.TargetAddress = Second.Entry;
  auto Caller =
      returning("invoke_second", call(Hint, I32, {parameter(0, I32)}), {I32});
  Caller.Entry = 0x1300;
  const auto Source = emit({Caller, First, Second});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_NE(Source.find("OUTLINED_FUNCTION_0_2((int32_t)(arg0))"),
            std::string::npos)
      << Source;
  compileAndRun(Source +
                "int main(void) { return invoke_second(-37) != -37; }\n");
}

TEST(HighCSourceCalls,
     TypedBlockDispatchExecutesCapturedIntegerAndMixedFloatValues) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto U64 = NdType::makeInt(8, false);
  auto F64 = NdType::makeFloat(8);
  auto Integer = native("", I32, {Pointer, I32});
  Integer.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Integer.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  auto Floating = native("", F64, {Pointer, F64, I32});
  Floating.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Floating.Signature.Origin = SourceFunctionTypeHint::OriginKind::BlockRuntime;
  auto A =
      returning("integer_block",
                call(Integer, I32, {parameter(0, Pointer), parameter(1, I32)}),
                {Pointer, I32});
  auto B = returning(
      "floating_block",
      call(Floating, U64,
           {parameter(0, Pointer), parameter(1, U64), parameter(2, I32)}),
      {Pointer, U64, I32});
  auto Source = emit({A, B});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  compileAndRun(Source + R"(
void *_NSConcreteStackBlock[32], *_NSConcreteGlobalBlock[32];
int main(void) {
  int captured = 17;
  int (^integer)(int) = ^(int value) { return (int)((unsigned)value * 3u + (unsigned)captured); };
  double (^floating)(double,int) = ^(double a,int b) { return a * 2.0 + b; };
  unsigned cases[] = {0,1,0x80,0xff,0x80000001u,0xffffffffu};
  for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
    int x=(int)cases[i];
    if (integer_block((void *)integer,x)!=integer(x)) return 1;
    double a=(double)(int)i - 2.25;
    uint64_t bits=__builtin_bit_cast(uint64_t,a);
    uint64_t actual=floating_block((void *)floating,bits,x);
    if (__builtin_bit_cast(double,actual)!=floating(a,x)) return 2;
  }
  return 0;
}
)");
}

TEST(HighCSourceCalls, BlockReceiverIsEvaluatedExactlyOnce) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("", I32, {Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  auto Next = call(native("next_block", Pointer, {}), Pointer);
  auto Source = emit({returning(
      "invoke_once", call(Hint, I32, {Next, parameter(0, I32)}), {I32})});
  compileAndRun(Source + R"(
void *_NSConcreteGlobalBlock[32];
static unsigned calls;
void *next_block(void) {
  ++calls;
  return (void *)^(int value) { return value + 7; };
}
int main(void) { return invoke_once(5)!=12 || calls!=1; }
)");
}

TEST(HighCSourceCalls,
     BlockSourceAddressesUseActualDefinitionsAndBoundHelpers) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto U64 = NdType::makeInt(8, false);
  auto I32 = NdType::makeInt(4);
  std::vector<HighFunc> Functions;
  auto Address = native("original_untrusted_name", Pointer, {});
  Address.CallKind = SourceCallTypeHint::Kind::NativeAddress;
  Address.TargetAddress = 0xabc;
  Functions.push_back(returning("get_invoke", call(Address, U64)));
  auto Definition =
      returning("neverd_block_invoke_abc", parameter(0, I32), {I32});
  Definition.Entry = 0xabc;
  Functions.push_back(Definition);
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockIsa;
  Address.TargetName = "__NSConcreteStackBlock";
  Functions.push_back(returning("get_isa", call(Address, U64)));
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockDescriptor;
  Address.TargetAddress = 0xdead;
  Functions.push_back(returning("get_descriptor", call(Address, U64)));
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockLiteral;
  Address.TargetAddress = 0xbeef;
  Functions.push_back(returning("get_literal", call(Address, U64)));
  auto Source = emit(Functions);
  EXPECT_EQ(Source.find("original_untrusted_name"), std::string::npos);
  EXPECT_NE(Source.find("int32_t neverd_block_invoke_abc(int32_t);"),
            std::string::npos);
  compileAndRun(Source + R"(
void *_NSConcreteStackBlock[32];
static int descriptor, literal;
uintptr_t neverd_block_descriptor_dead_address(void) { return (uintptr_t)&descriptor; }
uintptr_t neverd_block_literal_beef_address(void) { return (uintptr_t)&literal; }
int main(void) {
  if (((int32_t (*)(int32_t))get_invoke())(-71)!=-71) return 1;
  if (get_isa()!=(uintptr_t)_NSConcreteStackBlock) return 2;
  return get_descriptor()!=(uintptr_t)&descriptor || get_literal()!=(uintptr_t)&literal;
}
)");
}

TEST(HighCSourceCalls,
     UnknownOrOperandBearingSourceAddressesRemainExplicitFailures) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Hint = native("_NSConcreteStackBlock_extra", Pointer, {});
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeBlockIsa;
  EXPECT_NE(emit({returning("bad_isa", call(Hint, Pointer))})
                .find("bad source call: unknown concrete"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::NativeAddress;
  Hint.TargetAddress = 0x1000;
  EXPECT_NE(emit({returning("missing", call(Hint, Pointer))})
                .find("bad source call: native address"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeBlockDescriptor;
  EXPECT_NE(emit({returning("args",
                            call(Hint, Pointer, {HighExpr::makeConst(0, 8)}))})
                .find("bad source call: invalid source address"),
            std::string::npos);
}

TEST(HighCSourceCalls,
     SwiftTypeMetadataAddressesAcceptDescriptorFreePrintableReferences) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Hint = native("", Pointer, {});
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress;
  Hint.TargetAddress = 0x1000;
  Hint.SwiftTypeMetadata = SourceCallTypeHint::SwiftTypeMetadataAddress{
      0x1000, 0x1010, 0x1020, 0, "", "ScPSg"};
  const auto Source = emit({returning("metadata_cache", call(Hint, Pointer))});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  EXPECT_NE(Source.find("neverd_swift_type_metadata_1000_1010_cache_address()"),
            std::string::npos);

  Hint.SwiftTypeMetadata->DescriptorSlot = 0x1030;
  EXPECT_NE(emit({returning("bad_descriptor", call(Hint, Pointer))})
                .find("bad source call: Swift type metadata"),
            std::string::npos);
}
} // namespace

TEST(HighCSourceCalls, VariadicMessageCompilesAndPreservesPromotedArguments) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto I64 = NdType::makeInt(8);
  auto Double = NdType::makeFloat(8);
  auto Hint = native("objc_msgSend", I64,
                     {Pointer, Pointer, Pointer, I32, Double, I64, Pointer});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "format:";
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
  Hint.Format = SourceCallTypeHint::FormatArguments{3, 2, 0x2000};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinVariadicSourceABI(Hint.Signature, 3, Arch::X64, Error));
  auto Expression =
      call(Hint, I64,
           {parameter(0, Pointer), parameter(1, Pointer), parameter(2, Pointer),
            parameter(3, I32), parameter(4, Double), parameter(5, I64),
            parameter(6, Pointer)});
  const auto Source =
      emit({returning("send_format", Expression,
                      {Pointer, Pointer, Pointer, I32, Double, I64, Pointer})},
           false);
  EXPECT_NE(Source.find("(*)(id, SEL, void*, ...)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(R"(
#include <stdint.h>
#include <stdarg.h>
typedef void *id;
typedef void *SEL;
static int64_t implementation(id receiver, SEL selector, void *format, ...) {
  va_list args; va_start(args,format);
  int count=va_arg(args,int);
  double value=va_arg(args,double);
  int64_t wide=va_arg(args,int64_t);
  void *object=va_arg(args,void*);
  va_end(args);
  if (receiver!=(void*)1 || selector!=(void*)2 || format!=(void*)3 || object!=(void*)4)
    return -999;
  return wide + count + (int64_t)(value*4);
}
static int64_t (*objc_msgSend)(id,SEL,void*,...)=implementation;
)" + Source + R"(
int main(void) {
  for(int n=-32;n<=32;++n) for(int k=-32;k<=32;++k) {
    int64_t wide=INT64_C(0x123456789abc), expected=wide+n+k;
    if(send_format((void*)1,(void*)2,(void*)3,n,k/4.0,wide,(void*)4)!=expected) return 1;
  }
  return 0;
})");
}

TEST(HighCSourceCalls, DynamicFormatsPermitEmptyOrPointerVariadicTails) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Hint = native("objc_msgSend", Pointer, {Pointer, Pointer, Pointer});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "localizedStringWithFormat:";
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
  Hint.Format = SourceCallTypeHint::FormatArguments{
      3, 2, 0, SourceCallTypeHint::FormatSyntax::NSString, {}, true};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinVariadicSourceABI(Hint.Signature, 3, Arch::X64, Error));
  auto Expression = call(
      Hint, Pointer,
      {parameter(0, Pointer), parameter(1, Pointer), parameter(2, Pointer)});
  const auto Source = emit({returning("send_dynamic_format", Expression,
                                      {Pointer, Pointer, Pointer})},
                           false);
  EXPECT_NE(Source.find("(*)(id, SEL, void*, ...)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;

  auto PointerTail = Hint;
  PointerTail.Format = SourceCallTypeHint::FormatArguments{
      3, 2, 0, SourceCallTypeHint::FormatSyntax::NSString, {}, false, true};
  PointerTail.Signature.Parameters.push_back({"first", Pointer});
  PointerTail.Signature.Parameters.push_back({"second", Pointer});
  ASSERT_TRUE(assignDarwinVariadicSourceABI(PointerTail.Signature, 3,
                                             Arch::X64, Error));
  const auto PointerTailSource = emit(
      {returning("send_dynamic_pointer_format",
                 call(PointerTail, Pointer,
                      {parameter(0, Pointer), parameter(1, Pointer),
                       parameter(2, Pointer), parameter(3, Pointer),
                       parameter(4, Pointer)}),
                 {Pointer, Pointer, Pointer, Pointer, Pointer})},
      false);
  EXPECT_NE(PointerTailSource.find("(*)(id, SEL, void*, ...)"),
            std::string::npos)
      << PointerTailSource;
  EXPECT_EQ(PointerTailSource.find("bad source call"), std::string::npos)
      << PointerTailSource;

  auto ConflictingDynamic = PointerTail;
  ConflictingDynamic.Format->DynamicWithoutArguments = true;
  EXPECT_NE(emit({returning(
                      "bad_conflicting_dynamic_format",
                      call(ConflictingDynamic, Pointer,
                           {parameter(0, Pointer), parameter(1, Pointer),
                            parameter(2, Pointer), parameter(3, Pointer),
                            parameter(4, Pointer)}),
                      {Pointer, Pointer, Pointer, Pointer, Pointer})},
                 false)
                .find("bad source call: invalid variadic source declaration"),
            std::string::npos);

  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Bad = Hint;
    if (Mutation == 0)
      Bad.Format->FormatAddress = 0x2000;
    if (Mutation == 1)
      Bad.Format->AlternativeFormatAddresses = {0x2020};
    if (Mutation == 2)
      Bad.Format->FixedCount = 2;
    if (Mutation == 3)
      Bad.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    EXPECT_NE(
        emit({returning("bad_dynamic_format_" + std::to_string(Mutation),
                        call(Bad, Pointer,
                             {parameter(0, Pointer), parameter(1, Pointer),
                              parameter(2, Pointer)}),
                        {Pointer, Pointer, Pointer})},
             false)
            .find("bad source call: invalid variadic source declaration"),
        std::string::npos)
        << Mutation;
  }
}

TEST(HighCSourceCalls, VariadicCDeclarationsMergeOnlyTheirFixedPrefixes) {
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto I32 = NdType::makeInt(4);
  const auto Double = NdType::makeFloat(8);
  auto First = native("format_capture", I32, {Pointer, I32, Double});
  First.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  First.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  First.Format = SourceCallTypeHint::FormatArguments{1, 0, 0x2000};
  auto Second = First;
  Second.Signature.Parameters.resize(1);
  std::string Error;
  ASSERT_TRUE(
      assignDarwinVariadicSourceABI(First.Signature, 1, Arch::X64, Error));
  ASSERT_TRUE(
      assignDarwinVariadicSourceABI(Second.Signature, 1, Arch::X64, Error));
  auto With = returning(
      "with_arguments",
      call(First, I32,
           {parameter(0, Pointer), parameter(1, I32), parameter(2, Double)}),
      {Pointer, I32, Double});
  auto Empty = returning("without_arguments",
                         call(Second, I32, {parameter(0, Pointer)}), {Pointer});
  const auto Source = emit({With, Empty});
  EXPECT_NE(Source.find("neverd_darwin_format_capture(void*, ...)"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(Source + R"(
#include <stdarg.h>
int capture(void*,...) __asm__("_format_capture");
int capture(void *format,...) {
  if (!format) return 77;
  va_list args; va_start(args,format);
  int number=va_arg(args,int);double value=va_arg(args,double);va_end(args);
  return number+(int)(value*4);
}
int main(void) {
  if (without_arguments(0)!=77) return 1;
  for(int i=-32;i<=32;++i)for(int j=-32;j<=32;++j)
    if(with_arguments((void*)1,i,j/4.0)!=i+j) return 2;
  return 0;
})");
  auto Wrong = Second;
  Wrong.Format.reset();
  auto Fixed =
      returning("fixed", call(Wrong, I32, {parameter(0, Pointer)}), {Pointer});
  EXPECT_NE(emit({With, Fixed}).find("conflicting native declarations"),
            std::string::npos);
}

TEST(HighCSourceCalls, DarwinWeakImportsRemainOptionalInDeclarations) {
  const auto U8 = NdType::makeInt(1, false);
  const auto U32 = NdType::makeInt(4, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Hint = native("_availability_version_check", U8, {U32, Pointer});
  Hint.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  Hint.WeakImport = true;
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Hint.Signature, Arch::X64, Error));
  auto Function =
      returning("check_version",
                call(Hint, U8, {parameter(0, U32), parameter(1, Pointer)}),
                {U32, Pointer});
  const auto Source = emit({Function});
  EXPECT_NE(Source.find("extern __attribute__((weak_import)) uint8_t "),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("__asm__(\"__availability_version_check\")"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;

  auto Forged = Hint;
  Forged.CallKind = SourceCallTypeHint::Kind::Native;
  auto Bad =
      returning("forged_weak",
                call(Forged, U8, {parameter(0, U32), parameter(1, Pointer)}),
                {U32, Pointer});
  EXPECT_NE(
      emit({Bad}, false).find("weak import belongs to another binding kind"),
      std::string::npos);
}

TEST(HighCSourceCalls, SwiftConventionSurvivesDefinitionsAndRejectsConflicts) {
  const auto Word = NdType::makeInt(8, false);
  auto Hint = native("swift_identity", Word, {Word});
  std::string Error;
  ASSERT_TRUE(assignDarwinSwiftSourceABI(Hint.Signature, Arch::X64, Error));
  auto Identity = returning("swift_identity", parameter(0, Word), {Word});
  Identity.SourceTypeHint = Hint.Signature;
  Identity.Entry = 0x1234;
  auto Forward = returning("forward_identity",
                           call(Hint, Word, {parameter(0, Word)}), {Word});
  for (const auto &Functions :
       {std::vector{Forward, Identity}, std::vector{Identity, Forward}}) {
    const auto Source = emit(Functions);
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    EXPECT_NE(Source.find("__attribute__((swiftcall)) uint64_t swift_identity"),
              std::string::npos)
        << Source;
    compileAndRun(Source + R"(
int main(void) {
  for (uint64_t i=0; i<4096; ++i) {
    uint64_t value=UINT64_C(0xfedcba9876543210)^i;
    if (forward_identity(value)!=value) return 1;
  }
  return 0;
})");
  }
  auto Ordinary = Hint;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Ordinary.Signature, Arch::X64, Error));
  auto Wrong = returning("wrong_identity",
                         call(Ordinary, Word, {parameter(0, Word)}), {Word});
  for (const auto &Functions :
       {std::vector{Forward, Wrong}, std::vector{Wrong, Forward},
        std::vector{Identity, Wrong}})
    EXPECT_NE(emit(Functions).find("conflicting native declarations"),
              std::string::npos);

  auto Address =
      native("swift_identity", NdType::makePtr(NdType::makeVoid()), {});
  Address.CallKind = SourceCallTypeHint::Kind::NativeAddress;
  Address.TargetAddress = Identity.Entry;
  auto Get = returning("callback", call(Address, Address.Signature.ReturnType));
  EXPECT_NE(emit({Identity, Get})
                .find("unsupported native callback calling convention"),
            std::string::npos);
}

TEST(HighCSourceCalls, DeadPhiCleanupPreservesConditionalEntryAndCallEffects) {
  for (bool ExplicitElse : {false, true}) {
    const auto Integer = NdType::makeInt(4);
    auto Function =
        returning("dead_phi_entry", HighExpr::makeConst(7, 4), {Integer});
    Function.SourceTypeHint =
        native("dead_phi_entry", Integer, {Integer}).Signature;
    auto Return = Function.Body.back();
    Return.Addr = 0x1300;
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1200;
    HighStmt Exit = Jump;
    Exit.GotoTarget = Return.Addr;
    MedVar Dead;
    Dead.Kind = MedVar::Temp;
    Dead.Id = 10;
    Dead.Size = 4;
    HighStmt Copy;
    Copy.Kind = StmtKind::Assign;
    Copy.IsPhiCopy = true;
    Copy.Addr = Jump.GotoTarget;
    Copy.Dst = HighExpr::makeVar(Dead, Integer);
    Copy.Val = HighExpr::makeConst(42, 4);
    auto Observe = [&](va_t Address, unsigned Value) {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.Addr = Address;
      S.CallExpr = call(native("record_branch", NdType::makeVoid(), {Integer}),
                        NdType::makeVoid(), {HighExpr::makeConst(Value, 4)});
      return S;
    };
    HighStmt Branch;
    Branch.Kind = ExplicitElse ? StmtKind::IfElse : StmtKind::If;
    Branch.Addr = Jump.GotoTarget;
    Branch.Cond = parameter(0, Integer);
    Branch.Body = {Copy, Observe(0x1204, 11), Exit};
    if (ExplicitElse)
      Branch.ElseBody = {Copy, Observe(0x1208, 23), Exit};
    Function.Body = {Jump, Branch, Return};
    eliminateUnusedValues(Function.Body);
    coalesceBranchEntryStatements(Function);
    compileAndRun(emit({Function}) + "\n#define HAS_ELSE " +
                  std::to_string(ExplicitElse) + R"(
static unsigned calls;
static int last;
void record_branch(int value) { ++calls; last = value; }
int main(void) {
    for (int value = -32; value <= 32; ++value) {
        unsigned before = calls;
        if (dead_phi_entry(value) != 7) return 1;
        unsigned expected_calls = HAS_ELSE || value != 0;
        if (calls - before != expected_calls) return 2;
        if (expected_calls && last != (value ? 11 : 23)) return 3;
    }
    return 0;
}
)");
  }
}

TEST(HighCSourceCalls, DeadPhiCleanupKeepsUnprovenNestedEntriesAmbiguous) {
  for (bool Separated : {false, true}) {
    auto Function = returning("unproven_dead_entry", HighExpr::makeConst(7, 4),
                              {NdType::makeInt(4)});
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1200;
    MedVar Dead;
    Dead.Kind = MedVar::Temp;
    Dead.Id = 10;
    Dead.Size = 4;
    HighStmt Copy;
    Copy.Kind = StmtKind::Assign;
    Copy.IsPhiCopy = Separated;
    Copy.Addr = Jump.GotoTarget;
    Copy.Dst = HighExpr::makeVar(Dead);
    Copy.Val = HighExpr::makeConst(42, 4);
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Addr = Jump.GotoTarget;
    Branch.Cond = parameter(0, NdType::makeInt(4));
    if (Separated) {
      HighStmt Separator;
      Separator.Kind = StmtKind::Block;
      Separator.Addr = 0x1210;
      Branch.Body.push_back(Separator);
    }
    Branch.Body.push_back(Copy);
    Function.Body = {Jump, Branch, Function.Body.back()};
    eliminateUnusedValues(Function.Body);
    coalesceBranchEntryStatements(Function);
    unsigned Entries = 0;
    walkStmts(Function.Body,
              [&](const HighStmt &S) { Entries += S.Addr == 0x1200; });
    EXPECT_EQ(Entries, 2u);
  }
}

TEST(HighCSourceCalls, PartialIntegerCarriersPreserveExactMemoryWidths) {
  for (unsigned Bytes : {3U, 5U, 6U, 7U}) {
    const auto Integer = NdType::makeInt(8, false);
    const auto Pointer = NdType::makePtr(NdType::makeInt(1, false));
    const auto Partial = NdType::makeInt(Bytes, false);
    auto Truncated = std::make_shared<HighExpr>();
    Truncated->Kind = ExprKind::Cast;
    Truncated->Type = Truncated->CastTo = Partial;
    Truncated->Operands = {parameter(1, Integer)};
    auto Loaded = HighExpr::makeLoad(parameter(0, Pointer), Partial);
    auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Loaded);
    Extended->Type = Integer;
    auto Function = returning("partial_memory", Extended, {Pointer, Integer});
    Function.SourceTypeHint =
        native("partial_memory", Integer, {Pointer, Integer}).Signature;
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = parameter(0, Pointer);
    Store.StoreVal = Truncated;
    Function.Body.insert(Function.Body.begin(), Store);
    auto SignedLoad =
        HighExpr::makeLoad(parameter(0, Pointer), NdType::makeInt(Bytes));
    auto SignedExtended = HighExpr::makeUnary(NdOp::INT_SEXT, SignedLoad);
    SignedExtended->Type = NdType::makeInt(8);
    auto Signed = returning("partial_signed", SignedExtended, {Pointer});
    Signed.SourceTypeHint =
        native("partial_signed", NdType::makeInt(8), {Pointer}).Signature;
    auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, Loaded,
                                   HighExpr::makeConst(1, Bytes));
    Sum->Type = Partial;
    auto SumExtended = HighExpr::makeUnary(NdOp::INT_ZEXT, Sum);
    SumExtended->Type = Integer;
    auto Add = returning("partial_increment", SumExtended, {Pointer});
    Add.SourceTypeHint =
        native("partial_increment", Integer, {Pointer}).Signature;
    std::vector<HighFunc> Functions{Function, Signed, Add};
    for (const auto &[Name, Op] :
         {std::pair{"partial_signed_add", NdOp::INT_ADD},
          std::pair{"partial_signed_sub", NdOp::INT_SUB},
          std::pair{"partial_signed_mul", NdOp::INT_MULT}}) {
      auto Value =
          HighExpr::makeBinop(Op, SignedLoad, HighExpr::makeConst(37, Bytes));
      Value->Type = NdType::makeInt(Bytes);
      auto Bits = HighExpr::makeUnary(NdOp::INT_ZEXT, Value);
      Bits->Type = Integer;
      auto Arithmetic = returning(Name, Bits, {Pointer});
      Arithmetic.SourceTypeHint = native(Name, Integer, {Pointer}).Signature;
      Functions.push_back(std::move(Arithmetic));
    }
    compileAndRun(emit(Functions) + "\n#define BYTES " + std::to_string(Bytes) +
                  R"(
int main(void) {
    uint64_t mask = (UINT64_C(1) << (BYTES * 8)) - 1;
    uint64_t state = UINT64_C(0x1be927fa8046d5c3);
    for (unsigned i = 0; i < 4096; ++i) {
        state = state * UINT64_C(6364136223846793005) + 1;
        uint64_t input = i == 0 ? mask : i == 1 ? 0 : state;
        unsigned char memory[16];
        for (unsigned j = 0; j < sizeof(memory); ++j) memory[j] = 0xa5;
        if (partial_memory(memory + 1, input) != (input & mask)) return 1;
        for (unsigned j = 0; j < BYTES; ++j)
            if (memory[j + 1] != ((input >> (j * 8)) & 255)) return 2;
        if (memory[0] != 0xa5) return 3;
        for (unsigned j = BYTES + 1; j < sizeof(memory); ++j)
            if (memory[j] != 0xa5) return 4;
        uint64_t bits = input & mask;
        int64_t expected = (bits & (UINT64_C(1) << (BYTES * 8 - 1)))
                            ? -(int64_t)((mask + 1) - bits) : (int64_t)bits;
        if (partial_signed(memory + 1) != expected) return 5;
        if (partial_increment(memory + 1) != ((bits + 1) & mask)) return 6;
        if (partial_signed_add(memory + 1) != ((bits + 37) & mask)) return 7;
        if (partial_signed_sub(memory + 1) != ((bits - 37) & mask)) return 8;
        if (partial_signed_mul(memory + 1) != ((bits * 37) & mask)) return 9;
    }
    return 0;
}
)");
  }
}

TEST(HighCSourceCalls, PointerValuesStoredThroughIntegerLoadsUsePointerBits) {
  const auto I64 = NdType::makeInt(8);
  const auto I64Pointer = NdType::makePtr(I64);
  const auto BytePointer = NdType::makePtr(NdType::makeInt(1, false));
  HighFunc Function;
  Function.Name = "store_pointer_bits";
  Function.ReturnType = NdType::makeVoid();
  Function.Params = {{"destination", I64Pointer}, {"value", BytePointer}};
  Function.SourceTypeHint =
      native(Function.Name, Function.ReturnType, {I64Pointer, BytePointer})
          .Signature;
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeLoad(parameter(0, I64Pointer), I64);
  Assign.Val = parameter(1, BytePointer);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Function.Body = {Assign, Return};

  const auto Source = emit({Function});
  EXPECT_NE(Source.find("(int64_t)(uintptr_t)((uintptr_t)value)"),
            std::string::npos)
      << Source;
  compileAndRun(Source + R"(
int main(void) {
    int object = 0;
    int64_t bits = 0;
    store_pointer_bits(&bits, (uint8_t *)&object);
    return (uintptr_t)bits == (uintptr_t)&object ? 0 : 1;
}
)");
}

TEST(HighCSourceCalls, PointerValuesAssignedToIntegerTempsUsePointerBits) {
  const auto I64 = NdType::makeInt(8);
  const auto BytePointer = NdType::makePtr(NdType::makeInt(1, false));
  MedVar Bits;
  Bits.Kind = MedVar::Temp;
  Bits.Id = 17;
  Bits.Size = 8;
  Bits.TheArch = Arch::X64;
  HighFunc Function;
  Function.Name = "pointer_temp_bits";
  Function.ReturnType = I64;
  Function.Params = {{"value", BytePointer}};
  Function.SourceTypeHint =
      native(Function.Name, I64, {BytePointer}).Signature;
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(Bits, I64);
  Assign.Val = parameter(0, BytePointer);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeVar(Bits, I64);
  Function.Body = {Assign, Return};

  const auto Source = emit({Function});
  EXPECT_NE(Source.find("t17 = (int64_t)(uintptr_t)((uintptr_t)value);"),
            std::string::npos)
      << Source;
  compileAndRun(Source + R"(
int main(void) {
    int object = 0;
    return (uintptr_t)pointer_temp_bits((uint8_t *)&object) ==
                   (uintptr_t)&object
               ? 0
               : 1;
}
)");
}

TEST(HighCSourceCalls, PartialIntegerCarriersPreserveWideValuesAndShiftBounds) {
  for (unsigned Bytes : {3U, 5U, 6U, 7U, 9U, 10U, 11U, 12U, 13U, 14U, 15U}) {
    const auto Pointer = NdType::makePtr(NdType::makeInt(1, false));
    const auto CountType = NdType::makeInt(8, false);
    const auto Partial = NdType::makeInt(Bytes, false);
    std::vector<HighFunc> Functions;
    for (const auto &[Name, Op] :
         {std::pair{"partial_left", NdOp::INT_LEFT},
          std::pair{"partial_right", NdOp::INT_RIGHT},
          std::pair{"partial_arithmetic", NdOp::INT_ASHR}}) {
      auto Loaded = HighExpr::makeLoad(parameter(0, Pointer), Partial);
      auto Shifted = HighExpr::makeBinop(Op, Loaded, parameter(2, CountType));
      Shifted->Type = Partial;
      auto Function = returning(Name, HighExpr::makeConst(0, 4),
                                {Pointer, Pointer, CountType});
      Function.SourceTypeHint =
          native(Name, NdType::makeInt(4), {Pointer, Pointer, CountType})
              .Signature;
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.StoreAddr = parameter(1, Pointer);
      Store.StoreVal = Shifted;
      Function.Body.insert(Function.Body.begin(), Store);
      Functions.push_back(std::move(Function));
    }
    compileAndRun(emit(Functions) + "\n#define BYTES " + std::to_string(Bytes) +
                  R"(
int main(void) {
    unsigned char input[BYTES], output[BYTES + 2];
    uint64_t state = UINT64_C(0x9813ae24abf20c85);
    for (unsigned round = 0; round < 8; ++round) {
        for (unsigned i = 0; i < BYTES; ++i) {
            state = state * UINT64_C(6364136223846793005) + 1;
            input[i] = state >> 56;
        }
        for (unsigned count = 0; count <= BYTES * 8 + 1; ++count) {
            for (unsigned op = 0; op < 3; ++op) {
                memset(output, 0xa5, sizeof(output));
                if (op == 0) partial_left(input, output + 1, count);
                if (op == 1) partial_right(input, output + 1, count);
                if (op == 2) partial_arithmetic(input, output + 1, count);
                if (output[0] != 0xa5 || output[BYTES + 1] != 0xa5) return 1;
                for (unsigned bit = 0; bit < BYTES * 8; ++bit) {
                    int source = op == 0 ? (int)bit - (int)count : bit + count;
                    unsigned expected = source >= 0 && source < BYTES * 8
                      ? (input[source / 8] >> (source % 8)) & 1
                      : op == 2 && (input[BYTES - 1] & 128) ? 1 : 0;
                    if (((output[1 + bit / 8] >> (bit % 8)) & 1) != expected)
                        return 2;
                }
            }
        }
    }
    return 0;
}
)");
  }
}

TEST(HighCSourceCalls, PartialIntegerAtomicsRejectWidenedMemoryAccess) {
  EXPECT_THROW(typeToC(NdType::makeInt(17)), std::invalid_argument);
  const auto Partial = NdType::makeInt(3, false);
  const auto Pointer = NdType::makePtr(NdType::makeInt(1, false));
  auto Load = HighExpr::makeLoad(parameter(0, Pointer), Partial);
  Load->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  auto Function = returning("partial_atomic_load", Load, {Pointer});
  EXPECT_DEATH(emit({Function}),
               "atomic access requires a supported machine width");
  for (NdOp Op : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG, NdOp::ATOMIC_CMPXCHG}) {
    auto Atomic = HighExpr::makeBinop(Op, parameter(0, Pointer),
                                      HighExpr::makeConst(1, 3));
    Atomic->Type = Partial;
    Atomic->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    if (Op == NdOp::ATOMIC_CMPXCHG)
      Atomic->Operands.push_back(HighExpr::makeConst(2, 3));
    Function = returning("partial_atomic_update", Atomic, {Pointer});
    EXPECT_DEATH(emit({Function}),
                 "atomic access requires a supported machine width");
  }
  Function =
      returning("partial_atomic_store", HighExpr::makeConst(0, 4), {Pointer});
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = parameter(0, Pointer);
  Store.StoreVal = HighExpr::makeConst(1, 3);
  Store.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  Function.Body.insert(Function.Body.begin(), Store);
  EXPECT_DEATH(emit({Function}),
               "atomic access requires a supported machine width");
}

TEST(HighCSourceCalls, AssignedVersionZeroRegisterKeepsReturningCallResult) {
  const auto I64 = NdType::makeInt(8, false);
  for (bool Typed : {false, true}) {
    HighFunc F;
    F.Name = "read_value";
    F.ReturnType = I64;
    if (Typed)
      F.SourceTypeHint = native(F.Name, I64, {}).Signature;
    MedVar Value;
    Value.Kind = MedVar::Reg;
    Value.Id = 0;
    Value.SSAVer = 0;
    Value.Size = 8;
    Value.TheArch = Arch::X64;
    auto Hint = native("value_provider", I64, {});
    Hint.TargetAddress = 0x1234;
    HighStmt Assign;
    Assign.Kind = StmtKind::Assign;
    Assign.Dst = HighExpr::makeVar(Value, I64);
    Assign.Val = call(Hint, I64);
    HighStmt Ret;
    Ret.Kind = StmtKind::Return;
    Ret.RetVal = HighExpr::makeVar(Value, I64);
    F.Body = {Assign, Ret};
    compileAndRun(emit({F}) + R"(
uint64_t value_provider(void) { return UINT64_C(0x123456789abcdef0); }
int main(void) { return read_value() != UINT64_C(0x123456789abcdef0); }
)");
  }
}

TEST(HighCSourceCalls, DynamicInteger64FormatsKeepTrueVariadicCalls) {
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Signed = NdType::makeInt(8, true);
  const auto Unsigned = NdType::makeInt(8, false);
  auto Hint = native("objc_msgSend", Pointer,
                     {Pointer, Pointer, Pointer, Signed, Unsigned});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "localizedStringWithFormat:";
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
  Hint.Format = SourceCallTypeHint::FormatArguments{
      3,  2,     0,     SourceCallTypeHint::FormatSyntax::NSString,
      {}, false, false, true};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinVariadicSourceABI(Hint.Signature, 3, Arch::AArch64, Error));
  const auto Render = [&](const SourceCallTypeHint &Binding) {
    std::vector<TypeRef> Types;
    std::vector<ExprPtr> Arguments;
    for (size_t I = 0; I < Binding.Signature.Parameters.size(); ++I) {
      const auto &Type = Binding.Signature.Parameters[I].Type;
      Types.push_back(Type);
      auto V = parameter(unsigned(I), Type);
      V->Var.TheArch = Arch::AArch64;
      Arguments.push_back(V);
    }
    auto F = returning("send_dynamic_integer_format",
                       call(Binding, Pointer, Arguments), Types);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.EmitIncludes = false;
    Options.EmitComments = false;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    EXPECT_TRUE(HighCEmitter().emit({F}, OS, Options));
    return Source;
  };
  const auto Source = Render(Hint);
  EXPECT_NE(Source.find("(*)(id, SEL, void*, ...)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(R"(
#include <stdint.h>
#include <stdarg.h>
typedef void *id;
typedef void *SEL;
static int64_t observed_signed;
static uint64_t observed_unsigned;
static void *implementation(id receiver, SEL selector, void *format, ...) {
  va_list args;
  va_start(args, format);
  observed_signed = va_arg(args, int64_t);
  observed_unsigned = va_arg(args, uint64_t);
  va_end(args);
  return receiver == (void*)1 && selector == (void*)2 ? format : (void*)0;
}
static void *(*objc_msgSend)(id, SEL, void*, ...) = implementation;
)" + Source + R"(
int main(void) {
  const int64_t signed_values[] = { INT64_MIN, -1, 0, 1, INT64_MAX };
  const uint64_t unsigned_values[] = { 0, 1, UINT64_C(0x8000000000000000), UINT64_MAX };
  for (unsigned i = 0; i < 5; ++i) for (unsigned j = 0; j < 4; ++j) {
    void *format = (void*)(uintptr_t)(3+i+j);
    if (send_dynamic_integer_format((void*)1, (void*)2, format,
                                   signed_values[i], unsigned_values[j]) != format)
      return 1;
    if (observed_signed != signed_values[i] || observed_unsigned != unsigned_values[j])
      return 2;
  }
  return 0;
})");
  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Bad = Hint;
    if (Mutation == 0)
      Bad.Format->DynamicPointerArguments = true;
    if (Mutation == 1)
      Bad.Format->DynamicWithoutArguments = true;
    if (Mutation == 2)
      Bad.Format->FormatAddress = 0x1234;
    if (Mutation == 3)
      Bad.Format->AlternativeFormatAddresses = {0x1234};
    if (Mutation == 4)
      Bad.Signature.Parameters[3].Type = NdType::makeFloat(8);
    if (Mutation == 5)
      Bad.Signature.Parameters[3].Type = Pointer;
    if (Mutation == 6) {
      Bad.Signature.Parameters.resize(3);
      ASSERT_TRUE(assignDarwinVariadicSourceABI(Bad.Signature, 3, Arch::AArch64,
                                                Error));
    }
    if (Mutation == 7)
      ASSERT_TRUE(
          assignDarwinVariadicSourceABI(Bad.Signature, 3, Arch::X64, Error));
    if (Mutation == 8)
      Bad.Signature.HasExplicitABI = false;
    if (Mutation == 9) {
      Bad.Signature.Parameters[3].Location.EntryStackOffset = 8;
      Bad.Signature.Parameters[4].Location.EntryStackOffset = 16;
    }
    if (Mutation == 10) {
      ASSERT_TRUE(assignDarwinFixedSourceABI(Bad.Signature, Arch::AArch64, Error));
      ASSERT_TRUE(validateSourceABI(Bad.Signature, Error));
    }
    EXPECT_NE(Render(Bad).find("bad source call"), std::string::npos);
  }
}
