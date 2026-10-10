#include "../../../lib/loader/Swift/SwiftBooleanSourceBinding.h"
#include "../../../lib/loader/Swift/SwiftMangledClassMethodABI.h"
#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "CFunctionParameterCallFixture.h"
#include "RuntimeFunctionAddressFixture.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "llvm/ADT/ArrayRef.h"
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

std::string emit(const std::vector<HighFunc> &Functions, bool Includes = true,
                 Arch Architecture = Arch::X64,
                 CEmitterOptions::ScalarPointerSpelling Spelling =
                     CEmitterOptions::ScalarPointerSpelling::StandardTypes,
                 DebugContext *Debug = nullptr) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  CEmitterOptions Options;
  Options.TheArch = Architecture;
  Options.ScalarPointers = Spelling;
  // Source calls are Apple-platform calls: their symbols carry Mach-O's
  // decoration underscore.
  Options.Format = BinaryFormat::MachO;
  Options.EmitIncludes = Includes;
  Options.EmitComments = false;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options, Debug));
  return Result;
}

using source_call_execution_test::compileAndRun;

MedFunc savedFloatingRecord(unsigned Calls, bool ReplaceFirst = false,
                            bool ReadUpper = false) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto Pair =
      NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
  const auto Record = NdType::makeStruct({Pair, Pair});
  SourceFunctionTypeHint Entry, Callee;
  Entry.ReturnType = Record;
  Entry.Parameters = {{"value", Record}};
  Callee.ReturnType = NdType::makeVoid();
  std::string Error;
  EXPECT_TRUE(assignDarwinFixedSourceABI(Entry, Arch::AArch64, Error)) << Error;
  EXPECT_TRUE(assignDarwinFixedSourceABI(Callee, Arch::AArch64, Error))
      << Error;
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "saved_record";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  auto Append = [&](NdOp Code, NdVar Out, std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Addr = Low.Entry + Block.Ops.size() * 4;
    Op.Output = Out;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Block.Ops.push_back(Op);
  };
  auto Saved = [&](unsigned I) {
    return NdVar::reg(TRI.VecRegBase + (8 + I) * TRI.VecRegStride, 16);
  };
  for (unsigned I = 0; I < 4; ++I)
    Append(NdOp::COPY, Saved(I), {NdVar::reg(TRI.FPParamRegs[I], 16)});
  for (unsigned I = 0; I < Calls; ++I) {
    Append(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)});
    if (ReplaceFirst && I == Calls / 2)
      Append(NdOp::COPY, Saved(0), {Saved(1)});
  }
  for (unsigned I = 0; I < 4; ++I) {
    if (ReadUpper && I == 0)
      Append(NdOp::SUBBYTES, NdVar::reg(TRI.FPReturnReg, 8),
             {Saved(I), NdVar::cst(8, 4)});
    else
      Append(NdOp::COPY, NdVar::reg(TRI.FPParamRegs[I], 16), {Saved(I)});
  }
  Append(NdOp::RETURN, {}, {});
  Block.EndAddr = Low.Entry + Block.Ops.size() * 4;
  Low.Blocks.push_back(Block);
  const std::map<va_t, SourceFunctionTypeHint> Hints{{Low.Entry, Entry},
                                                     {0x2000, Callee}};
  LowToMedConverter Converter;
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceCalleeTypeHints(&Hints);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  Med.SourceTypeHint = Entry;
  recoverCallAbi(Med, Arch::AArch64, {{0x2000, "observe_call"}});
  inferMedTypes(Med, Arch::AArch64);
  EXPECT_TRUE(verifyMedFunc(Med, "saved-floating-record"));
  return Med;
}

HighFunc lowerSavedFloatingRecord(const MedFunc &Med) {
  const std::map<va_t, std::string> Names{{0x2000, "observe_call"}};
  MedToHighConverter Converter;
  Converter.setFuncNames(&Names);
  return Converter.convert(Med, Arch::AArch64);
}

TEST(HighCSourceCalls, UnnamedFixedParametersShareDeclarationAndBodyNames) {
  const auto Word = NdType::makeInt(8, false);
  for (bool Collision : {false, true}) {
    SCOPED_TRACE(Collision);
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.ReturnType = Word;
    Hint.Parameters = {{"", Word}, {Collision ? "arg0" : "", Word}, {"", Word}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Arch::AArch64, Error));
    auto Xor = HighExpr::makeBinop(NdOp::INT_XOR, parameter(0, Word),
                                   parameter(1, Word));
    Xor->Type = Word;
    auto Value = HighExpr::makeBinop(NdOp::INT_ADD, Xor, parameter(2, Word));
    Value->Type = Word;
    auto Function = returning("unnamed_fixed", Value, {Word, Word, Word});
    Function.SourceTypeHint = Hint;
    for (size_t I = 0; I < Function.Params.size(); ++I)
      Function.Params[I].Name = Hint.Parameters[I].Name;
    const auto Source = emit({Function}, true, Arch::AArch64);
    EXPECT_EQ(Function.Params[0].Name, "");
    EXPECT_EQ(Function.SourceTypeHint->Parameters[0].Name, "");
    for (const auto Level : {"-O0", "-O2"}) {
      SCOPED_TRACE(Level);
      compileAndRun(Source + R"(
int main(void) {
  uint64_t state=UINT64_C(0x91b623);
  for(unsigned i=0;i<512;++i) {
    state=state*UINT64_C(6364136223846793005)+1;
    uint64_t a=state,b=~(state>>7),c=state^(state<<17);
    if (unnamed_fixed(a,b,c)!=((a^b)+c)) return 1;
  }
  return 0;
}
)",
                    {Level, "-Werror=uninitialized"});
    }
  }
}

TEST(HighCSourceCalls, SavedFloatingRecordSurvivesLongCallChains) {
  for (unsigned Calls : {1U, 20U, 64U})
    for (bool ReplaceFirst : {false, true}) {
      SCOPED_TRACE(Calls);
      SCOPED_TRACE(ReplaceFirst);
      auto Med = savedFloatingRecord(Calls, ReplaceFirst);
      if (Calls == 64)
        std::reverse(Med.CallClobbers.begin(), Med.CallClobbers.end());
      ASSERT_TRUE(Med.SourceTypeHint);
      const auto High = lowerSavedFloatingRecord(Med);
      ASSERT_TRUE(High.SourceTypeHint);
      const auto Source = emit({High}, true, Arch::AArch64);
      ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
      const auto Program = "#include <string.h>\n" + Source + "\ntypedef " +
                           typeToC(High.ReturnType) +
                           " Record;\n#define CALLS " + std::to_string(Calls) +
                           "\n#define REPLACE " + std::to_string(ReplaceFirst) +
                           R"(
static unsigned calls;
void observe_call(void) { ++calls; }
int main(void) {
    const double values[] = {-0.0, 0.0, -37.5, 0.125, 1048576.25};
    for (unsigned i = 0; i < 5; ++i) {
        Record input = {{values[i], values[(i + 1) % 5]},
                        {values[(i + 2) % 5], values[(i + 3) % 5]}};
        unsigned before = calls;
        Record actual = saved_record(input);
        Record expected = input;
        if (REPLACE) expected.field_0.field_0 = input.field_0.field_1;
        if (calls - before != CALLS) return 1;
        if (memcmp(&actual, &expected, sizeof(actual))) return 2;
    }
    return 0;
}
)";
      for (const char *Optimization : {"-O0", "-O2"})
        compileAndRun(Program, {Optimization});
    }
}

TEST(HighCSourceCalls, CallClobberedVectorSuffixStaysUnknown) {
  for (unsigned Calls : {1U, 20U, 64U}) {
    SCOPED_TRACE(Calls);
    const auto Med = savedFloatingRecord(Calls, false, true);
    const auto Source =
        emit({lowerSavedFloatingRecord(Med)}, true, Arch::AArch64);
    EXPECT_NE(Source.find("unknown value"), std::string::npos) << Source;
  }
}

TEST(HighCSourceCalls, BrokenPreservedPrefixChainsRemainUnknown) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Med = savedFloatingRecord(3);
    std::vector<MedCallClobber *> Saved;
    for (auto &Clobber : Med.CallClobbers)
      if (Clobber.Value.RegOff == TRI.VecRegBase + 8 * TRI.VecRegStride)
        Saved.push_back(&Clobber);
    ASSERT_EQ(Saved.size(), 3U);
    switch (Mutation) {
    case 0:
      Saved[1]->PreservedPrefixSize = 0;
      Saved[1]->PreservedInput = {};
      break;
    case 1:
      Saved[1]->PreservedPrefixSize = 4;
      break;
    case 2:
      Saved[0]->PreservedInput = Saved[2]->Value;
      break;
    case 3:
      Saved[1]->PreservedInput.RegOff += TRI.VecRegStride;
      break;
    case 4:
      Saved[1]->PreservedInput.Size = 8;
      break;
    case 5:
      Saved[1]->PreservedInput.TheArch = Arch::X64;
      break;
    }
    // The first two are valid partial-value records; the remaining cases
    // also require bounded, conservative handling of malformed input.
    if (Mutation < 2)
      ASSERT_TRUE(verifyMedFunc(Med, "reduced-preserved-prefix"));
    const auto Source =
        emit({lowerSavedFloatingRecord(Med)}, true, Arch::AArch64);
    EXPECT_NE(Source.find("unknown value"), std::string::npos) << Source;
  }
}

TEST(HighCSourceCalls, DynamicTypeAndTypeNamePreserveCanonicalInputsAndPair) {
#if defined(__aarch64__) || defined(__arm64__)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  std::vector<HighFunc> Functions;
  for (const bool Dynamic : {true, false}) {
    const std::string Import = Dynamic ? "_swift_getDynamicType"
                                       : "_$ss9_typeName_9qualifiedSSypXp_SbtF";
    const va_t Slot = Dynamic ? 0x1000 : 0x2000;
    Image.ImportPtrSlots[Slot] = Import;
    Image.DyldBindSlots[Slot] = {Import, 0, "/usr/lib/swift/libswiftCore.dylib",
                                 false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    ASSERT_TRUE(Hint);
    for (const unsigned Flag : {0U, 1U}) {
      std::vector<TypeRef> Types{Pointer};
      std::vector<ExprPtr> Arguments{parameter(0, Pointer)};
      if (Dynamic) {
        Types.push_back(Pointer);
        Arguments.push_back(parameter(1, Pointer));
      }
      auto Boolean =
          HighExpr::makeBinop(NdOp::SUBBYTES, HighExpr::makeConst(Flag, 4),
                              HighExpr::makeConst(0, 4));
      Boolean->Type = NdType::makeInt(1, false);
      Arguments.push_back(Boolean);
      Functions.push_back(
          returning(std::string(Dynamic ? "project_type" : "project_name") +
                        std::to_string(Flag),
                    call(*Hint, Hint->Signature.ReturnType, Arguments), Types));
    }
  }
  const auto Source = emit(Functions, true, Architecture);
  EXPECT_EQ(Source.find("swift_context"), std::string::npos);
  // The independent definitions use actual one-bit prototypes and a mixed
  // word/pointer Swift record, rather than mirroring our byte/int128 types.
  const auto Program = Source + R"(
static unsigned seen, count;
#define SYMBOL_PREFIX_(prefix) #prefix
#define SYMBOL_PREFIX(prefix) SYMBOL_PREFIX_(prefix)
void *dynamic_oracle(void *, void *, _Bool)
    __asm__(SYMBOL_PREFIX(__USER_LABEL_PREFIX__) "swift_getDynamicType");
void *dynamic_oracle(void *value, void *metadata, _Bool flag) {
  seen = flag;
  ++count;
  return flag ? metadata : value;
}
struct StringWords { uint64_t word; void *storage; };
struct StringWords __attribute__((swiftcall))
name_oracle(void *, _Bool) __asm__("_$ss9_typeName_9qualifiedSSypXp_SbtF");
struct StringWords __attribute__((swiftcall))
name_oracle(void *metadata, _Bool flag) {
  seen = flag;
  ++count;
  return (struct StringWords){ (uint64_t)(uintptr_t)metadata + flag,
                              (void *)(uintptr_t)(0x778811ULL + flag) };
}
int main(void) {
  void *value = (void *)(uintptr_t)0x123456;
  void *metadata = (void *)(uintptr_t)0x556677;
  if (project_type0(value, metadata) != value || seen != 0) return 1;
  if (project_type1(value, metadata) != metadata || seen != 1) return 2;
  unsigned __int128 result = project_name0(metadata);
  if ((uint64_t)result != 0x556677 || (uint64_t)(result >> 64) != 0x778811 ||
      seen != 0) return 3;
  result = project_name1(metadata);
  if ((uint64_t)result != 0x556678 || (uint64_t)(result >> 64) != 0x778812 ||
      seen != 1 || count != 4) return 4;
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    compileAndRun(Program, {Optimization});
  }
}

TEST(HighCSourceCalls, SwiftDictionaryRemovalKeepsMetadataAndContextDistinct) {
#if defined(__aarch64__) || defined(__arm64__)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  const std::string Import =
      "_$ss17_NativeDictionaryV9removeAll8isUniqueySb_tF";
  Image.ImportPtrSlots[0x1000] = Import;
  Image.DyldBindSlots[0x1000] = {Import, 0, "/usr/lib/swift/libswiftCore.dylib",
                                 false};
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x1000);
  ASSERT_TRUE(Hint);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  std::vector<HighFunc> Functions;
  for (const unsigned Flag : {0U, 1U}) {
    HighFunc Function;
    Function.Name = "clear" + std::to_string(Flag);
    Function.ReturnType = NdType::makeVoid();
    Function.Params = {{"arg0", Pointer}, {"arg1", Pointer}};
    HighStmt Statement;
    Statement.Kind = StmtKind::ExprStmt;
    Statement.Val = call(*Hint, Hint->Signature.ReturnType,
                         {HighExpr::makeConst(Flag, 1), parameter(0, Pointer),
                          parameter(1, Pointer)});
    Function.Body = {Statement};
    Functions.push_back(Function);
  }
  const auto Source = emit(Functions, true, Architecture);
  ASSERT_NE(Source.find("swift_context"), std::string::npos);
  const auto Program = Source + R"(
static unsigned calls;
static const void *expected_metadata;
void __attribute__((swiftcall)) remove_oracle(
    _Bool, const void *, uintptr_t * __attribute__((swift_context)))
    __asm__("_$ss17_NativeDictionaryV9removeAll8isUniqueySb_tF");
void __attribute__((swiftcall)) remove_oracle(
    _Bool unique, const void *metadata,
    uintptr_t *dictionary __attribute__((swift_context))) {
  if (metadata != expected_metadata || *dictionary != 0x112233) __builtin_trap();
  *dictionary = unique ? 0x445566 : 0x778899;
  ++calls;
}
int main(void) {
  uintptr_t dictionary = 0x112233;
  expected_metadata = &calls;
  clear0((void *)expected_metadata, &dictionary);
  if (dictionary != 0x778899 || calls != 1) return 1;
  dictionary = 0x112233;
  clear1((void *)expected_metadata, &dictionary);
  if (dictionary != 0x445566 || calls != 2) return 2;
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    compileAndRun(Program, {Optimization});
  }
}

TEST(HighCSourceCalls, SwiftNSCoderPreservesMetadataContextAndCompleteResult) {
#if defined(__aarch64__) || defined(__arm64__)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Word = NdType::makeInt(8, false);
  std::vector<HighFunc> Functions;
  for (const bool Any : {false, true}) {
    const std::string Import =
        Any ? "_$"
              "sSo7NSCoderC10FoundationE12decodeObject2of6forKeyypSgSayyXlXpGSg"
              "_SStF"
            : "_$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyxSgxm_"
              "SStSo8NSObjectCRbzSo8NSCodingRzlF";
    const va_t Slot = Any ? 0x2000 : 0x1000;
    Image.ImportPtrSlots[Slot] = Import;
    Image.DyldBindSlots[Slot] = {
        Import, 0, "/System/Library/Frameworks/Foundation.framework/Foundation",
        false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    ASSERT_TRUE(Hint);
    // Ordinary wrapper prototypes come from the independent compiler client.
    const std::vector<TypeRef> Types =
        Any ? std::vector<TypeRef>{Pointer, Word, Word, Pointer, Pointer}
            : std::vector<TypeRef>{Pointer, Word, Pointer, Pointer, Pointer};
    std::vector<ExprPtr> Arguments;
    HighFunc Function;
    Function.Name = Any ? "decode_any" : "decode_typed";
    Function.ReturnType = Hint->Signature.ReturnType;
    for (unsigned I = 0; I != Types.size(); ++I) {
      Function.Params.push_back({"arg" + std::to_string(I), Types[I]});
      Arguments.push_back(parameter(I, Types[I]));
    }
    HighStmt Statement;
    Statement.Kind = Any ? StmtKind::ExprStmt : StmtKind::Return;
    auto Result = call(*Hint, Hint->Signature.ReturnType, Arguments);
    if (Any)
      Statement.Val = Result;
    else
      Statement.RetVal = Result;
    Function.Body = {Statement};
    Functions.push_back(Function);
  }
  const auto Source = emit(Functions, true, Architecture);
  ASSERT_NE(Source.find("swift_indirect_result"), std::string::npos);
  ASSERT_NE(Source.find("swift_context"), std::string::npos);
  const auto Program = Source + R"(
struct CoderOracle { unsigned calls, empty; };
struct AnyOracle { uintptr_t buffer[3]; const void *metadata; };
static int expected_type, expected_metadata, expected_storage;
uint64_t __attribute__((swiftcall)) typed_oracle(
    const void *, uint64_t, const void *, const void *,
    struct CoderOracle * __attribute__((swift_context)))
    __asm__("_$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyxSgxm_SStSo8NSObjectCRbzSo8NSCodingRzlF");
uint64_t __attribute__((swiftcall)) typed_oracle(
    const void *type, uint64_t word, const void *storage, const void *metadata,
    struct CoderOracle *coder __attribute__((swift_context))) {
  if (type != &expected_type || metadata != &expected_metadata ||
      storage != &expected_storage || word != 0x8123456789abcdefULL)
    __builtin_trap();
  ++coder->calls;
  return coder->empty ? 0 : (uint64_t)(uintptr_t)metadata;
}
void __attribute__((swiftcall)) any_oracle(
    struct AnyOracle * __attribute__((swift_indirect_result)),
    uint64_t, uint64_t, const void *,
    struct CoderOracle * __attribute__((swift_context)))
    __asm__("_$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyypSgSayyXlXpGSg_SStF");
void __attribute__((swiftcall)) any_oracle(
    struct AnyOracle *result __attribute__((swift_indirect_result)),
    uint64_t classes, uint64_t word, const void *storage,
    struct CoderOracle *coder __attribute__((swift_context))) {
  if (classes != 0xf123456789abcde0ULL || word != 0x1122334455667788ULL ||
      storage != &expected_storage) __builtin_trap();
  ++coder->calls;
  // Apple Clang 17's DSE drops nonvolatile payload stores in this explicit
  // swift_indirect_result definition even when called without our wrapper.
  // Keep the oracle writes observable; the generated caller stays optimized.
  volatile struct AnyOracle *observed = result;
  observed->buffer[0] = coder->empty ? 0 : classes;
  observed->buffer[1] = coder->empty ? 0 : word;
  observed->buffer[2] = coder->empty ? 0 : (uintptr_t)storage;
  observed->metadata = coder->empty ? 0 : &expected_metadata;
}
int main(void) {
  struct CoderOracle coder = {0, 0};
  struct { uint64_t before; struct AnyOracle value; uint64_t after; } box;
  box.before = 0x0123456789abcdefULL;
  box.after = 0xfedcba9876543210ULL;
  for (unsigned empty = 0; empty != 2; ++empty) {
    coder.empty = empty;
    uint64_t object = decode_typed(&expected_type, 0x8123456789abcdefULL,
                                   &expected_storage, &expected_metadata, &coder);
    if (object != (empty ? 0 : (uint64_t)(uintptr_t)&expected_metadata)) return 1;
    decode_any(&box.value, 0xf123456789abcde0ULL, 0x1122334455667788ULL,
               &expected_storage, &coder);
    if (box.value.buffer[0] != (empty ? 0 : 0xf123456789abcde0ULL) ||
        box.value.buffer[1] != (empty ? 0 : 0x1122334455667788ULL) ||
        box.value.buffer[2] != (empty ? 0 : (uintptr_t)&expected_storage) ||
        box.value.metadata != (empty ? 0 : &expected_metadata)) return 2;
    if (box.before != 0x0123456789abcdefULL || box.after != 0xfedcba9876543210ULL ||
        coder.calls != 2 * (empty + 1)) return 3;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    compileAndRun(Program, {Optimization});
  }
}

TEST(HighCSourceCalls, SwiftSubjectInitializerTransportsValueContextAndResult) {
#if defined(__aarch64__) || defined(_M_ARM64)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  auto Image = runtime_function_address_test::image(Architecture);
  const va_t Slot = runtime_function_address_test::Slot;
  const std::string Name = "_$s7Combine19CurrentValueSubjectCyACyxq_Gxcfc";
  Image.ImportPtrSlots[Slot] = Name;
  Image.DyldBindSlots[Slot] = {
      Name, 0, "/System/Library/Frameworks/Combine.framework/Combine", false};
  const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
  ASSERT_TRUE(Hint);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Call =
      call(*Hint, Pointer, {parameter(0, Pointer), parameter(1, Pointer)});
  ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
  const auto Function =
      returning("recovered_subject", Call, {Pointer, Pointer});
  const auto Source = emit({Function}, true, Architecture);
  ASSERT_NE(Source.find("swift_context"), std::string::npos);
  ASSERT_NE(Source.find("swiftcall"), std::string::npos);
  // This is a carrier oracle, not a replacement Combine implementation. The
  // result deliberately differs from the context, and the input stays opaque.
  const auto Program = Source + R"(
#include <string.h>
struct Context { unsigned size, calls; uint64_t result; unsigned char data[80]; };
void *__attribute__((swiftcall)) subject_oracle(
    void *, struct Context * __attribute__((swift_context)))
    __asm__("_$s7Combine19CurrentValueSubjectCyACyxq_Gxcfc");
void *__attribute__((swiftcall)) subject_oracle(
    void *value, struct Context *context __attribute__((swift_context))) {
  unsigned char *input = value;
  for (unsigned i = 0; i < context->size; ++i) {
    context->data[i] = input[i];
    input[i] ^= 0x5a;
  }
  ++context->calls;
  context->result = (uint64_t)(uintptr_t)value ^ context->size;
  return &context->result;
}
int main(void) {
  for (unsigned n = 0; n <= 80; ++n) {
    struct { uint64_t before; struct Context value; uint64_t after; } box;
    unsigned char input[82];
    memset(&box, 0xa5, sizeof(box));
    memset(input, 0x6b, sizeof(input));
    box.before = 0x0123456789abcdefULL; box.after = 0xfedcba9876543210ULL;
    box.value.size = n; box.value.calls = 0;
    for (unsigned i = 0; i < n; ++i) input[i + 1] = (unsigned char)(i * 13 + n);
    void *result = recovered_subject(input + 1, &box.value);
    if (result != &box.value.result || box.value.calls != 1 ||
        box.value.result != ((uint64_t)(uintptr_t)(input + 1) ^ n)) return 1;
    if (input[0] != 0x6b || input[n + 1] != 0x6b ||
        box.before != 0x0123456789abcdefULL || box.after != 0xfedcba9876543210ULL)
      return 2;
    for (unsigned i = 0; i < n; ++i)
      if (box.value.data[i] != (unsigned char)(i * 13 + n) ||
          input[i + 1] != (unsigned char)((i * 13 + n) ^ 0x5a)) return 3;
    for (unsigned i = n; i < 80; ++i)
      if (box.value.data[i] != 0xa5) return 4;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"})
    compileAndRun(Program, {Optimization});
}

TEST(HighCSourceCalls, SwiftPublishedInitializerTransportsOpaqueStorage) {
#if defined(__aarch64__) || defined(_M_ARM64)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  auto Image = runtime_function_address_test::image(Architecture);
  const va_t Slot = runtime_function_address_test::Slot;
  const std::string Name = "_$s7Combine9PublishedV12initialValueACyxGx_tcfC";
  Image.ImportPtrSlots[Slot] = Name;
  Image.DyldBindSlots[Slot] = {
      Name, 0, "/System/Library/Frameworks/Combine.framework/Combine", false};
  const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
  ASSERT_TRUE(Hint);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  HighFunc Function;
  Function.Name = "recovered_published";
  Function.ReturnType = NdType::makeVoid();
  std::vector<ExprPtr> Arguments;
  for (unsigned I = 0; I < 3; ++I) {
    Function.Params.push_back({"arg" + std::to_string(I), Pointer});
    Arguments.push_back(parameter(I, Pointer));
  }
  HighStmt Statement;
  Statement.Kind = StmtKind::ExprStmt;
  Statement.Val = call(*Hint, Function.ReturnType, Arguments);
  Function.Body = {Statement};
  const auto Source = emit({Function}, true, Architecture);
  ASSERT_NE(Source.find("swift_indirect_result"), std::string::npos);
  const auto Program = Source + R"(
#include <string.h>
struct TypeMetadata { unsigned size, calls; };
void __attribute__((swiftcall)) published_oracle(
    void * __attribute__((swift_indirect_result)), void *, struct TypeMetadata *)
    __asm__("_$s7Combine9PublishedV12initialValueACyxGx_tcfC");
void __attribute__((swiftcall)) published_oracle(
    void *result __attribute__((swift_indirect_result)), void *value,
    struct TypeMetadata *metadata) {
  volatile unsigned char *out = result;
  unsigned char *in = value;
  for (unsigned i = 0; i < metadata->size; ++i) {
    out[i] = in[i];
    in[i] = 0;
  }
  ++metadata->calls;
}
int main(void) {
  for (unsigned n = 0; n < 80; ++n) {
    struct TypeMetadata metadata = {n, 0};
    unsigned char input[80], output[82];
    for (unsigned i = 0; i < 80; ++i) input[i] = (unsigned char)(i * 7 + n);
    memset(output, 0xa5, sizeof(output));
    recovered_published(output + 1, input, &metadata);
    if (metadata.calls != 1 || output[0] != 0xa5 || output[n + 1] != 0xa5) return 1;
    for (unsigned i = 0; i < n; ++i)
      if (input[i] || output[i + 1] != (unsigned char)(i * 7 + n)) return 2;
    for (unsigned i = n; i < 80; ++i)
      if (input[i] != (unsigned char)(i * 7 + n)) return 3;
    for (unsigned i = n + 1; i < sizeof(output); ++i)
      if (output[i] != 0xa5) return 4;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"})
    compileAndRun(Program, {Optimization});
}

TEST(HighCSourceCalls, SwiftAnyHashableInitializerKeepsAllOpaqueCarriers) {
#if defined(__aarch64__) || defined(_M_ARM64)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  auto Image = runtime_function_address_test::image(Architecture);
  const va_t Slot = runtime_function_address_test::Slot;
  const std::string Name = "_$ss11AnyHashableVyABxcSHRzlufC";
  Image.ImportPtrSlots[Slot] = Name;
  Image.DyldBindSlots[Slot] = {Name, 0, "/usr/lib/swift/libswiftCore.dylib",
                               false};
  const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
  ASSERT_TRUE(Hint);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  HighFunc Function;
  Function.Name = "recovered_anyhashable";
  Function.ReturnType = NdType::makeVoid();
  std::vector<ExprPtr> Args;
  for (unsigned I = 0; I < 4; ++I) {
    Function.Params.push_back({"arg" + std::to_string(I), Pointer});
    Args.push_back(parameter(I, Pointer));
  }
  HighStmt Statement;
  Statement.Kind = StmtKind::ExprStmt;
  Statement.Val = call(*Hint, Function.ReturnType, Args);
  ASSERT_TRUE(sdk::objcSourceCallBound(*Statement.Val, Image, {}));
  Function.Body = {Statement};
  const auto Source = emit({Function}, true, Architecture);
  ASSERT_NE(Source.find("swift_indirect_result"), std::string::npos);
  ASSERT_NE(Source.find("swiftcall"), std::string::npos);
  ASSERT_EQ(Source.find("swift_context"), std::string::npos);
  // Only a carrier oracle: its variable-size output and input mutation must
  // not imply a Swift value layout or a borrowed private-frame permission.
  const auto Program = Source + R"(
#include <string.h>
struct Metadata { unsigned size, calls; };
struct Witness { unsigned salt, calls; };
void __attribute__((swiftcall)) anyhashable_oracle(
    void * __attribute__((swift_indirect_result)), void *,
    struct Metadata *, struct Witness *)
    __asm__("_$ss11AnyHashableVyABxcSHRzlufC");
void __attribute__((swiftcall)) anyhashable_oracle(
    void *result __attribute__((swift_indirect_result)), void *value,
    struct Metadata *metadata, struct Witness *witness) {
  unsigned char *out=result, *in=value;
  for (unsigned i=0;i<metadata->size;++i) {
    out[i]=in[i]^(unsigned char)(witness->salt+i);
    in[i]=0;
  }
  ++metadata->calls; ++witness->calls;
}
int main(void) {
  for (unsigned n=0;n<=80;++n) {
    struct Metadata metadata={n,0};struct Witness witness={n*17+3,0};
    unsigned char input[82],output[82];
    memset(input,0x6b,sizeof(input));memset(output,0xa5,sizeof(output));
    for(unsigned i=0;i<n;++i)input[i+1]=(unsigned char)(i*7+n);
    recovered_anyhashable(output+1,input+1,&metadata,&witness);
    if(metadata.calls!=1||witness.calls!=1||metadata.size!=n||witness.salt!=n*17+3)return 1;
    if(input[0]!=0x6b||output[0]!=0xa5)return 2;
    for(unsigned i=0;i<n;++i)
      if(input[i+1]||output[i+1]!=((unsigned char)(i*7+n)^(unsigned char)(witness.salt+i)))return 3;
    for(unsigned i=n+1;i<82;++i)
      if(input[i]!=0x6b||output[i]!=0xa5)return 4;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"})
    compileAndRun(Program, {Optimization});
}

TEST(HighCSourceCalls, SwiftStaticArrayKeepsTokenHeaderPayloadAndAliases) {
  constexpr va_t Base = 0x1008;
  constexpr va_t Slot = 0x2000;
  auto Image = runtime_function_address_test::image(Arch::AArch64);
  Image.MachOTwoLevelNamespace = true;
  Image.ImportPtrSlots[Slot] = "_swift_initStaticObject";
  Image.DyldBindSlots[Slot].Name = "_swift_initStaticObject";
  Segment Data;
  Data.VA = Data.FileOff = Base;
  Data.Size = Data.FileSz = 72;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(72);
  Data.Data[24] = 1;
  Data.Data[32] = 2;
  Data.Data[40] = 'a';
  Data.Data[41] = 'k';
  Data.Data[55] = 0xe2;
  Data.Data[56] = 't';
  Data.Data[57] = 'w';
  Data.Data[71] = 0xe2;
  Image.Segments.push_back(Data);
  Section Section;
  Section.Name = "__data";
  Section.VA = Section.FileOff = Data.VA;
  Section.Size = Section.FileSz = Data.Size;
  Section.Flags = Data.Flags;
  Image.Sections.push_back(Section);
  Image.Symbols.push_back(
      {"_$s14StorageFixture5pairsSaySS_SStGyFTv_", Base, 0, false});
  Image.Symbols.push_back({"_next", Base + 72, 0, false});
  const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
  ASSERT_TRUE(Hint);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Constructor = returning(
      "initialize_pairs",
      call(*Hint, Pointer,
           {parameter(0, Pointer),
            HighExpr::makeConst(Base + 8, 8,
                                ConstantAddressProvenance::DataAddress)}),
      {Pointer});
  auto Payload =
      returning("pair_payload",
                HighExpr::makeConst(Base + 40, 8,
                                    ConstantAddressProvenance::DataAddress));
  const auto BoundConstructor =
      sdk::bindObjCSourceReferences(Constructor, Image);
  const auto BoundPayload = sdk::bindObjCSourceReferences(Payload, Image);
  ASSERT_TRUE(BoundConstructor.Limitation.empty())
      << BoundConstructor.Limitation;
  ASSERT_TRUE(BoundPayload.Limitation.empty()) << BoundPayload.Limitation;
  ASSERT_EQ(BoundConstructor.LocalStorageExtents,
            BoundPayload.LocalStorageExtents);
  std::set<std::string> Shared;
  const auto Source = emit({BoundConstructor.Function, BoundPayload.Function},
                           true, Arch::AArch64) +
                      sdk::renderObjCLocalStorageHelpers(
                          Image, BoundConstructor.LocalStorageExtents, Shared) +
                      R"(
static unsigned initialized;
#define SYMBOL_PREFIX_(prefix) #prefix
#define SYMBOL_PREFIX(prefix) SYMBOL_PREFIX_(prefix)
void *initialize_oracle(void *metadata, void *object)
  __asm__(SYMBOL_PREFIX(__USER_LABEL_PREFIX__) "swift_initStaticObject");
void *initialize_oracle(void *metadata, void *object) {
  uintptr_t *header = object;
  if (!header[-1]) {
    ++initialized;
    header[0] = (uintptr_t)metadata;
    header[1] = 0x12345678;
    header[-1] = 1;
  }
  return object;
}
int main(void) {
  int first, second;
  unsigned char *payload = (unsigned char *)(uintptr_t)pair_payload();
  if (payload[0] != 'a' || payload[1] != 'k' || payload[15] != 0xe2 ||
      payload[16] != 't' || payload[17] != 'w' || payload[31] != 0xe2) return 1;
  uintptr_t *object = initialize_pairs(&first);
  if ((unsigned char *)object + 32 != payload || object[-1] != 1 ||
      object[0] != (uintptr_t)&first || object[1] != 0x12345678 ||
      object[2] != 1 || object[3] != 2) return 2;
  payload[0] = 'x';
  if (initialize_pairs(&second) != object || initialized != 1 ||
      object[0] != (uintptr_t)&first || ((unsigned char *)object)[32] != 'x' ||
      pair_payload() != (uintptr_t)payload) return 3;
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"})
    compileAndRun(Source, {Optimization, "-fsanitize=alignment",
                           "-fsanitize-trap=alignment"});
}

TEST(HighCSourceCalls, NamedReleaseStorageKeepsOrderingBitsAndAdjacentBytes) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = Data.FileOff = 0x1000;
  Data.Size = Data.FileSz = 24;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(24);
  std::fill_n(Data.Data.begin(), 8, 0xa5);
  std::fill_n(Data.Data.begin() + 16, 8, 0x5a);
  Image.Segments.push_back(Data);
  Section Section;
  Section.Name = "__data";
  Section.VA = Section.FileOff = Data.VA;
  Section.Size = Section.FileSz = Data.Size;
  Section.Flags = Data.Flags;
  Image.Sections.push_back(Section);
  Symbol Storage;
  Storage.Name = "_published_state";
  Storage.Addr = Data.VA;
  Storage.Size = Data.Size;
  Image.Symbols.push_back(Storage);
  const auto Word = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  std::vector<HighFunc> Functions;
  std::map<va_t, uint64_t> Extents;
  for (const bool Object : {false, true}) {
    HighFunc Function;
    Function.Name = Object ? "publish_pointer" : "publish_word";
    Function.ReturnType = NdType::makeVoid();
    Function.Params = {{"arg0", Object ? Pointer : Word}};
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = HighExpr::makeConst(0x1008, 8);
    Store.StoreVal = parameter(0, Function.Params.front().Type);
    Store.MemoryOrdering = NdMemoryOrdering::Release;
    Function.Body = {Store};
    const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.Function.Body.front().MemoryOrdering,
              NdMemoryOrdering::Release);
    for (const auto &[Address, Width] : Bound.LocalStorageExtents)
      Extents[Address] = std::max(Extents[Address], Width);
    Functions.push_back(Bound.Function);
  }
  for (unsigned I = 0; I != 3; ++I) {
    auto Function = returning(
        "read_word" + std::to_string(I),
        HighExpr::makeLoad(HighExpr::makeConst(0x1000 + 8 * I, 8), Word));
    const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    for (const auto &[Address, Width] : Bound.LocalStorageExtents)
      Extents[Address] = std::max(Extents[Address], Width);
    Functions.push_back(Bound.Function);
  }
  std::set<std::string> Shared;
  const auto Source =
      emit(Functions, true, Arch::AArch64) +
      sdk::renderObjCLocalStorageHelpers(Image, Extents, Shared);
  ASSERT_NE(Source.find("__ATOMIC_RELEASE"), std::string::npos);
  const auto Program = Source + R"(
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_MAX, 0x8000000000000000ULL,
                              0x1234567887654321ULL, 0};
  uint64_t *published = (uint64_t *)(neverd_local_storage_1000_address() + 8);
  for (unsigned i = 0; i != sizeof(values) / sizeof(values[0]); ++i) {
    publish_word(values[i]);
    if (__atomic_load_n(published, __ATOMIC_ACQUIRE) != values[i] ||
        read_word1() != values[i]) return 1;
    if (read_word0() != 0xa5a5a5a5a5a5a5a5ULL ||
        read_word2() != 0x5a5a5a5a5a5a5a5aULL) return 2;
  }
  publish_pointer(published);
  if (__atomic_load_n(published, __ATOMIC_ACQUIRE) != (uintptr_t)published ||
      read_word1() != (uintptr_t)published) return 3;
  publish_pointer(0);
  if (read_word1() || read_word0() != 0xa5a5a5a5a5a5a5a5ULL ||
      read_word2() != 0x5a5a5a5a5a5a5a5aULL) return 4;
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    compileAndRun(Program, {Optimization});
  }
}

TEST(HighCSourceCalls, CFunctionParameterCallPreservesDispatchAndEffects) {
  using namespace c_function_parameter_test;
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.high());
  const auto Source = emit({*F.high()}, true, Arch::AArch64);
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("objc/message.h"), std::string::npos);
  const auto Program = Source + R"(
struct Input { int value; unsigned released, observed; };
static unsigned calls, order;
void swift_release(void *p) {
  struct Input *input = p;
  ++input->released;
  input->value += 5;
  order = order * 10 + 1;
}
static void callback_a(void *p) {
  struct Input *input = p;
  input->observed = input->value + 7;
  ++calls;
  order = order * 10 + 2;
}
static void callback_b(void *p) {
  struct Input *input = p;
  input->observed = input->value - 3;
  ++calls;
  order = order * 10 + 3;
}
int main(void) {
  struct Input a = {13, 0, 0}, b = {20, 0, 0};
  invoke_callback(&a, callback_a);
  invoke_callback(&b, callback_b);
  return a.released != 1 || b.released != 1 || a.observed != 25 ||
         b.observed != 22 || calls != 2 || order != 1213;
}
)";
  compileAndRun(Program, {"-O0", "-Werror"});
  compileAndRun(Program, {"-O2", "-Werror"});
  auto Forged = *F.high();
  for (auto &Statement : Forged.Body)
    if (Statement.CallExpr && Statement.CallExpr->SourceCallHint &&
        Statement.CallExpr->SourceCallHint->FunctionParameterCall) {
      Statement.CallExpr = std::make_shared<HighExpr>(*Statement.CallExpr);
      auto Hint = *Statement.CallExpr->SourceCallHint;
      Hint.Signature.Convention = SourceFunctionTypeHint::ConventionKind::Swift;
      Statement.CallExpr->SourceCallHint =
          std::make_shared<const SourceCallTypeHint>(std::move(Hint));
    }
  EXPECT_NE(emit({Forged}, true, Arch::AArch64).find("bad source call"),
            std::string::npos);
}

TEST(HighCSourceCalls, SharedStoreTailPreservesOneDynamicCallback) {
  using namespace c_function_parameter_test;
  Fixture F(false, true, true);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  ASSERT_TRUE(F.high());
  const auto Source = emit({*F.high()}, true, Arch::AArch64);
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  const auto Program = Source + R"(
static unsigned calls, failed;
static void callback_a(void *p) {
  uint64_t *words = p;
  failed |= words[1] != 0;
  words[1] = words[0] + 7;
  ++calls;
}
static void callback_b(void *p) {
  uint64_t *words = p;
  failed |= words[1] != 0;
  words[1] = words[0] ^ UINT64_C(0xd182e394f5061728);
  ++calls;
}
int main(void) {
  for (unsigned i = 0; i != 2048; ++i) {
    uint64_t words[] = {UINT64_C(0x1234567887654321), 99, 101,
                        UINT64_C(0xabcdef0123456789)};
    uint64_t first = (i & 3) ? UINT64_C(0x8000000000000000) + i : 0;
    uint64_t second = (i & 4) ? UINT64_MAX - i : 0;
    uint64_t expected = !first ? 22 : !second ? 33 : 11;
    unsigned before = calls;
    shared_tail_callback(words + 1, (i & 1) ? callback_a : callback_b,
                         first, second);
    if (calls != before + 1 || failed || words[1] != expected ||
        words[2] != ((i & 1) ? expected + 7 :
                     expected ^ UINT64_C(0xd182e394f5061728)) ||
        words[0] != UINT64_C(0x1234567887654321) ||
        words[3] != UINT64_C(0xabcdef0123456789)) return 1;
  }
  return 0;
}
)";
  compileAndRun(Program, {"-O0", "-Werror"});
  compileAndRun(Program, {"-O2", "-Werror"});
}

TEST(HighCSourceCalls, DeclaresOpaqueBlockObjectPointerUsedByHelpers) {
  const auto Opaque =
      NdType::makePtr(NdType::makeNamedRecord("_Block_object", 8));
  const auto Function =
      returning("opaque_block_identity", parameter(0, Opaque), {Opaque});
  for (const bool Includes : {true, false}) {
    const auto Source = emit({Function}, Includes);
    EXPECT_NE(Source.find("typedef struct _Block_object _Block_object;"),
              std::string::npos);
    compileAndRun(
        (Includes ? "" : "#include <stdint.h>\n") + Source +
        "\nint main(void) { return opaque_block_identity(0) != 0; }\n");
  }
}

TEST(HighCSourceCalls,
     UntypedStoresPreserveUnalignedBytesAndExpressionResults) {
  const auto Word = NdType::makeInt(8, false);
  const auto Address = NdType::makeInt(8, false);
  std::vector<HighFunc> Functions;
  for (unsigned Form = 0; Form < 3; ++Form) {
    auto F = returning("raw_store_" + std::to_string(Form),
                       HighExpr::makeLoad(parameter(0, Address), Word),
                       {Address, Word});
    if (Form == 1) {
      auto Store = std::make_shared<HighExpr>();
      Store->Kind = ExprKind::Store;
      Store->Type = Word;
      Store->Operands = {parameter(0, Address), parameter(1, Word)};
      F.Body[0].RetVal = Store;
    } else {
      HighStmt Store;
      if (Form == 0) {
        Store.Kind = StmtKind::Store;
        Store.StoreAddr = parameter(0, Address);
        Store.StoreVal = parameter(1, Word);
      } else {
        Store.Kind = StmtKind::Assign;
        Store.Dst = HighExpr::makeLoad(parameter(0, Address), Word);
        Store.Val = parameter(1, Word);
      }
      F.Body.insert(F.Body.begin(), Store);
    }
    Functions.push_back(std::move(F));
  }
  const std::string Check = R"(
int main(void) {
  _Alignas(16) unsigned char bytes[32];
  uint64_t (*stores[])(uint64_t, uint64_t) = {raw_store_0, raw_store_1, raw_store_2};
  for (unsigned i = 0; i != 3; ++i) {
    memset(bytes, 0xA5, sizeof(bytes));
    uint64_t expected = UINT64_C(0x9182736455463728) + i;
    uint64_t result = stores[i]((uintptr_t)(bytes + 1), expected);
    uint64_t actual;
    memcpy(&actual, bytes + 1, sizeof(actual));
    if (result != expected || actual != expected || bytes[0] != 0xA5 || bytes[9] != 0xA5)
      return 1;
  }
  return 0;
}
)";
  using Spelling = CEmitterOptions::ScalarPointerSpelling;
  for (const llvm::StringRef Opt : {"-O0", "-O2"}) {
    // The standard types read scalars at any address, as the targets do.
    compileAndRun(emit(Functions) + Check, {Opt});
    // The alias types keep the bytes exact even where alignment is checked;
    // traps exercise that without a sanitizer runtime library.
    compileAndRun(emit(Functions, true, Arch::X64, Spelling::AliasTypes) +
                      Check,
                  {Opt, "-fsanitize=alignment", "-fsanitize-trap=alignment"});
  }
}

TEST(HighCSourceCalls, UnusedCallsAfterPredicatesPreserveSideEffects) {
  const auto Integer = NdType::makeInt(8);
  const auto Effect = native("fixture_predicate_effect", Integer, {Integer});
  const auto Marker =
      native("fixture_predicate_marker", NdType::makeVoid(), {});
  std::vector<HighFunc> Functions;
  for (unsigned Form = 0; Form != 3; ++Form) {
    auto Function = returning("predicate_calls_" + std::to_string(Form),
                              HighExpr::makeConst(0, 8), {Integer});
    auto Predicate = call(Effect, Integer, {parameter(0, Integer)});
    MedVar Result;
    Result.Kind = MedVar::Temp;
    Result.Id = 19;
    Result.Size = 8;
    HighStmt Unused;
    Unused.Kind = StmtKind::Assign;
    Unused.Dst = HighExpr::makeVar(Result, Integer);
    Unused.Val = call(Effect, Integer, {HighExpr::makeConst(7, 8)});
    HighStmt Guard;
    Guard.Kind = StmtKind::If;
    Guard.Cond = Predicate;
    Guard.Body = {Unused};
    if (Form == 1) {
      MedVar PredicateResult = Result;
      PredicateResult.Id = 18;
      HighStmt Evaluate;
      Evaluate.Kind = StmtKind::Assign;
      Evaluate.Dst = HighExpr::makeVar(PredicateResult, Integer);
      Evaluate.Val = Predicate;
      Guard.Cond = HighExpr::makeVar(PredicateResult, Integer);
      Function.Body.insert(Function.Body.begin(), {Evaluate, Guard});
    } else if (Form == 2) {
      HighStmt Mark;
      Mark.Kind = StmtKind::Call;
      Mark.CallExpr = call(Marker, NdType::makeVoid());
      Guard.Body = {Mark};
      Function.Body.insert(Function.Body.begin(), {Guard, Unused});
    } else {
      Function.Body.insert(Function.Body.begin(), Guard);
    }
    Functions.push_back(std::move(Function));
  }
  compileAndRun(emit(Functions) + R"(
static int calls, total, markers;
int64_t fixture_predicate_effect(int64_t value) {
  ++calls;
  total += value;
  return value;
}
void fixture_predicate_marker(void) { ++markers; }
int main(void) {
  uint64_t (*functions[])(int64_t) = {
    predicate_calls_0, predicate_calls_1, predicate_calls_2
  };
  for (unsigned form = 0; form != 3; ++form) {
    for (int condition = 0; condition != 2; ++condition) {
      calls = total = markers = 0;
      if (functions[form](condition) != 0)
        return 1;
      const int has_second = condition || form == 2;
      if (calls != 1 + has_second || total != condition + 7 * has_second ||
          markers != (form == 2 && condition))
        return 2;
    }
  }
  return 0;
}
)");
}

TEST(HighCSourceCalls, UnknownOnlyTempsFailAtTheirObservableUse) {
  const auto Integer = NdType::makeInt(8);
  MedVar Unknown;
  Unknown.Kind = MedVar::Temp;
  Unknown.Id = 731;
  Unknown.Size = 8;
  Unknown.SSAVer = 3;
  HighStmt Define;
  Define.Kind = StmtKind::Assign;
  Define.Dst = HighExpr::makeVar(Unknown, Integer);
  Define.Val = HighExpr::makeUndef(8);
  HighStmt Consume;
  Consume.Kind = StmtKind::Call;
  Consume.CallExpr =
      call(native("fixture_unknown_consumer", NdType::makeVoid(), {Integer}),
           NdType::makeVoid(), {HighExpr::makeVar(Unknown, Integer)});
  HighStmt Guard;
  Guard.Kind = StmtKind::If;
  Guard.Cond = parameter(0, Integer);
  Guard.Body = {Consume};
  auto Function =
      returning("unknown_argument", HighExpr::makeConst(0, 8), {Integer});
  Function.Body.insert(Function.Body.begin(), {Define, Guard});
  const std::string Source = emit({Function});
  EXPECT_NE(Source.find("__builtin_trap(), 0 /* unknown value */"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("t731"), std::string::npos) << Source;
  // The harness intercepts the emitted failure intrinsic so both the normal
  // path and the failure before calling a consumer can execute in one process.
  compileAndRun(R"(
#include <setjmp.h>
static jmp_buf failure;
static void fixture_unknown_trap(void) { longjmp(failure, 1); }
#define __builtin_trap fixture_unknown_trap
)" + Source + R"(
static int consumed;
void fixture_unknown_consumer(int64_t value) { ++consumed; }
int main(void) {
  if (unknown_argument(0) != 0 || consumed)
    return 1;
  if (setjmp(failure) == 0) {
    unknown_argument(1);
    return 2;
  }
  return consumed ? 3 : 0;
}
)");
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
    Statement.CallExpr =
        call(Binding, NdType::makeVoid(), {HighExpr::makeConst(Value, 4)});
    Function.Body = {std::move(Statement)};
    return emit({Function});
  };

  const auto Null = Render(0);
  EXPECT_NE(Null.find("fixture_pointer_consumer((void*)0)"), std::string::npos)
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

TEST(HighCSourceCalls, SwiftVirtualGetterUsesSwiftContextFunctionType) {
  for (bool Bool : {false, true}) {
    SourceCallTypeHint Hint;
    Hint.CallKind = SourceCallTypeHint::Kind::SwiftVirtual;
    Hint.TargetName = "swift_virtual";
    Hint.Virtual = SourceCallTypeHint::SwiftVirtualEvidence{
        0x1000, 0x1010, 0x2000, Bool ? 600U : 624U};
    Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.Signature.ReturnType =
        Bool ? NdType::makeInt(1, false) : NdType::makeFloat(8);
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    Hint.Signature.Parameters = {{"self", Pointer}};
    Hint.Signature.Parameters[0].TheRole =
        SourceParameterTypeHint::Role::SwiftContext;
    std::string Diagnostic;
    ASSERT_TRUE(
        assignDarwinSwiftSourceABI(Hint.Signature, Arch::AArch64, Diagnostic))
        << Diagnostic;

    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      V.TheArch = Arch::AArch64;
      return HighExpr::makeVar(V, Pointer);
    };
    auto Call = HighExpr::makeCall("indirect_call", 0, {Param(0)});
    Call->Type = Hint.Signature.ReturnType;
    Call->IsIndirectCall = true;
    Call->IndirectTarget = Param(1);
    Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "call_virtual";
    Function.ReturnType = Hint.Signature.ReturnType;
    Function.Params = {{"context", Pointer}, {"target", Pointer}};
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body = {Return};
    const auto Source = emit({Function}, true, Arch::AArch64);
    EXPECT_NE(Source.find(Bool ? "_Bool __attribute__((swiftcall)) (*)"
                               : "double __attribute__((swiftcall)) (*)"),
              std::string::npos)
        << Source;
    if (Bool) {
      compileAndRun(Source + R"(
static _Bool __attribute__((swiftcall)) getter(
    void *self __attribute__((swift_context))) {
  return (*(const uint8_t *)self & 1) != 0;
}
int main(void) {
  uint8_t value = 1;
  return call_virtual(&value, (void *)&getter) == 1 ? 0 : 1;
}
)");
    } else {
      compileAndRun(Source + R"(
static double __attribute__((swiftcall)) getter(
    void *self __attribute__((swift_context))) {
  return *(const double *)self + 0.5;
}
int main(void) {
  double value = 42.0;
  return call_virtual(&value, (void *)&getter) == 42.5 ? 0 : 1;
}
)");
    }
  }
}

TEST(HighCSourceCalls, SwiftVirtualSetterUsesValueAndSwiftContext) {
  for (bool Bool : {false, true}) {
    SourceCallTypeHint Hint;
    Hint.CallKind = SourceCallTypeHint::Kind::SwiftVirtual;
    Hint.TargetName = "swift_virtual";
    Hint.Virtual = SourceCallTypeHint::SwiftVirtualEvidence{
        0x1000, 0x1010, 0x2000, Bool ? 608U : 632U};
    Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.Signature.ReturnType = NdType::makeVoid();
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto ValueType =
        Bool ? NdType::makeInt(1, false) : NdType::makeFloat(8);
    Hint.Signature.Parameters = {{"value", ValueType}, {"self", Pointer}};
    Hint.Signature.Parameters[1].TheRole =
        SourceParameterTypeHint::Role::SwiftContext;
    std::string Diagnostic;
    ASSERT_TRUE(
        assignDarwinSwiftSourceABI(Hint.Signature, Arch::AArch64, Diagnostic))
        << Diagnostic;

    auto Param = [&](unsigned Id, TypeRef Type) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = Type->Size;
      V.TheArch = Arch::AArch64;
      return HighExpr::makeVar(V, Type);
    };
    auto Call = HighExpr::makeCall("indirect_call", 0,
                                   {Param(0, ValueType), Param(1, Pointer)});
    Call->Type = NdType::makeVoid();
    Call->IsIndirectCall = true;
    Call->IndirectTarget = Param(2, Pointer);
    Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "call_virtual_setter";
    Function.ReturnType = NdType::makeVoid();
    Function.Params = {
        {"value", ValueType}, {"context", Pointer}, {"target", Pointer}};
    HighStmt Statement;
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr = Call;
    Function.Body = {Statement};
    const auto Source = emit({Function}, true, Arch::AArch64);
    EXPECT_NE(
        Source.find(Bool ? "(*)(_Bool, void* __attribute__((swift_context)))"
                         : "(*)(double, void* __attribute__((swift_context)))"),
        std::string::npos)
        << Source;
    if (Bool) {
      compileAndRun(Source + R"(
static void __attribute__((swiftcall)) setter(
    _Bool value, void *self __attribute__((swift_context))) {
  *(uint8_t *)self = value;
}
int main(void) {
  uint8_t value = 0;
  call_virtual_setter(1, &value, (void *)&setter);
  return value == 1 ? 0 : 1;
}
)");
    } else {
      compileAndRun(Source + R"(
static void __attribute__((swiftcall)) setter(
    double value, void *self __attribute__((swift_context))) {
  *(double *)self = value;
}
int main(void) {
  double value = 0.0;
  call_virtual_setter(42.5, &value, (void *)&setter);
  return value == 42.5 ? 0 : 1;
}
)");
    }
  }
}

TEST(HighCSourceCalls, SwiftVirtualVoidMethodUsesSwiftContextOnly) {
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::SwiftVirtual;
  Hint.TargetName = "swift_virtual";
  Hint.Virtual =
      SourceCallTypeHint::SwiftVirtualEvidence{0x1000, 0x1010, 0x2000, 280};
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Signature.ReturnType = NdType::makeVoid();
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Hint.Signature.Parameters = {{"self", Pointer}};
  Hint.Signature.Parameters[0].TheRole =
      SourceParameterTypeHint::Role::SwiftContext;
  std::string Diagnostic;
  ASSERT_TRUE(
      assignDarwinSwiftSourceABI(Hint.Signature, Arch::AArch64, Diagnostic))
      << Diagnostic;

  auto Param = [&](unsigned Id) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = Id;
    V.Size = 8;
    V.TheArch = Arch::AArch64;
    return HighExpr::makeVar(V, Pointer);
  };
  auto Call = HighExpr::makeCall("indirect_call", 0, {Param(0)});
  Call->Type = NdType::makeVoid();
  Call->IsIndirectCall = true;
  Call->IndirectTarget = Param(1);
  Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "call_virtual_action";
  Function.ReturnType = NdType::makeVoid();
  Function.Params = {{"context", Pointer}, {"target", Pointer}};
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = Call;
  Function.Body = {Statement};
  const auto Source = emit({Function}, true, Arch::AArch64);
  EXPECT_NE(Source.find("void __attribute__((swiftcall)) (*)(void* "
                        "__attribute__((swift_context)))"),
            std::string::npos)
      << Source;
  compileAndRun(Source + R"(
static void __attribute__((swiftcall)) action(
    void *self __attribute__((swift_context))) {
  ++*(int *)self;
}
int main(void) {
  int value = 41;
  call_virtual_action(&value, (void *)&action);
  return value == 42 ? 0 : 1;
}
)");
}

TEST(HighCSourceCalls, SwiftVirtualVoidMethodPreservesTwoZeroWords) {
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::SwiftVirtual;
  Hint.TargetName = "swift_virtual";
  Hint.Virtual =
      SourceCallTypeHint::SwiftVirtualEvidence{0x1000, 0x1010, 0x2000, 232, 2};
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Signature.ReturnType = NdType::makeVoid();
  const auto Word = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Hint.Signature.Parameters = {
      {"zero", Word}, {"zero", Word}, {"self", Pointer}};
  Hint.Signature.Parameters.back().TheRole =
      SourceParameterTypeHint::Role::SwiftContext;
  std::string Diagnostic;
  ASSERT_TRUE(
      assignDarwinSwiftSourceABI(Hint.Signature, Arch::AArch64, Diagnostic))
      << Diagnostic;

  auto Param = [&](unsigned Id) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = Id;
    V.Size = 8;
    V.TheArch = Arch::AArch64;
    return HighExpr::makeVar(V, Pointer);
  };
  auto Call = HighExpr::makeCall(
      "indirect_call", 0,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8), Param(0)});
  Call->Type = NdType::makeVoid();
  Call->IsIndirectCall = true;
  Call->IndirectTarget = Param(1);
  Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "call_virtual_nil_action";
  Function.ReturnType = NdType::makeVoid();
  Function.Params = {{"context", Pointer}, {"target", Pointer}};
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = Call;
  Function.Body = {Statement};
  const auto Source = emit({Function}, true, Arch::AArch64);
  EXPECT_NE(Source.find("uint64_t, uint64_t, void* "
                        "__attribute__((swift_context))"),
            std::string::npos)
      << Source;
  compileAndRun(Source + R"(
static void __attribute__((swiftcall)) action(
    uint64_t first, uint64_t second,
    void *self __attribute__((swift_context))) {
  if (first == 0 && second == 0)
    ++*(int *)self;
}
int main(void) {
  int value = 41;
  call_virtual_nil_action(&value, (void *)&action);
  return value == 42 ? 0 : 1;
}
)");
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
    // The public declaration follows the pointer source ABI, even though the
    // call expression still carries the result through a machine integer.
    EXPECT_NE(Source.find("void* copy_value("), std::string::npos) << Source;
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
    void *result = copy_value(&destination, &source, expected_metadata);
    return result == &destination && destination == source ? 0 : 1;
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

TEST(HighCSourceCalls,
     RuntimeCFunctionAddressExecutesWithItsDeclaredPrototype) {
  using namespace runtime_function_address_test;
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    const auto Hint = runtimeCFunctionAddressHint(Image, Slot);
    ASSERT_TRUE(Hint);
    auto Address = HighExpr::makeCall({}, 0, {});
    Address->Type = Hint->Signature.ReturnType;
    Address->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    auto Factory = returning("runtime_callback", Address, {});
    const auto Source = emit({Factory}, true, Architecture);
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    EXPECT_NE(Source.find("&swift_release"), std::string::npos) << Source;
    for (const char *Optimization : {"-O0", "-O2"})
      compileAndRun(Source + R"(
static int observed;
void swift_release(void *value) {
  if (value != (void *)(uintptr_t)0x31) __builtin_trap();
  ++observed;
}
int main(void) {
  void (*callback)(void *) = runtime_callback();
  if (callback != &swift_release) return 1;
  callback((void *)(uintptr_t)0x31);
  return observed != 1;
}
)",
                    {Optimization});
    auto Broken = std::make_shared<SourceCallTypeHint>(*Hint);
    Broken->AddressedFunctionABI->Convention =
        SourceFunctionTypeHint::ConventionKind::Swift;
    Address->SourceCallHint = Broken;
    EXPECT_NE(emit({Factory}, true, Architecture).find("bad source call"),
              std::string::npos);

    Image.ImportPtrSlots[Slot] = Image.DyldBindSlots[Slot].Name = "_malloc";
    Image.DyldBindSlots[Slot].Module = "/usr/lib/libSystem.B.dylib";
    const auto Allocate = runtimeCFunctionAddressHint(Image, Slot);
    ASSERT_TRUE(Allocate);
    Address->Type = Allocate->Signature.ReturnType;
    Address->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Allocate);
    const auto Allocator = returning("runtime_allocator", Address, {});
    const auto AllocateSource = emit({Allocator}, true, Architecture);
    EXPECT_EQ(AllocateSource.find("bad source call"), std::string::npos)
        << AllocateSource;
    EXPECT_NE(AllocateSource.find("&neverd_darwin_malloc"), std::string::npos)
        << AllocateSource;
  }
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
    SCOPED_TRACE(ExplicitElse);
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

namespace {
enum class FloatingCarrierBoundary {
  EntryReturn,
  ExtendedEntryReturn,
  BitCast,
  Argument
};

class FloatingCarrierDebug : public NullDebugContext {
  bool IntegerReturn;

public:
  explicit FloatingCarrierDebug(bool IntegerReturn)
      : IntegerReturn(IntegerReturn) {}
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    if (Address != 0x2004 && Address != 0x2008)
      return std::nullopt;
    const auto Width = Address - 0x2000;
    FunctionSym Function;
    Function.Name = "_carrier_provider_" + std::to_string(Width);
    Function.Addr = Address;
    Function.CallConv = DebugCallConv::Cdecl;
    Function.ReturnType = IntegerReturn ? NdType::makeInt(Width, false)
                                        : NdType::makeFloat(Width);
    Function.Params.emplace_back("value", NdType::makeFloat(Width));
    Function.Params.emplace_back("count",
                                 NdType::makePtr(NdType::makeInt(8, false)));
    return Function;
  }
  bool hasInfo() const override { return true; }
};

std::vector<HighFunc> floatingCarrierFunctions(unsigned Width,
                                               FloatingCarrierBoundary Boundary,
                                               va_t ProviderAddress = 0) {
  const auto Bits = NdType::makeInt(Width, false);
  const auto Float = NdType::makeFloat(Width);
  const auto Count = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(Count);
  const std::string Suffix = std::to_string(Width);
  const std::string Provider = "_carrier_provider_" + Suffix;
  auto Hint = native(Provider, Float, {Float, Pointer});
  Hint.TargetAddress = ProviderAddress;
  auto Value = [&] {
    return call(Hint, Bits, {parameter(0, Bits), parameter(1, Pointer)});
  };
  ExprPtr Result = Value();
  if (Boundary == FloatingCarrierBoundary::ExtendedEntryReturn) {
    auto Extended = std::make_shared<HighExpr>();
    Extended->Kind = ExprKind::UnaryOp;
    Extended->Op = NdOp::INT_SEXT;
    Extended->Type = NdType::makeInt(Width * 2, false);
    Extended->Operands = {Result};
    Result = Extended;
  } else if (Boundary == FloatingCarrierBoundary::BitCast)
    Result = HighExpr::makeBitCast(Result, Float);
  else if (Boundary == FloatingCarrierBoundary::Argument) {
    Result = HighExpr::makeCall("_carrier_sink_" + Suffix, 0, {Result});
    Result->Type = Float;
  }
  auto Subject =
      returning("carrier_subject_" + Suffix, Result, {Bits, Pointer});
  Subject.SourceTypeHint =
      native(Subject.Name, Float, {Bits, Pointer}).Signature;
  auto Raw = returning("carrier_raw_" + Suffix, Value(), {Bits, Pointer});
  auto Cast = std::make_shared<HighExpr>();
  Cast->Kind = ExprKind::Cast;
  Cast->Type = Cast->CastTo = Float;
  Cast->Operands = {Value()};
  auto Numeric = returning("carrier_numeric_" + Suffix, Cast, {Bits, Pointer});
  auto Typed =
      returning("carrier_typed_" + Suffix,
                call(Hint, Float, {parameter(0, Float), parameter(1, Pointer)}),
                {Float, Pointer});
  auto Helper = returning(Provider, parameter(0, Float), {Float, Pointer});
  Helper.Entry = ProviderAddress;
  Helper.SourceTypeHint = Hint.Signature;
  HighStmt Increment;
  Increment.Kind = StmtKind::Store;
  Increment.StoreAddr = parameter(1, Pointer);
  Increment.StoreVal = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeLoad(parameter(1, Pointer), Count),
      HighExpr::makeConst(1, 8));
  Helper.Body.insert(Helper.Body.begin(), Increment);
  auto Sink =
      returning("_carrier_sink_" + Suffix, parameter(0, Float), {Float});
  return {Subject, Raw, Numeric, Typed, Helper, Sink};
}

void checkFloatingCarrierBoundary(FloatingCarrierBoundary Boundary) {
  const std::string Harness = R"(
int main(void) {
  const uint32_t a[] = {0,0x80000000U,1,0x007fffffU,0x00800000U,
    0x3fa00000U,0xbfa00000U,0x7fc00042U,0xffc00042U,0x7f800000U,0xff800000U};
  const uint64_t b[] = {0,0x8000000000000000ULL,1,0x000fffffffffffffULL,
    0x0010000000000000ULL,0x3ff4000000000000ULL,0xbff4000000000000ULL,
    0x7ff8000000000042ULL,0xfff8000000000042ULL,0x7ff0000000000000ULL,
    0xfff0000000000000ULL};
  for (unsigned i=0; i<sizeof(a)/sizeof(a[0]); ++i) {
    struct { uint64_t head, calls, tail; } s = {0x1234,0,0x5678};
    uint32_t bits;
    float value = carrier_subject_4(a[i],&s.calls);
    __builtin_memcpy(&bits,&value,4);
    if (bits!=a[i] || s.calls!=1) return 1;
    if (carrier_raw_4(a[i],&s.calls)!=a[i] || s.calls!=2) return 2;
    if (carrier_numeric_4(a[i],&s.calls)!=(float)a[i] || s.calls!=3) return 3;
    __builtin_memcpy(&value,&a[i],4);
    value = carrier_typed_4(value,&s.calls);
    __builtin_memcpy(&bits,&value,4);
    if (bits!=a[i] || s.calls!=4 || s.head!=0x1234 || s.tail!=0x5678) return 4;
  }
  for (unsigned i=0; i<sizeof(b)/sizeof(b[0]); ++i) {
    struct { uint64_t head, calls, tail; } s = {0x1234,0,0x5678};
    uint64_t bits;
    double value = carrier_subject_8(b[i],&s.calls);
    __builtin_memcpy(&bits,&value,8);
    if (bits!=b[i] || s.calls!=1) return 5;
    if (carrier_raw_8(b[i],&s.calls)!=b[i] || s.calls!=2) return 6;
    if (carrier_numeric_8(b[i],&s.calls)!=(double)b[i] || s.calls!=3) return 7;
    __builtin_memcpy(&value,&b[i],8);
    value = carrier_typed_8(value,&s.calls);
    __builtin_memcpy(&bits,&value,8);
    if (bits!=b[i] || s.calls!=4 || s.head!=0x1234 || s.tail!=0x5678) return 8;
  }
  return 0;
})";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    auto Functions = floatingCarrierFunctions(4, Boundary);
    const auto Double = floatingCarrierFunctions(8, Boundary);
    Functions.insert(Functions.end(), Double.begin(), Double.end());
    const auto Source = emit(Functions, true, Architecture);
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    ASSERT_NO_FATAL_FAILURE(compileAndRun(Source + Harness));
  }
}
} // namespace

TEST(HighCSourceCalls, SourceBoundFloatingReturnsPreserveBitsAndEffects) {
  checkFloatingCarrierBoundary(FloatingCarrierBoundary::EntryReturn);
}

TEST(HighCSourceCalls,
     SourceBoundFloatingExtendedReturnsPreserveBitsAndEffects) {
  checkFloatingCarrierBoundary(FloatingCarrierBoundary::ExtendedEntryReturn);
}

TEST(HighCSourceCalls, SourceBoundFloatingBitCastsPreserveBitsAndEffects) {
  checkFloatingCarrierBoundary(FloatingCarrierBoundary::BitCast);
}

TEST(HighCSourceCalls, SourceBoundFloatingArgumentsPreserveBitsAndEffects) {
  checkFloatingCarrierBoundary(FloatingCarrierBoundary::Argument);
}

TEST(HighCSourceCalls, FloatingArgumentsPreserveBitCastsAndNumericConversions) {
  FloatingCarrierDebug Debug(false);
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    for (bool HasDebug : {false, true}) {
      SCOPED_TRACE(HasDebug);
      std::vector<HighFunc> Functions;
      for (unsigned Width : {4, 8}) {
        const auto Bits = NdType::makeInt(Width, false);
        const auto Float = NdType::makeFloat(Width);
        const auto Pointer = NdType::makePtr(NdType::makeInt(8, false));
        const auto Address = 0x2000 + Width;
        auto Helpers = floatingCarrierFunctions(
            Width, FloatingCarrierBoundary::EntryReturn, Address);
        Functions.push_back(Helpers[4]);
        for (bool Numeric : {false, true}) {
          auto Value = HighExpr::makeBitCast(parameter(0, Bits), Float);
          if (Numeric) {
            Value->Kind = ExprKind::Cast;
            Value->CastTo = Float;
          }
          auto Call = HighExpr::makeCall(Helpers[4].Name, Address,
                                         {Value, parameter(1, Pointer)});
          Call->Type = Float;
          Functions.push_back(
              returning(std::string(Numeric ? "numeric_arg_" : "bit_arg_") +
                            std::to_string(Width),
                        Call, {Bits, Pointer}));
        }
      }
      compileAndRun(emit(Functions, true, Architecture,
                         CEmitterOptions::ScalarPointerSpelling::StandardTypes,
                         HasDebug ? &Debug : nullptr) +
                    R"(
int main(void) {
  const uint32_t a[] = {0,0x80000000U,1,0x007fffffU,0x00800000U,
    0x3fa00000U,0xbfa00000U,0x7fc00042U,0xffc00042U,0x7f800000U,0xff800000U};
  const uint64_t b[] = {0,0x8000000000000000ULL,1,0x000fffffffffffffULL,
    0x0010000000000000ULL,0x3ff4000000000000ULL,0xbff4000000000000ULL,
    0x7ff8000000000042ULL,0xfff8000000000042ULL,0x7ff0000000000000ULL,
    0xfff0000000000000ULL};
  for (unsigned i=0; i<sizeof(a)/sizeof(a[0]); ++i) {
    struct { uint64_t head, calls, tail; } s = {0x1234,0,0x5678};
    float value=bit_arg_4(a[i],&s.calls);
    uint32_t bits; __builtin_memcpy(&bits,&value,4);
    if(bits!=a[i] || s.calls!=1) return 1;
    if(numeric_arg_4(a[i],&s.calls)!=(float)a[i] || s.calls!=2) return 2;
    if(s.head!=0x1234 || s.tail!=0x5678) return 3;
  }
  for (unsigned i=0; i<sizeof(b)/sizeof(b[0]); ++i) {
    struct { uint64_t head, calls, tail; } s = {0x1234,0,0x5678};
    double value=bit_arg_8(b[i],&s.calls);
    uint64_t bits; __builtin_memcpy(&bits,&value,8);
    if(bits!=b[i] || s.calls!=1) return 4;
    if(numeric_arg_8(b[i],&s.calls)!=(double)b[i] || s.calls!=2) return 5;
    if(s.head!=0x1234 || s.tail!=0x5678) return 6;
  }
  return 0;
})");
    }
  }
}

TEST(HighCSourceCalls,
     SourceBoundFloatingDeclarationsOverrideConflictingDebugReturns) {
  FloatingCarrierDebug Debug(true);
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    std::vector<HighFunc> Functions;
    for (unsigned Width : {4, 8}) {
      const auto Bits = NdType::makeInt(Width, false);
      const auto Float = NdType::makeFloat(Width);
      const auto Pointer = NdType::makePtr(NdType::makeInt(8, false));
      const auto Address = 0x2000 + Width;
      auto Typed = floatingCarrierFunctions(
          Width, FloatingCarrierBoundary::ExtendedEntryReturn, Address);
      Functions.insert(Functions.end(), Typed.begin(), Typed.end());
      auto Ordinary = HighExpr::makeCall(
          "_carrier_provider_" + std::to_string(Width), Address,
          {HighExpr::makeBitCast(parameter(0, Bits), Float),
           parameter(1, Pointer)});
      Ordinary->Type = Bits;
      Functions.push_back(returning("carrier_ordinary_" + std::to_string(Width),
                                    Ordinary, {Bits, Pointer}));
    }
    const auto Source =
        emit(Functions, true, Architecture,
             CEmitterOptions::ScalarPointerSpelling::StandardTypes, &Debug);
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    compileAndRun(Source + R"(
int main(void) {
  const uint32_t a[] = {0,0x80000000U,1,0x007fffffU,0x00800000U,
    0x3fa00000U,0xbfa00000U,0x7fc00042U,0xffc00042U,0x7f800000U,0xff800000U};
  const uint64_t b[] = {0,0x8000000000000000ULL,1,0x000fffffffffffffULL,
    0x0010000000000000ULL,0x3ff4000000000000ULL,0xbff4000000000000ULL,
    0x7ff8000000000042ULL,0xfff8000000000042ULL,0x7ff0000000000000ULL,
    0xfff0000000000000ULL};
  for (unsigned i=0; i<sizeof(a)/sizeof(a[0]); ++i) {
    struct { uint64_t head, calls, tail; } s = {0x1234,0,0x5678};
    float value = carrier_subject_4(a[i],&s.calls);
    uint32_t bits; __builtin_memcpy(&bits,&value,4);
    if(bits!=a[i] || s.calls!=1) return 1;
    if(carrier_ordinary_4(a[i],&s.calls)!=a[i] || s.calls!=2) return 2;
    if(s.head!=0x1234 || s.tail!=0x5678) return 3;
  }
  for (unsigned i=0; i<sizeof(b)/sizeof(b[0]); ++i) {
    struct { uint64_t head, calls, tail; } s = {0x1234,0,0x5678};
    double value = carrier_subject_8(b[i],&s.calls);
    uint64_t bits; __builtin_memcpy(&bits,&value,8);
    if(bits!=b[i] || s.calls!=1) return 4;
    if(carrier_ordinary_8(b[i],&s.calls)!=b[i] || s.calls!=2) return 5;
    if(s.head!=0x1234 || s.tail!=0x5678) return 6;
  }
  return 0;
})");
  }
}

TEST(HighCSourceCalls,
     SourceBoundFloatingResultsRejectInvalidCarriersAndTypes) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    for (unsigned Width : {4, 8}) {
      SCOPED_TRACE(Width);
      const auto Float = NdType::makeFloat(Width);
      const auto Bits = NdType::makeInt(Width, false);
      const auto Hint = native("_carrier_checked", Float, {});
      const auto Provider = returning(
          Hint.TargetName,
          HighExpr::makeBitCast(HighExpr::makeConst(0, Width), Float));
      for (const auto &Carrier :
           {TypeRef{}, NdType::makeInt(Width == 4 ? 8 : 4, false),
            NdType::makePtr(NdType::makeVoid())}) {
        auto Subject = returning("carrier_invalid", call(Hint, Carrier));
        Subject.ReturnType = Float;
        Subject.SourceTypeHint = native(Subject.Name, Float, {}).Signature;
        EXPECT_NE(
            emit({Subject, Provider}, true, Architecture)
                .find("result carrier disagrees with the source declaration"),
            std::string::npos);
      }
      auto First = returning("carrier_first", call(Hint, Bits));
      First.SourceTypeHint = native(First.Name, Float, {}).Signature;
      auto Other = Hint;
      Other.Signature.ReturnType = NdType::makeFloat(Width == 4 ? 8 : 4);
      auto Second = returning("carrier_second", call(Other, Bits));
      Second.SourceTypeHint = native(Second.Name, Float, {}).Signature;
      EXPECT_NE(emit({First, Second, Provider}, true, Architecture)
                    .find("conflicting native declarations"),
                std::string::npos);
    }
  }
}

TEST(HighCSourceCalls, FloatingCallCarriersRemainBitsAtSourceBoundaries) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    std::vector<HighFunc> Functions;
    for (unsigned Width : {4U, 8U}) {
      auto Bits = NdType::makeInt(Width, false);
      auto Float = NdType::makeFloat(Width);
      const std::string Suffix = std::to_string(Width);
      const std::string Helper = "_helper_" + Suffix;
      Functions.push_back(returning(Helper, parameter(0, Float), {Float}));
      for (unsigned Boundary = 0; Boundary != 3; ++Boundary) {
        auto Value =
            call(native(Helper, Float, {Float}), Bits, {parameter(0, Bits)});
        if (Boundary == 1) {
          auto Cast = std::make_shared<HighExpr>();
          Cast->Kind = ExprKind::BitCast;
          Cast->Type = Float;
          Cast->Operands = {Value};
          Value = std::move(Cast);
        } else if (Boundary == 2) {
          Value = HighExpr::makeCall(Helper, 0x2000, {Value});
          Value->Type = Float;
        }
        auto Function =
            returning("forward_" + Suffix + "_" + std::to_string(Boundary),
                      Value, {Bits});
        Function.ReturnType = Float;
        Functions.push_back(std::move(Function));
      }
    }
    const auto Source = emit(Functions, true, Architecture) + R"(
int main(void) {
  const uint32_t a[] = {0,0x80000000U,1,0x3fa00000U,0xc1240000U,0x7fc00042U,0x7f800000U};
  const uint64_t b[] = {0,0x8000000000000000ULL,1,0x3ff4000000000000ULL,
                        0xc024800000000000ULL,0x7ff8000000000042ULL,0x7ff0000000000000ULL};
  for (unsigned i=0; i<sizeof(a)/sizeof(a[0]); ++i)
    if (__builtin_bit_cast(uint32_t, forward_4_0(a[i])) != a[i] ||
        __builtin_bit_cast(uint32_t, forward_4_1(a[i])) != a[i] ||
        __builtin_bit_cast(uint32_t, forward_4_2(a[i])) != a[i]) return 1;
  for (unsigned i=0; i<sizeof(b)/sizeof(b[0]); ++i)
    if (__builtin_bit_cast(uint64_t, forward_8_0(b[i])) != b[i] ||
        __builtin_bit_cast(uint64_t, forward_8_1(b[i])) != b[i] ||
        __builtin_bit_cast(uint64_t, forward_8_2(b[i])) != b[i]) return 2;
  return 0;
})";
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      ASSERT_NO_FATAL_FAILURE(compileAndRun(Source, {Optimization}));
    }
  }
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

TEST(HighCSourceCalls, ScrollInsetsPreserveAllFourIndependentValues) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  const auto Signature = objcSelectorSourceTypeHint(Image, "setContentInset:");
  ASSERT_TRUE(Signature);
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.TargetName = "objc_msgSend";
  Hint.Selector = "setContentInset:";
  Hint.Signature = *Signature;
  const auto Word = NdType::makeInt(8, false);
  const auto Double = NdType::makeFloat(8);
  std::vector<ExprPtr> Leaves;
  HighFunc Function;
  Function.Name = "set_insets";
  Function.ReturnType = NdType::makeVoid();
  Function.Params = {{"receiver", Word}, {"selector", Word}};
  for (unsigned I = 0; I != 4; ++I) {
    Function.Params.push_back({"edge" + std::to_string(I), Double});
    Leaves.push_back(parameter(I + 2, Double));
  }
  HighStmt Statement;
  Statement.Kind = StmtKind::ExprStmt;
  Statement.Val =
      call(Hint, Function.ReturnType,
           {parameter(0, Word), parameter(1, Word),
            HighExpr::makeRecord(Signature->Parameters.back().Type, Leaves)});
  Function.Body = {Statement};
  const auto Source = emit({Function}, false, Arch::AArch64);
  const auto Program = R"(
#include <stdint.h>
#include <string.h>
typedef void *id;
typedef void *SEL;
typedef struct { double top, left, bottom, right; } InsetsOracle;
static InsetsOracle observed;
static unsigned calls;
static void implementation(id receiver, SEL selector, InsetsOracle insets) {
  if (receiver != (id)(uintptr_t)0x1234 ||
      selector != (SEL)(uintptr_t)0x5678) __builtin_trap();
  observed = insets;
  ++calls;
}
static void (*objc_msgSend)(id, SEL, InsetsOracle) = implementation;
)" + Source + R"(
int main(void) {
  const uint64_t patterns[][4] = {
    {0, 0, 0x4024000000000000ULL, 0},
    {0x3ff4000000000000ULL, 0xc004000000000000ULL,
     0x8000000000000000ULL, 0xc032c00000000000ULL},
    {1, 0x7ff8000000000042ULL, 0xfff0000000000000ULL,
     0x3fe0000000000000ULL}
  };
  _Static_assert(sizeof(InsetsOracle) == 32, "four CGFloat fields");
  for (unsigned i = 0; i != 3; ++i) {
    InsetsOracle input;
    memcpy(&input, patterns[i], sizeof(input));
    set_insets(0x1234, 0x5678, input.top, input.left, input.bottom, input.right);
    if (memcmp(&observed, patterns[i], sizeof(observed)) || calls != i + 1)
      return 1;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    compileAndRun(Program, {Optimization});
  }
}

TEST(HighCSourceCalls, SafeAreaResultPreservesEveryFieldAndSingleEvaluation) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  const auto Signature = objcSelectorSourceTypeHint(Image, "safeAreaInsets");
  ASSERT_TRUE(Signature);
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.TargetName = "objc_msgSend";
  Hint.Selector = "safeAreaInsets";
  Hint.Signature = *Signature;
  const auto Word = NdType::makeInt(8, false);
  std::vector<HighFunc> Functions;
  for (unsigned I = 0; I != 4; ++I) {
    auto Result = call(Hint, Signature->ReturnType,
                       {parameter(0, Word), parameter(1, Word)});
    Functions.push_back(returning("read_edge" + std::to_string(I),
                                  HighExpr::makeRecordField(Result, I * 8, 8),
                                  {Word, Word}));
  }
  const auto Source = emit(Functions, false, Arch::AArch64);
  const auto Program = R"(
#include <stdint.h>
#include <string.h>
typedef void *id;
typedef void *SEL;
typedef struct { double top, left, bottom, right; } InsetsOracle;
static unsigned calls;
static const uint64_t expected[] = {
  0x3ff4000000000000ULL, 0xc004000000000000ULL,
  0x8000000000000000ULL, 0x7ff8000000000042ULL
};
static InsetsOracle implementation(id receiver, SEL selector) {
  if (selector != (SEL)(uintptr_t)0x5678) __builtin_trap();
  ++calls;
  InsetsOracle result = {0, 0, 0, 0};
  if (receiver) {
    if (receiver != (id)(uintptr_t)0x1234) __builtin_trap();
    memcpy(&result, expected, sizeof(result));
  }
  return result;
}
static InsetsOracle (*objc_msgSend)(id, SEL) = implementation;
)" + Source + R"(
int main(void) {
  double (*readers[])(uint64_t, uint64_t) = {
    read_edge0, read_edge1, read_edge2, read_edge3
  };
  for (unsigned i = 0; i != 4; ++i) {
    double value = readers[i](0x1234, 0x5678);
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits != expected[i] || calls != 2 * i + 1) return 1;
    value = readers[i](0, 0x5678);
    memcpy(&bits, &value, sizeof(bits));
    if (bits != 0 || calls != 2 * i + 2) return 2;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    compileAndRun(Program, {Optimization});
  }
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

TEST(HighCSourceCalls, ForwardedCallResultsKeepInterveningCallOrder) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Retain = native("objc_retain", Pointer, {Pointer});
  Retain.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Retain.TargetAddress = 0xdeadbeef;
  auto Release = native("objc_release", NdType::makeVoid(), {Pointer});
  Release.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Release.TargetAddress = 0xdeadbef7;
  MedVar First;
  First.Kind = MedVar::Temp;
  First.Id = 1;
  First.Size = 8;
  First.TheArch = Arch::X64;
  MedVar Second = First;
  Second.Id = 2;
  HighFunc Function = returning(
      "ordered_calls", HighExpr::makeVar(Second, Pointer), {Pointer, Pointer});
  HighStmt FirstCall;
  FirstCall.Kind = StmtKind::Assign;
  FirstCall.Addr = 0x1000;
  FirstCall.Dst = HighExpr::makeVar(First, Pointer);
  FirstCall.Val = call(Retain, Pointer, {parameter(0, Pointer)});
  HighStmt SecondCall = FirstCall;
  SecondCall.Addr = 0x1004;
  SecondCall.Dst = HighExpr::makeVar(Second, Pointer);
  SecondCall.Val = call(Retain, Pointer, {parameter(1, Pointer)});
  HighStmt FinalCall;
  FinalCall.Kind = StmtKind::Call;
  FinalCall.Addr = 0x1008;
  FinalCall.CallExpr =
      call(Release, NdType::makeVoid(), {HighExpr::makeVar(First, Pointer)});
  Function.Body.insert(Function.Body.begin(),
                       {FirstCall, SecondCall, FinalCall});
  const auto Source = emit({Function});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(Source + R"(
static unsigned sequence;
void *objc_retain(void *value) {
  sequence = sequence * 10 + (unsigned)(uintptr_t)value;
  return value;
}
void objc_release(void *value) {
  sequence = sequence * 10 + (value == (void *)(uintptr_t)1 ? 3 : 9);
}
int main(void) {
  void *result = ordered_calls((void *)(uintptr_t)1,
                               (void *)(uintptr_t)2);
  return result != (void *)(uintptr_t)2 || sequence != 123;
})");
}

TEST(HighCSourceCalls, CallResultReturnedNextIsReturnedDirectly) {
  // Returning a call result from the very next statement moves no
  // evaluation, so a tail call prints as `return f(x);`. A store between the
  // call and its return keeps the call at its own statement.
  auto Int = NdType::makeInt(8, true);
  auto Pointer = NdType::makePtr(Int);
  auto Next = native("next_value", Int, {Int});
  Next.TargetAddress = 0x2000;
  MedVar Result;
  Result.Kind = MedVar::Temp;
  Result.Id = 1;
  Result.Size = 8;
  Result.TheArch = Arch::X64;
  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Addr = 0x1000;
  Call.Dst = HighExpr::makeVar(Result, Int);
  Call.Val = call(Next, Int, {parameter(0, Int)});
  HighFunc Tail =
      returning("tail_value", HighExpr::makeVar(Result, Int), {Int});
  Tail.Entry = 0x1000;
  Tail.Body.insert(Tail.Body.begin(), Call);
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.Addr = 0x1104;
  Store.StoreAddr = parameter(1, Pointer);
  Store.StoreVal = parameter(0, Int);
  Call.Addr = 0x1100;
  HighFunc Stored =
      returning("stored_value", HighExpr::makeVar(Result, Int), {Int, Pointer});
  Stored.Entry = 0x1100;
  Stored.Body.insert(Stored.Body.begin(), {Call, Store});
  const auto Source = emit({Tail, Stored});
  const size_t TailAt = Source.find("int64_t tail_value(");
  const size_t StoredAt = Source.find("int64_t stored_value(");
  ASSERT_NE(TailAt, std::string::npos) << Source;
  ASSERT_NE(StoredAt, std::string::npos) << Source;
  const std::string TailBody = Source.substr(TailAt, StoredAt - TailAt);
  const std::string StoredBody = Source.substr(StoredAt);
  EXPECT_NE(TailBody.find("return (int64_t)(next_value("), std::string::npos)
      << Source;
  EXPECT_EQ(TailBody.find("= (int64_t)(next_value("), std::string::npos)
      << Source;
  EXPECT_NE(StoredBody.find("= (int64_t)(next_value("), std::string::npos)
      << Source;
  EXPECT_EQ(StoredBody.find("return (int64_t)(next_value("), std::string::npos)
      << Source;
  compileAndRun(Source + R"(
static int64_t cell = 100;
int64_t next_value(int64_t value) { return value + cell; }
int main(void) {
  if (tail_value(1) != 101)
    return 1;
  return stored_value(5, &cell) != 105 || cell != 5;
})");
}

TEST(HighCSourceCalls, RuntimeImportsDoNotBindToLiftedVeneersWithTheSameName) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Runtime = native("objc_opt_self", Pointer, {Pointer});
  Runtime.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Runtime.TargetAddress = 0xc772f8;
  auto Veneer =
      returning("_objc_opt_self",
                call(Runtime, Pointer, {parameter(0, Pointer)}), {Pointer});
  Veneer.Entry = 0x915d3c;

  const auto Source = emit({Veneer});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern void* objc_opt_self(void*);"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("void* objc_opt_self_2(void* arg0)"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("objc_opt_self((void*)(uintptr_t)("), std::string::npos)
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

TEST(HighCSourceCalls, DarwinAliasLinkNamesDoNotBindToLiftedVeneers) {
  const auto Double = NdType::makeFloat(8);
  const auto Pair = NdType::makeStruct({Double, Double});
  for (const std::string Name : {"neverd_test_hfa", "__neverd_test_hfa"}) {
    SCOPED_TRACE(Name);
    auto Runtime = native(Name, Pair, {Double});
    Runtime.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Runtime.TargetAddress = 0x2080;
    Runtime.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    std::string Error;
    ASSERT_TRUE(
        assignDarwinFixedSourceABI(Runtime.Signature, Arch::AArch64, Error));
    auto Veneer = returning(
        "_" + Name, call(Runtime, Pair, {parameter(0, Double)}), {Double});
    Veneer.Entry = 0x1000;
    Veneer.SourceTypeHint = Runtime.Signature;
    auto Local = native("_" + Name, Pair, {Double});
    Local.TargetAddress = Veneer.Entry;
    Local.Signature = Runtime.Signature;
    Local.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    auto Forward = returning(
        "forward_hfa", call(Local, Pair, {parameter(0, Double)}), {Double});
    Forward.Entry = 0x1100;
    const auto Source = emit({Forward, Veneer}, true, Arch::AArch64);
    ASSERT_NE(Source.find(Name + "_2(double arg0)"), std::string::npos)
        << Source;
    const auto Driver =
        "\nstatic unsigned calls;\n"
        "struct nd_record_r2_d_d_e provider(double) __asm__(\"_" +
        Name +
        "\");\nstruct nd_record_r2_d_d_e provider(double x) {\n"
        "  ++calls; return (struct nd_record_r2_d_d_e){x + 3, x - 7};\n}\n"
        "int main(void) {\n"
        "  for (int i = -128; i <= 128; ++i) {\n"
        "    struct nd_record_r2_d_d_e p = forward_hfa(i);\n"
        "    if (p.field_0 != i + 3 || p.field_1 != i - 7) return 1;\n"
        "  }\n return calls != 257;\n}\n";
    for (const auto Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Driver, {Optimization});
  }
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

TEST(HighCSourceCalls, PreparedImageFunctionNamesPreserveEmission) {
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto I64 = NdType::makeInt(8, false);
  auto Call = HighExpr::makeCall("_strlen", 0x4000, {parameter(0, Pointer)});
  Call->Type = I64;
  auto Caller = returning("length_of_string", Call, {Pointer});
  Caller.Entry = 0x1000;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::ELF;
  Image.Symbols.push_back({"_strlen", 0x4000, 0, true});
  const auto Emit = [&](HighCEmitter &Emitter, const BinaryImage &Current) {
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::ELF;
    Options.Image = &Current;
    EXPECT_TRUE(Emitter.emit({Caller}, OS, Options));
    return Source;
  };
  HighCEmitter Plain;
  HighCEmitter Prepared;
  Prepared.prepareImageFunctionNames(Image);
  const auto Source = Emit(Prepared, Image);
  EXPECT_EQ(Source, Emit(Plain, Image));
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_NE(Source.find("strlen("), std::string::npos) << Source;

  BinaryImage Other;
  Other.Arch = Arch::X64;
  Other.Format = BinaryFormat::ELF;
  EXPECT_EQ(Emit(Prepared, Other), Emit(Plain, Other));
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
  ASSERT_TRUE(assignDarwinVariadicSourceABI(PointerTail.Signature, 3, Arch::X64,
                                            Error));
  const auto PointerTailSource =
      emit({returning("send_dynamic_pointer_format",
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
  EXPECT_NE(emit({returning("bad_conflicting_dynamic_format",
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

TEST(HighCSourceCalls, CompilerRTPlatformCheckKeepsItsExactLinkNameAndABI) {
  const auto I32 = NdType::makeInt(4, true);
  const auto U32 = NdType::makeInt(4, false);
  auto Hint = native("__isPlatformVersionAtLeast", I32, {U32, U32, U32, U32});
  Hint.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Hint.Signature, Arch::X64, Error));
  auto Function = returning("check_platform",
                            call(Hint, I32,
                                 {parameter(0, U32), parameter(1, U32),
                                  parameter(2, U32), parameter(3, U32)}),
                            {U32, U32, U32, U32});
  const auto Source = emit({Function});
  EXPECT_NE(
      Source.find("extern int32_t neverd_darwin___isPlatformVersionAtLeast("),
      std::string::npos)
      << Source;
  EXPECT_NE(Source.find("__asm__(\"___isPlatformVersionAtLeast\")"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int32_t platform_check(uint32_t platform, uint32_t major, uint32_t minor,
                       uint32_t subminor)
    __asm__("___isPlatformVersionAtLeast");
int32_t platform_check(uint32_t platform, uint32_t major, uint32_t minor,
                       uint32_t subminor) {
  return (int32_t)(platform + major * 10 + minor * 100 + subminor * 1000);
}
int main(void) { return check_platform(2, 14, 6, 3) != 3742; }
)");
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
  Function.SourceTypeHint = native(Function.Name, I64, {BytePointer}).Signature;
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
  EXPECT_THROW(typeToC(NdType::makeInt(65)), std::invalid_argument);
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
      ASSERT_TRUE(
          assignDarwinFixedSourceABI(Bad.Signature, Arch::AArch64, Error));
      ASSERT_TRUE(validateSourceABI(Bad.Signature, Error));
    }
    EXPECT_NE(Render(Bad).find("bad source call"), std::string::npos);
  }
}

namespace {
HighFunc
booleanSourceFunction(llvm::StringRef Import = SwiftBooleanComparisonImport) {
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::SwiftBooleanProjection;
  Hint.Signature = *swiftBooleanNormalizedSignature(Import);
  Hint.TargetName = Import.drop_front().str();
  Hint.TargetAddress = 0x2080;
  Hint.BooleanResult = SourceCallTypeHint::BooleanResultProjection{
      0x1000, {0x101c, 0, NdOp::CALL, 0x1100}};
  std::vector<ExprPtr> Args;
  std::vector<TypeRef> Types;
  for (const auto &P : Hint.Signature.Parameters) {
    Args.push_back(parameter(Args.size(), P.Type));
    Types.push_back(P.Type);
  }
  auto E = call(Hint, Hint.Signature.ReturnType, Args);
  E->CallAddr = 0x1100;
  auto Function = returning("projected_bool", E, Types);
  Function.Entry = 0x1000;
  return Function;
}
} // namespace

TEST(HighCSourceCalls, SwiftBooleanUsesTrueI1DeclarationAndPreservesArguments) {
  const auto Source = emit({booleanSourceFunction()}, true, Arch::AArch64);
  EXPECT_NE(Source.find("extern _Bool neverd_swift_string_compare_bool"),
            std::string::npos);
  EXPECT_NE(Source.find("swiftcall"), std::string::npos);
  EXPECT_NE(Source.find("((uint8_t)neverd_swift_string_compare_bool("),
            std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  const auto Program = Source + R"(
static unsigned calls;
_Bool __attribute__((swiftcall)) neverd_swift_string_compare_bool(
    uint64_t a, void *b, uint64_t c, void *d, uint8_t e) {
  ++calls;
  return a == 5 && b == (void *)0x1234 && c == 7 && d == (void *)0x5678 && e == 255;
}
int main(void) {
  if (projected_bool(5, (void *)0x1234, 7, (void *)0x5678, 255) != 1 || calls != 1) return 1;
  if (projected_bool(6, (void *)0x1234, 7, (void *)0x5678, 255) != 0 || calls != 2) return 2;
  return 0;
}
)";
  compileAndRun(Program, {"-O0", "-Werror"});
  compileAndRun(Program, {"-O2", "-Werror"});
}

TEST(HighCSourceCalls, SwiftPrefixKeepsItsOwnFourArgumentI1Prototype) {
  const auto Source = emit({booleanSourceFunction(SwiftBooleanPrefixImport)},
                           true, Arch::AArch64);
  EXPECT_NE(
      Source.find("extern _Bool neverd_swift_string_has_prefix_bool(uint64_t, "
                  "void*, uint64_t, void*)"),
      std::string::npos);
  EXPECT_NE(Source.find("__asm__(\"_$sSS9hasPrefixySbSSF\")"),
            std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  EXPECT_EQ(Source.find("neverd_swift_string_compare_bool"), std::string::npos);
  const auto Program = Source + R"(
static unsigned calls;
_Bool __attribute__((swiftcall)) neverd_swift_string_has_prefix_bool(
    uint64_t a, void *b, uint64_t c, void *d) {
  ++calls;
  return a == 5 && b == (void *)0x1234 && c == 7 && d == (void *)0x5678;
}
int main(void) {
  if (projected_bool(5, (void *)0x1234, 7, (void *)0x5678) != 1 || calls != 1) return 1;
  if (projected_bool(6, (void *)0x1234, 7, (void *)0x5678) != 0 || calls != 2) return 2;
  return 0;
}
)";
  compileAndRun(Program, {"-O0", "-Werror"});
  compileAndRun(Program, {"-O2", "-Werror"});
}

TEST(HighCSourceCalls, SwiftSuffixKeepsItsOwnFourArgumentI1Prototype) {
  const auto Source = emit({booleanSourceFunction(SwiftBooleanSuffixImport)},
                           true, Arch::AArch64);
  EXPECT_NE(
      Source.find("extern _Bool neverd_swift_string_has_suffix_bool(uint64_t, "
                  "void*, uint64_t, void*)"),
      std::string::npos);
  EXPECT_NE(Source.find("__asm__(\"_$sSS9hasSuffixySbSSF\")"),
            std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  const auto Program = Source + R"(
static unsigned calls;
_Bool __attribute__((swiftcall)) neverd_swift_string_has_suffix_bool(
    uint64_t a, void *b, uint64_t c, void *d) {
  ++calls;
  return a == 5 && b == (void *)0x1234 && c == 7 && d == (void *)0x5678;
}
int main(void) {
  if (projected_bool(5, (void *)0x1234, 7, (void *)0x5678) != 1 || calls != 1) return 1;
  if (projected_bool(6, (void *)0x1234, 7, (void *)0x5678) != 0 || calls != 2) return 2;
  return 0;
}
)";
  compileAndRun(Program, {"-O0", "-Werror"});
  compileAndRun(Program, {"-O2", "-Werror"});
}

TEST(HighCSourceCalls, SwiftObjectEqualityPreservesHiddenContextAndI1Result) {
  const auto Source =
      emit({booleanSourceFunction(SwiftBooleanObjectEqualityImport)}, true,
           Arch::AArch64);
  EXPECT_NE(Source.find("extern _Bool neverd_swift_nsobject_equal_bool"),
            std::string::npos);
  EXPECT_NE(Source.find("swift_context"), std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  const auto Program = Source + R"(
static unsigned calls;
_Bool __attribute__((swiftcall)) neverd_swift_nsobject_equal_bool(
    void *a, void *b, void *metadata __attribute__((swift_context))) {
  ++calls;
  return a == (void *)0x1234 && b == (void *)0x5678 && metadata == (void *)0x9abc;
}
int main(void) {
  if (projected_bool((void *)0x1234, (void *)0x5678, (void *)0x9abc) != 1 || calls != 1) return 1;
  if (projected_bool((void *)0x1234, (void *)0x5678, (void *)0x9abd) != 0 || calls != 2) return 2;
  return 0;
}
)";
  compileAndRun(Program, {"-O0", "-Werror"});
  compileAndRun(Program, {"-O2", "-Werror"});
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    auto Function = booleanSourceFunction(SwiftBooleanObjectEqualityImport);
    auto E = Function.Body[0].RetVal;
    auto Hint = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
    E->SourceCallHint = Hint;
    if (Mutation == 0)
      Hint->Signature.Parameters.back().TheRole =
          SourceParameterTypeHint::Role::Ordinary;
    else
      Hint->Signature.Parameters.back().Location.RegisterOffset = 16;
    EXPECT_NE(emit({Function}, false, Arch::AArch64).find("bad source call"),
              std::string::npos);
  }
}

TEST(HighCSourceCalls, SwiftBooleanRejectsForeignBindingsAndBytePrototypes) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = booleanSourceFunction();
    auto E = Function.Body[0].RetVal;
    auto Hint = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
    E->SourceCallHint = Hint;
    switch (Mutation) {
    case 0:
      Hint->BooleanResult.reset();
      break;
    case 1:
      Hint->BooleanResult->FunctionEntry += 4;
      break;
    case 2:
      Hint->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
      break;
    case 3:
      Hint->Signature.ReturnType = NdType::makeInt(8, false);
      break;
    case 4:
      E->IsIndirectCall = true;
      break;
    case 5:
      E->CallAddr += 4;
      break;
    case 6:
      Hint->ValueWitness = SourceCallTypeHint::SwiftValueWitnessKind::Destroy;
      break;
    case 7:
      Hint->SwiftStringInputs = {{0, 1}};
      break;
    }
    EXPECT_NE(emit({Function}, false, Arch::AArch64).find("bad source call"),
              std::string::npos);
  }
  auto Function = booleanSourceFunction();
  auto E = Function.Body[0].RetVal;
  auto Byte = std::make_shared<HighExpr>(*E);
  auto Hint = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
  Hint->BooleanResult.reset();
  Hint->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
  Byte->SourceCallHint = Hint;
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = Byte;
  Function.Body.insert(Function.Body.begin(), Statement);
  EXPECT_NE(emit({Function}, false, Arch::AArch64)
                .find("conflicting Swift Boolean runtime declaration"),
            std::string::npos);
}

TEST(HighCSourceCalls, SwiftCancellableCallsPreserveEveryCarrierAndOccurrence) {
#if defined(__aarch64__) || defined(_M_ARM64)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  std::vector<HighFunc> Functions;
  for (const bool Sink : {false, true}) {
    auto Image = runtime_function_address_test::image(Architecture);
    const va_t Slot = runtime_function_address_test::Slot;
    const std::string Name =
        Sink ? "_$"
               "s7Combine9PublisherPAAs5NeverO7FailureRtzrlE4sink12receiveValue"
               "AA14AnyCancellableCy6OutputQzc_tF"
             : "_$s7Combine14AnyCancellableC5store2inyShyACGz_tF";
    Image.ImportPtrSlots[Slot] = Name;
    Image.DyldBindSlots[Slot] = {
        Name, 0, "/System/Library/Frameworks/Combine.framework/Combine", false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    ASSERT_TRUE(Hint);
    HighFunc F;
    F.Name = Sink ? "recovered_sink" : "recovered_store";
    F.ReturnType = Hint->Signature.ReturnType;
    std::vector<ExprPtr> Args;
    for (unsigned I = 0; I < (Sink ? 5U : 2U); ++I) {
      F.Params.push_back({"arg" + std::to_string(I), Pointer});
      Args.push_back(parameter(I, Pointer));
    }
    auto Call = call(*Hint, F.ReturnType, Args);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    HighStmt Statement;
    Statement.Kind = Sink ? StmtKind::Return : StmtKind::ExprStmt;
    if (Sink)
      Statement.RetVal = Call;
    else
      Statement.Val = Call;
    F.Body = {Statement};
    Functions.push_back(std::move(F));
  }
  const auto Source = emit(Functions, true, Architecture);
  ASSERT_NE(Source.find("swiftcall"), std::string::npos);
  ASSERT_NE(Source.find("swift_context"), std::string::npos);
  // Independent carrier oracles. Real SDK ownership/callback behavior is
  // exercised separately with the original machine fixture and Combine.
  const auto Program = Source + R"(
struct State { unsigned calls; void *words[4]; uintptr_t result; };
void *__attribute__((swiftcall)) sink_oracle(void *,void *,void *,void *,
    struct State *__attribute__((swift_context)))
    __asm__("_$s7Combine9PublisherPAAs5NeverO7FailureRtzrlE4sink12receiveValueAA14AnyCancellableCy6OutputQzc_tF");
void *__attribute__((swiftcall)) sink_oracle(void *a,void *b,void *c,void *d,
    struct State *s __attribute__((swift_context))) {
  ++s->calls; s->words[0]=a; s->words[1]=b; s->words[2]=c; s->words[3]=d;
  s->result=(uintptr_t)a ^ (uintptr_t)b ^ (uintptr_t)c ^ (uintptr_t)d;
  return &s->result;
}
void __attribute__((swiftcall)) store_oracle(void *,void *__attribute__((swift_context)))
    __asm__("_$s7Combine14AnyCancellableC5store2inyShyACGz_tF");
void __attribute__((swiftcall)) store_oracle(void *out,void *object __attribute__((swift_context))) {
  __builtin_memcpy(out,&object,sizeof(object));
}
int main(void) {
  for (unsigned i=0;i<1024;++i) {
    struct { uintptr_t guard; struct State s; uintptr_t tail; } box={0};
    uintptr_t words[4]={i,~(uintptr_t)i,(uintptr_t)i*17,(uintptr_t)i*53};
    void *args[4]={words,words+1,words+2,words+3};
    box.guard=0xabcdef12;box.tail=0x12345678;
    void *result=recovered_sink(args[0],args[1],args[2],args[3],&box.s);
    if(result!=&box.s.result||box.s.calls!=1||box.guard!=0xabcdef12||box.tail!=0x12345678)return 1;
    uintptr_t expected=0;
    for(unsigned j=0;j<4;++j){if(box.s.words[j]!=args[j])return 2;expected^=(uintptr_t)args[j];}
    if(box.s.result!=expected)return 3;
    struct { uintptr_t guard; void *value; uintptr_t tail; } out={0x76543210,0,0xfedcba98};
    recovered_store(&out.value,result);
    if(out.value!=result||out.guard!=0x76543210||out.tail!=0xfedcba98)return 4;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"})
    compileAndRun(Program, {Optimization});
}

TEST(HighCSourceCalls,
     SwiftCoreGraphicsPointActionsKeepCoordinatesContextAndOrder) {
#if defined(__aarch64__) || defined(_M_ARM64)
  const auto Architecture = Arch::AArch64;
#else
  const auto Architecture = Arch::X64;
#endif
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Double = NdType::makeFloat(8);
  HighFunc F;
  F.Name = "recovered_point_actions";
  F.ReturnType = NdType::makeVoid();
  F.Params = {{"x", Double}, {"y", Double}, {"context", Pointer}};
  for (const char *Name :
       {"_$sSo12CGContextRefa12CoreGraphicsE4move2toySo7CGPointV_tF",
        "_$sSo12CGContextRefa12CoreGraphicsE7addLine2toySo7CGPointV_tF"}) {
    auto Image = runtime_function_address_test::image(Architecture);
    const auto Slot = runtime_function_address_test::Slot;
    Image.ImportPtrSlots[Slot] = Name;
    Image.DyldBindSlots[Slot] = {
        Name, 0,
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
        false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    ASSERT_TRUE(Hint);
    auto Call = call(
        *Hint, F.ReturnType,
        {parameter(0, Double), parameter(1, Double), parameter(2, Pointer)});
    ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    HighStmt Statement;
    Statement.Kind = StmtKind::ExprStmt;
    Statement.Val = Call;
    F.Body.push_back(std::move(Statement));
  }
  const auto Source = emit({F}, true, Architecture);
  ASSERT_NE(Source.find("swiftcall"), std::string::npos);
  ASSERT_NE(Source.find("swift_context"), std::string::npos);
  const auto Program = Source + R"(
struct PointState { unsigned calls; uint64_t bits[2][2]; };
static void capture_point(double x,double y,struct PointState *s,unsigned index) {
  if(s->calls!=index) __builtin_trap();
  __builtin_memcpy(&s->bits[index][0],&x,8);
  __builtin_memcpy(&s->bits[index][1],&y,8);
  ++s->calls;
}
void __attribute__((swiftcall)) move_oracle(double,double,
    struct PointState *__attribute__((swift_context)))
    __asm__("_$sSo12CGContextRefa12CoreGraphicsE4move2toySo7CGPointV_tF");
void __attribute__((swiftcall)) move_oracle(double x,double y,
    struct PointState *s __attribute__((swift_context))) {capture_point(x,y,s,0);}
void __attribute__((swiftcall)) line_oracle(double,double,
    struct PointState *__attribute__((swift_context)))
    __asm__("_$sSo12CGContextRefa12CoreGraphicsE7addLine2toySo7CGPointV_tF");
void __attribute__((swiftcall)) line_oracle(double x,double y,
    struct PointState *s __attribute__((swift_context))) {capture_point(x,y,s,1);}
int main(void) {
  const uint64_t values[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
    0x3ff0000000000000ULL,0xbff012345678abcdULL,0x7ff0000000000000ULL,
    0xfff0000000000000ULL,0x7ff812345678abcdULL,0x7ff012345678abcdULL};
  for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i)
    for(unsigned j=0;j<sizeof(values)/sizeof(values[0]);++j) {
      struct {uint64_t guard;struct PointState s;uint64_t tail;} box={0};
      box.guard=0xabcdef0123456789ULL;box.tail=0x123456789abcdef0ULL;
      double x,y;__builtin_memcpy(&x,values+i,8);__builtin_memcpy(&y,values+j,8);
      recovered_point_actions(x,y,&box.s);
      if(box.s.calls!=2||box.guard!=0xabcdef0123456789ULL||box.tail!=0x123456789abcdef0ULL)return 1;
      for(unsigned k=0;k<2;++k)
        if(box.s.bits[k][0]!=values[i]||box.s.bits[k][1]!=values[j])return 2;
    }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"})
    compileAndRun(Program, {Optimization});
}

TEST(HighCSourceCalls,
     SwiftCGRectMethodKeepsContextReceiverAndAllCoordinateBits) {
#if !defined(__aarch64__) && !defined(_M_ARM64)
  GTEST_SKIP() << "The complete CGRect method entry contract is arm64 only";
#else
  auto Image = runtime_function_address_test::image(Arch::AArch64);
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(4);
  Image.Segments.push_back(std::move(Text));
  const std::string Name =
      "_$s13RectMethodABI12DrawingOwnerC4draw2in4rectySo12CGContextRefa_"
      "So6CGRectVtF";
  Image.Symbols.push_back({Name, 0x1000, 0, true});
  const auto Declaration =
      swiftMangledCGContextCGRectClassMethodSourceABI(Image, 0x1000);
  ASSERT_TRUE(Declaration);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Double = NdType::makeFloat(8);
  HighFunc F;
  F.Name = "recovered_rect_method";
  F.ReturnType = NdType::makeVoid();
  F.Params = {{"context", Pointer}, {"x", Double},      {"y", Double},
              {"width", Double},    {"height", Double}, {"receiver", Pointer}};
  SourceCallTypeHint Hint;
  Hint.TargetName = "rect_carrier";
  Hint.TargetAddress = 0x1000;
  Hint.Signature = *Declaration;
  auto Rectangle =
      HighExpr::makeRecord(Hint.Signature.Parameters[1].Type,
                           {parameter(1, Double), parameter(2, Double),
                            parameter(3, Double), parameter(4, Double)});
  HighStmt S;
  S.Kind = StmtKind::ExprStmt;
  S.Val = call(Hint, F.ReturnType,
               {parameter(0, Pointer), Rectangle, parameter(5, Pointer)});
  F.Body = {S};
  const auto Source = emit({F}, true, Arch::AArch64);
  ASSERT_NE(Source.find("swift_context"), std::string::npos);
  // The oracle uses the independently compiler-observed physical scalar
  // signature, rather than repeating the emitted logical record declaration.
  const auto Program = Source + R"(
struct RectState { unsigned calls; void *context; uint64_t bits[4]; };
#ifdef __APPLE__
#define RECT_CARRIER_SYMBOL "_rect_carrier"
#else
#define RECT_CARRIER_SYMBOL "rect_carrier"
#endif
void __attribute__((swiftcall)) rect_oracle(void *,double,double,double,double,
    struct RectState *__attribute__((swift_context)))
    __asm__(RECT_CARRIER_SYMBOL);
void __attribute__((swiftcall)) rect_oracle(void *c,double x,double y,double w,double h,
    struct RectState *s __attribute__((swift_context))) {
  ++s->calls;s->context=c;
  __builtin_memcpy(s->bits+0,&x,8);__builtin_memcpy(s->bits+1,&y,8);
  __builtin_memcpy(s->bits+2,&w,8);__builtin_memcpy(s->bits+3,&h,8);
}
int main(void) {
  const uint64_t values[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
    0x3ff0000000000000ULL,0xbff012345678abcdULL,0x7ff0000000000000ULL,
    0xfff0000000000000ULL,0x7ff812345678abcdULL,0x7ff012345678abcdULL};
  for(unsigned i=0;i<1000;++i) {
    uint64_t expected[4];double v[4];
    for(unsigned j=0;j<4;++j) {
      expected[j]=values[(i/(j+1)+j*3)%10];
      __builtin_memcpy(v+j,expected+j,8);
    }
    struct {uint64_t guard;struct RectState s;uint64_t tail;} box={0};
    struct {uint64_t a,b;} context={0x0123456789abcdefULL,i};
    box.guard=0xabcdef0123456789ULL;box.tail=0x123456789abcdef0ULL;
    recovered_rect_method(&context,v[0],v[1],v[2],v[3],&box.s);
    if(box.s.calls!=1||box.s.context!=&context||box.guard!=0xabcdef0123456789ULL||
       box.tail!=0x123456789abcdef0ULL||context.a!=0x0123456789abcdefULL||context.b!=i)return 1;
    for(unsigned j=0;j<4;++j) if(box.s.bits[j]!=expected[j])return 2;
  }
  return 0;
}
)";
  for (const auto Optimization : {"-O0", "-O2"})
    compileAndRun(Program, {Optimization});
#endif
}
