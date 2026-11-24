//===- LLVMCSourcePointerTests.cpp - Source call pointer bridges --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute calls whose source declarations and machine carriers differ.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceTypeHints.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

class PointerContext final : public NullDebugContext {
public:
  std::map<va_t, FunctionSym> Functions;
  std::vector<va_t> BoundEntries;
  llvm::ArrayRef<va_t> boundFunctionEntries() const override {
    return BoundEntries;
  }
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    const auto It = Functions.find(Address);
    return It == Functions.end() ? std::nullopt : std::optional(It->second);
  }
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    for (const auto &[Address, Function] : Functions)
      if (Function.Name == Name)
        return Function;
    return std::nullopt;
  }
  bool hasInfo() const override { return true; }
};

TEST(LLVMCSourcePointers, ComplexResultIdentitySurvivesCallsAndSerialization) {
  for (const Arch Architecture : {Arch::X64, Arch::MIPS32}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const unsigned Bytes : {4, 8}) {
      SCOPED_TRACE(Bytes);
      llvm::LLVMContext Context;
      llvm::Module Module("complex-result", Context);
      Module.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> Builder(Context);
      auto *Scalar = Bytes == 4 ? Builder.getFloatTy() : Builder.getDoubleTy();
      auto *Integer = Builder.getIntNTy(Bytes * 8);
      auto *Pair = llvm::StructType::get(Context, {Scalar, Scalar});
      auto *Make = llvm::Function::Create(
          llvm::FunctionType::get(Pair, {Integer, Integer}, false),
          llvm::GlobalValue::ExternalLinkage, "make_complex", Module);
      Make->addFnAttr(LLVMSourceComplexResult);
      ASSERT_TRUE(readLLVMSourceComplexResult(Pair, Make->getAttributes()));
      EXPECT_THROW(readLLVMSourceComplexResult(Integer, Make->getAttributes()),
                   std::invalid_argument);
      EXPECT_THROW(readLLVMSourceComplexResult(
                       llvm::StructType::get(Context, {Scalar, Scalar}, true),
                       Make->getAttributes()),
                   std::invalid_argument);
      Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Make));
      auto *NoteCall = llvm::Function::Create(
          llvm::FunctionType::get(Builder.getVoidTy(), false),
          llvm::GlobalValue::ExternalLinkage, "note_complex_call", Module);
      Builder.CreateCall(NoteCall);
      llvm::Value *Value = llvm::PoisonValue::get(Pair);
      Value = Builder.CreateInsertValue(
          Value, Builder.CreateBitCast(Make->getArg(0), Scalar), {0});
      Value = Builder.CreateInsertValue(
          Value, Builder.CreateBitCast(Make->getArg(1), Scalar), {1});
      Builder.CreateRet(Value);
      auto *Forward = llvm::Function::Create(
          llvm::FunctionType::get(Integer, {Integer, Integer}, false),
          llvm::GlobalValue::ExternalLinkage, "imaginary_bits", Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Forward));
      auto *Call =
          Builder.CreateCall(Make, {Forward->getArg(0), Forward->getArg(1)});
      Builder.CreateRet(Builder.CreateBitCast(
          Builder.CreateExtractValue(Call, {1}), Integer));
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      std::string IR;
      llvm::raw_string_ostream IRStream(IR);
      Module.print(IRStream, nullptr);
      llvm::SMDiagnostic Diagnostic;
      auto Reparsed = llvm::parseAssemblyString(IR, Diagnostic, Context);
      ASSERT_TRUE(Reparsed);
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = BinaryFormat::ELF;
      ASSERT_TRUE(LLVMCEmitter().emit(*Reparsed, OS, Options));
      const std::string Word = Bytes == 4 ? "uint32_t" : "uint64_t";
      const std::string ScalarName = Bytes == 4 ? "float" : "double";
      EXPECT_NE(Source.find(ScalarName + " _Complex make_complex"),
                std::string::npos);
      Source += "\nstatic unsigned complex_calls;\n"
                "void note_complex_call(void) { ++complex_calls; }\n"
                "int main(void) { const " +
                Word + " values[] = {0, " +
                (Bytes == 4 ? "0x80000000u, 0x7fc01234u, 0xff800000u"
                            : "0x8000000000000000ull, 0x7ff8000000001234ull, "
                              "0xfff0000000000000ull") +
                "}; for(unsigned i=0;i<4;++i) for(unsigned j=0;j<4;++j) { "
                "complex_calls=0; " +
                ScalarName + " _Complex z=make_complex(values[i],values[j]); " +
                ScalarName + " r=__real__ z, m=__imag__ z; " + Word +
                " a,b; __builtin_memcpy(&a,&r,sizeof(a)); "
                "__builtin_memcpy(&b,&m,sizeof(b)); "
                "if(a!=values[i] || b!=values[j] || "
                "imaginary_bits(values[i],values[j])!=values[j] || "
                "complex_calls!=2) return 1; "
                "} return 0; }\n";
      for (const auto Optimization : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(Source, {Optimization});
    }
  }
}

TEST(LLVMCSourcePointers, ARM64ImageBackedImportLoadBelongsToItsCalleeUse) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(R"(
target datalayout = "e-p:64:64"
@__nd_data_180000000.image = external global [8994 x i8], align 4096, !neverd.source.image-storage !0
define i32 @only_call(ptr %handle) {
  %slot = load volatile i64, ptr getelementptr (i8, ptr @__nd_data_180000000.image, i64 8640), align 8
  %callee = inttoptr i64 %slot to ptr
  %r = call i32 %callee(ptr %handle)
  ret i32 %r
}
define i32 @also_pass_pointer() {
  %slot = load volatile i64, ptr getelementptr (i8, ptr @__nd_data_180000000.image, i64 8640), align 8
  %callee = inttoptr i64 %slot to ptr
  %r = call i32 %callee(ptr %callee)
  ret i32 %r
}
!0 = !{}
)",
                                          Error, Context);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::COFF;
  Image.Bits = Bitness::Bits64;
  Import Entry;
  Entry.Name = "IcmpCloseHandle";
  Entry.Module = "iphlpapi.dll";
  Entry.IATAddr = 0x1800021C0;
  Image.Imports.push_back(Entry);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  for (const auto &Fn : *Module) {
    SCOPED_TRACE(Fn.getName().str());
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(
        LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image, &Fn));
    Out.flush();
    EXPECT_NE(Source.find("IcmpCloseHandle("), std::string::npos) << Source;
    const bool ReadAsValue = Fn.getName() == "also_pass_pointer";
    EXPECT_EQ(Source.find("volatile uint8_t") != std::string::npos, ReadAsValue)
        << Source;
    EXPECT_EQ(Source.find("unk_180000000") != std::string::npos, ReadAsValue)
        << Source;
  }
}

TEST(LLVMCSourcePointers, StandaloneHeaderBooleanCompilesAsC17) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(R"(
    declare i8 @check_name(ptr)
    define i8 @use_check(ptr %name) {
      %result = call i8 @check_name(ptr %name)
      ret i8 %result
    }
  )",
                                          Error, Context);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  PointerContext Debug;
  auto &Check = Debug.Functions[0x2000];
  Check.Name = "check_name";
  Check.ReturnType = NdType::makeInt(1, false);
  Check.ReturnType->SourceName = "bool";
  Check.Params = {{"name", NdType::makePtr(NdType::makeInt(2, false))}};
  Check.PrototypeLibrary = "fixture-header";
  Check.PrototypeFromHeader = true;
  Check.PortablePrototypeDeclaration = "bool check_name(uint16_t *name)";
  Check.PortableParameterTypes = {"uint16_t *"};
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::COFF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options, &Debug));
  OS.flush();
  ASSERT_NE(Source.find("bool check_name("), std::string::npos) << Source;
  source_call_execution_test::checkSyntax(
      Source,
      {"--target=aarch64-pc-windows-msvc", "-std=c17", "-ffreestanding"});
}

TEST(LLVMCSourcePointers, AddressBoundZeroArgumentStdcallKeepsItsLinkage) {
  constexpr va_t Entry = 0x2000;
  llvm::LLVMContext Context;
  llvm::Module Module("entry-convention", Context);
  Module.setDataLayout("e-p:32:32");
  llvm::IRBuilder<> Builder(Context);
  auto *Type = llvm::FunctionType::get(Builder.getInt32Ty(), {}, false);
  auto *Fn = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                    "server_callback", Module);
  rewrite_source::setOriginalVA(*Fn, Entry);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
  Builder.CreateRet(Builder.getInt32(1));
  PointerContext Debug;
  auto &Source = Debug.Functions[Entry];
  Source.Addr = Entry;
  Source.Name = "server_callback";
  Source.CallConv = DebugCallConv::Stdcall;
  Source.ReturnType = NdType::makeInt(4, true);
  Source.PrototypeFromHeader = true;
  CEmitterOptions Options;
  Options.TheArch = Arch::X86;
  Options.Format = BinaryFormat::COFF;
  const auto render = [&] {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options, &Debug));
    return Text;
  };
  const std::string Attribute = "__attribute__((stdcall))";
  EXPECT_EQ(render().find(Attribute), std::string::npos);
  Debug.BoundEntries = {Entry};
  const auto Typed = render();
  EXPECT_NE(Typed.find(Attribute + " int32_t server_callback(void)"),
            std::string::npos)
      << Typed;
  Options.PreserveLLVMFunctionTypes = true;
  EXPECT_EQ(render().find(Attribute), std::string::npos);
  Fn->setCallingConv(llvm::CallingConv::X86_StdCall);
  EXPECT_NE(render().find(Attribute), std::string::npos);
}

TEST(LLVMCSourcePointers, HeaderCallbackCastsKeepNestedPointeesAndQualifiers) {
  for (const auto &[Arch, Convention] :
       {std::pair{Arch::X86, types::CallingConvention::Cdecl},
        std::pair{Arch::X86, types::CallingConvention::Stdcall},
        std::pair{Arch::X64, types::CallingConvention::Unspecified},
        std::pair{Arch::AArch64, types::CallingConvention::Unspecified}}) {
    SCOPED_TRACE(static_cast<int>(Arch));
    SCOPED_TRACE(static_cast<int>(Convention));
    const bool Narrow = Arch == Arch::X86;
    llvm::LLVMContext Context;
    llvm::Module Module("header-callback", Context);
    Module.setDataLayout(Narrow ? "e-p:32:32" : "e-p:64:64");
    llvm::IRBuilder<> Builder(Context);
    auto *Word = Narrow ? Builder.getInt32Ty() : Builder.getInt64Ty();
    auto *Signature = llvm::FunctionType::get(Word, {Word, Word, Word}, false);
    auto *Callee =
        llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                               "register_callback", Module);
    auto *Entry =
        llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                               "callback_entry", Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Entry));
    Builder.CreateRet(Builder.CreateCall(
        Callee, {Entry->getArg(0), Entry->getArg(1), Entry->getArg(2)}));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    const auto Record =
        NdType::makePtr(NdType::makeNamedRecord("CallbackRecord", 4));
    const auto Output = NdType::makePtr(NdType::makeInt(4, false));
    const auto Callback = NdType::makePtr(
        NdType::makeFunc(NdType::makeVoid(), {Record, Output}, Convention));
    Record->Size = Output->Size = Callback->Size = Narrow ? 4 : 8;
    const std::string Keyword = !Narrow ? ""
                                : Convention == types::CallingConvention::Cdecl
                                    ? "__cdecl "
                                    : "__stdcall ";
    PointerContext Debug;
    FunctionSym Function;
    Function.Name = "register_callback";
    Function.ReturnType = NdType::makeInt(4, false);
    Function.Params = {
        {"Callback", Callback}, {"Input", Record}, {"Output", Output}};
    Function.PrototypeLibrary = "fixture-header";
    Function.PrototypeFromHeader = true;
    Function.PortablePrototypeDeclaration =
        "uint32_t register_callback(void (" + Keyword +
        "*Callback)(const struct "
        "CallbackRecord *, "
        "volatile uint32_t *), const struct CallbackRecord *Input, volatile "
        "uint32_t *Output)";
    Function.PortableParameterTypes = {
        "void (" + Keyword +
            "*)(const struct CallbackRecord *, volatile uint32_t *)",
        "const struct CallbackRecord *", "volatile uint32_t *"};
    Debug.Functions[0x2000] = Function;
    CEmitterOptions Options;
    Options.TheArch = Arch;
    Options.Format = BinaryFormat::COFF;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options, &Debug));
    Out.flush();
    Text = "#include <stdint.h>\nstruct CallbackRecord { uint32_t Value; };\n" +
           Text;
    if (Narrow) {
      Text +=
          "typedef void (" + Keyword +
          "*ExpectedCallback)(const struct CallbackRecord *, volatile uint32_t "
          "*);\n"
          "typedef uint32_t (*ExpectedRegister)(ExpectedCallback, const struct "
          "CallbackRecord *, volatile uint32_t *);\n"
          "_Static_assert(__builtin_types_compatible_p(__typeof__(&register_"
          "callback), ExpectedRegister), \"source callback identity\");\n";
      source_call_execution_test::checkSyntax(
          Text, {"--target=i686-w64-windows-gnu",
                 "-Werror=incompatible-pointer-types",
                 "-Werror=incompatible-function-pointer-types"});
      continue;
    }
    Text += R"c(
uint32_t register_callback(void (*Callback)(const struct CallbackRecord *, volatile uint32_t *),
                           const struct CallbackRecord *Input, volatile uint32_t *Output) {
  Callback(Input, Output);
  return *Output + 3;
}
static void observed(const struct CallbackRecord *Input, volatile uint32_t *Output) {
  *Output = Input->Value ^ 0xA5;
}
int main(void) {
  struct CallbackRecord Input = {0x1234};
  volatile uint32_t Output = 0;
  uint64_t Result = callback_entry((uintptr_t)&observed, (uintptr_t)&Input, (uintptr_t)&Output);
  return Output != (0x1234 ^ 0xA5) || Result != (0x1234 ^ 0xA5) + 3;
}
)c";
    for (const auto Optimization : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Text, {Optimization, "-Werror=incompatible-pointer-types",
                 "-Werror=incompatible-function-pointer-types",
                 "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(LLVMCSourcePointers, DefinitionAndCallsShareObjectAndCallbackTypes) {
  for (const auto Arch : {Arch::X64, Arch::AArch64})
    for (const auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Mode = 0; Mode != 7; ++Mode) {
        SCOPED_TRACE(static_cast<int>(Arch));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Mode);
        const bool Selected = Mode == 1;
        const bool Preserved = Mode == 2;
        const bool Alias = Mode == 3 || Mode >= 5;
        const bool PointerCarrier = Mode >= 5;
        llvm::LLVMContext C;
        llvm::Module M("source-pointer-calls", C);
        M.setDataLayout("e-p:64:64");
        llvm::IRBuilder<> B(C);
        auto *Word = B.getInt64Ty();
        auto *Unary = llvm::FunctionType::get(Word, {Word}, false);
        // The caller precedes its definitions, as a module's physical order
        // carries no authority over a source prototype.
        auto *Caller = llvm::Function::Create(
            Unary, llvm::GlobalValue::ExternalLinkage, "bridge_entry", M);
        llvm::Type *Address = Word;
        if (PointerCarrier)
          Address = B.getPtrTy();
        auto *RoundTrip = llvm::Function::Create(
            llvm::FunctionType::get(Address, {Address}, false),
            llvm::GlobalValue::ExternalLinkage, "round_trip", M);
        auto *Apply = llvm::Function::Create(
            llvm::FunctionType::get(Word, {Word, Word}, false),
            llvm::GlobalValue::ExternalLinkage, "apply_callback", M);
        auto *Callback = llvm::Function::Create(
            Unary, llvm::GlobalValue::ExternalLinkage, "add_seven", M);
        rewrite_source::setOriginalVA(*RoundTrip, 0x2000);
        rewrite_source::setOriginalVA(*Apply, 0x3000);
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Caller));
        auto *Pointer = B.CreateCall(
            RoundTrip,
            {PointerCarrier ? B.CreateIntToPtr(Caller->getArg(0), Address)
                            : Caller->getArg(0)});
        auto *Value = B.CreateLoad(
            Word,
            PointerCarrier ? Pointer : B.CreateIntToPtr(Pointer, B.getPtrTy()));
        B.CreateRet(
            B.CreateCall(Apply, {B.CreatePtrToInt(Callback, Word), Value}));
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", RoundTrip));
        B.CreateRet(RoundTrip->getArg(0));
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Apply));
        B.CreateRet(B.CreateCall(
            Unary, B.CreateIntToPtr(Apply->getArg(0), B.getPtrTy()),
            {Apply->getArg(1)}));
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Callback));
        B.CreateRet(B.CreateAdd(Callback->getArg(0), B.getInt64(7)));
        ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
        const auto Scalar = NdType::makeInt(8, false);
        const auto PointerType = NdType::makePtr(Scalar);
        // A source pointer narrower than its machine carrier cannot change
        // either side of the signature into the compiler's native pointer.
        if (Mode == 4 || Mode == 6)
          PointerType->Size = 4;
        if (Alias)
          PointerType->TypedefName = "WORD_POINTER";
        auto Function = std::make_shared<NdType>();
        Function->Kind = NdTypeKind::Func;
        Function->RetType = Scalar;
        Function->ParamTypes = {Scalar};
        PointerContext Debug;
        FunctionSym Round;
        Round.Addr = 0x2000;
        Round.Name = "round_trip";
        Round.ReturnType = PointerType;
        Round.Params = {{"address", PointerType}};
        Debug.Functions[Round.Addr] = Round;
        FunctionSym Call;
        Call.Addr = 0x3000;
        Call.Name = "apply_callback";
        Call.ReturnType = Scalar;
        Call.Params = {{"callback", NdType::makePtr(Function)},
                       {"value", Scalar}};
        Debug.Functions[Call.Addr] = Call;
        CEmitterOptions Options;
        Options.TheArch = Arch;
        Options.Format = Format;
        Options.PreserveLLVMFunctionTypes = Preserved;
        Options.RecordDefinitions = [Alias](const std::vector<TypeRef> &) {
          CRecordDefinitions Definitions;
          if (Alias) {
            Definitions.Text = "typedef uint64_t *WORD_POINTER;\n";
            Definitions.Identifiers = {"WORD_POINTER"};
          }
          return Definitions;
        };
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        ASSERT_TRUE(LLVMCEmitter().emit(M, Out, Options, &Debug, nullptr,
                                        Selected ? Caller : nullptr));
        Out.flush();
        if (Alias)
          EXPECT_EQ(
              Text.find("WORD_POINTER round_trip(WORD_POINTER address)") !=
                  std::string::npos,
              Mode != 6)
              << Text;
        if (Selected)
          Text += R"c(
uint64_t round_trip(uint64_t address) { return address; }
uint64_t apply_callback(uint64_t callback, uint64_t value) {
  return ((uint64_t (*)(uint64_t))(uintptr_t)callback)(value);
}
uint64_t add_seven(uint64_t value) { return value + 7; }
)c";
        Text += R"c(
int main(void) {
  uint64_t value = 11;
  if (bridge_entry((uintptr_t)&value) != 18) return 1;
  value = 71;
  return bridge_entry((uintptr_t)&value) != 78;
}
)c";
        for (const auto Optimization : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Text, {Optimization, "-Werror=int-conversion",
                     "-Werror=incompatible-pointer-types",
                     "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
}
TEST(LLVMCSourcePointers, InferredPointeesSurviveSerializationAndShareCalls) {
  for (unsigned Kind : {0u, 1u, 2u})
    for (unsigned Mode : {0u, 1u, 2u, 3u}) {
      SCOPED_TRACE(Kind);
      SCOPED_TRACE(Mode);
      const bool Nested = Kind == 2;
      const bool Explicit = Mode == 1;
      const bool Preserve = Mode == 2;
      const bool Selected = Mode == 3;
      llvm::LLVMContext Context;
      llvm::Module Module("inferred-source-pointers", Context);
      Module.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> Builder(Context);
      const auto Scalar = NdType::makeInt(2, false);
      const auto Record = NdType::makeNamedRecord("PointerPayload", 64);
      auto Type = NdType::makePtr(Kind ? Record : Scalar);
      if (Nested)
        Type = NdType::makePtr(Type);
      auto *Entry = llvm::Function::Create(
          llvm::FunctionType::get(Builder.getInt16Ty(), {Builder.getInt64Ty()},
                                  false),
          llvm::GlobalValue::ExternalLinkage, "pointer_entry", Module);
      auto *Forward = llvm::Function::Create(
          llvm::FunctionType::get(Builder.getInt16Ty(), {Builder.getPtrTy()},
                                  false),
          llvm::GlobalValue::ExternalLinkage, "pointer_forward", Module);
      rewrite_source::setOriginalVA(*Forward, 0x2000);
      writeLLVMSourcePointerParameter(*Forward->getArg(0), Type);
      const auto Read = readLLVMSourcePointerParameter(*Forward->getArg(0));
      ASSERT_TRUE(Read);
      auto Leaf = Read;
      while (Leaf->Kind == NdTypeKind::Ptr)
        Leaf = Leaf->Pointee;
      if (Kind) {
        EXPECT_EQ(Leaf->Size, 0u);
        EXPECT_TRUE(Leaf->Fields.empty());
      }
      Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Entry));
      Builder.CreateRet(Builder.CreateCall(
          Forward,
          {Builder.CreateIntToPtr(Entry->getArg(0), Builder.getPtrTy())}));
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Forward));
      llvm::Value *Address = Forward->getArg(0);
      if (Nested)
        Address = Builder.CreateLoad(Builder.getPtrTy(), Address);
      Builder.CreateRet(Builder.CreateLoad(Builder.getInt16Ty(), Address));
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      std::string Serialized;
      llvm::raw_string_ostream IR(Serialized);
      Module.print(IR, nullptr);
      llvm::LLVMContext RestoredContext;
      llvm::SMDiagnostic Diagnostic;
      const auto Restored =
          llvm::parseAssemblyString(Serialized, Diagnostic, RestoredContext);
      ASSERT_TRUE(Restored);
      PointerContext Debug;
      if (Explicit) {
        FunctionSym Declared;
        Declared.Addr = 0x2000;
        Declared.ReturnType = Scalar;
        Declared.Params = {{"input", NdType::makePtr()}};
        Debug.Functions.emplace(Declared.Addr, Declared);
      }
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.Format = BinaryFormat::COFF;
      Options.PreserveLLVMFunctionTypes = Preserve;
      std::string Text;
      llvm::raw_string_ostream Out(Text);
      ASSERT_TRUE(LLVMCEmitter().emit(
          *Restored, Out, Options, Explicit ? &Debug : nullptr, nullptr,
          Selected ? Restored->getFunction("pointer_entry") : nullptr));
      Out.flush();
      if (Explicit || Preserve || Selected)
        EXPECT_EQ(Text.find("PointerPayload"), std::string::npos) << Text;
      else
        EXPECT_NE(Text.find(std::string(Kind ? "PointerPayload" : "uint16_t") +
                            (Nested ? "** " : "* ")),
                  std::string::npos)
            << Text;
      if (Selected)
        Text +=
            std::string("\nuint16_t pointer_forward(void *input) { return ") +
            (Nested ? "**(uint16_t **)input; }\n" : "*(uint16_t *)input; }\n");
      Text += R"c(
struct PointerPayload { uint16_t value; };
int main(void) {
  struct PointerPayload value = {0x1234};
  struct PointerPayload *pointer = &value;
)c";
      Text += std::string("  return pointer_entry((uintptr_t)") +
              (Nested ? "&pointer" : "&value") + " ) != 0x1234;\n}\n";
      for (const auto Optimization : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Text, {Optimization, "-Werror=int-conversion",
                   "-Werror=incompatible-pointer-types", "-fsanitize=undefined",
                   "-fsanitize-trap=all"});
    }
}

TEST(LLVMCSourcePointers, PointerSpellingRetainsSourceABIDepthBoundary) {
  llvm::LLVMContext Context;
  llvm::Module Module("pointer-attribute-depth", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> Builder(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getVoidTy(), {Builder.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "depth_boundary", Module);
  auto &Argument = *Function->getArg(0);
  for (bool Record : {false, true})
    for (unsigned Depth : {16u, 17u, 18u}) {
      SCOPED_TRACE(Record);
      SCOPED_TRACE(Depth);
      auto Type = Record ? NdType::makeNamedRecord("DepthPayload", 0)
                         : NdType::makeInt(2, false);
      std::string Text = "[";
      for (unsigned I = 0; I < Depth; ++I) {
        Type = NdType::makePtr(Type);
        Text += "[\"ptr\",8,false,\"\"],";
      }
      Text += Record ? "[\"record\",0,false,\"DepthPayload\"]]"
                     : "[\"int\",2,false,\"\"]]";
      // The record leaf is checked at its pointer's depth; scalar leaves
      // consume another recursion level. Keep these expectations independent
      // of the implementation's limit expression.
      const bool Accepted = Depth == 16 || (Record && Depth == 17);
      writeLLVMSourcePointerParameter(Argument, Type);
      const auto Restored = readLLVMSourcePointerParameter(Argument);
      ASSERT_EQ(bool(Restored), Accepted);
      if (Restored) {
        auto Leaf = Restored;
        for (unsigned I = 0; I < Depth; ++I) {
          ASSERT_EQ(Leaf->Kind, NdTypeKind::Ptr);
          Leaf = Leaf->Pointee;
          ASSERT_TRUE(Leaf);
        }
        EXPECT_EQ(Leaf->Kind, Record ? NdTypeKind::Struct : NdTypeKind::Int);
        if (Record)
          EXPECT_EQ(Leaf->SourceName, "DepthPayload");
      }
      // Direct attributes must retain the semantic limit too, rather than
      // relying on the writer to reject an unsupported chain first.
      Argument.addAttr(
          llvm::Attribute::get(Context, LLVMSourcePointerParameter, Text));
      EXPECT_EQ(bool(readLLVMSourcePointerParameter(Argument)), Accepted);
    }
}

TEST(LLVMCSourcePointers, MalformedOrChangedPointerCarriersIgnoreSpelling) {
  llvm::LLVMContext Context;
  llvm::Module Module("pointer-attribute-boundary", Context);
  Module.setDataLayout("e-p:64:64-p1:32:32");
  llvm::IRBuilder<> Builder(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(
          Builder.getVoidTy(),
          {Builder.getPtrTy(), Builder.getInt64Ty(), Builder.getPtrTy(1)},
          false),
      llvm::GlobalValue::ExternalLinkage, "boundary", Module);
  const auto Type = NdType::makePtr(NdType::makeInt(2, false));
  writeLLVMSourcePointerParameter(*Function->getArg(0), Type);
  const auto Good =
      Function->getAttributes().getParamAttr(0, LLVMSourcePointerParameter);
  ASSERT_TRUE(readLLVMSourcePointerParameter(*Function->getArg(0)));
  for (unsigned Index : {1u, 2u}) {
    Function->getArg(Index)->addAttr(Good);
    EXPECT_FALSE(readLLVMSourcePointerParameter(*Function->getArg(Index)));
  }
  Module.setDataLayout("e-p:32:32");
  EXPECT_FALSE(readLLVMSourcePointerParameter(*Function->getArg(0)));
  Module.setDataLayout("e-p:64:64");
  for (llvm::StringRef Text :
       {"invalid", "[]", "[[\"ptr\",8,false,\"\"]]",
        "[[\"ptr\",4,false,\"\"],[\"int\",2,false,\"\"]]",
        "[[\"ptr\",8,false,\"\"],[\"int\",3,false,\"\"]]",
        "[[\"ptr\",8,false,\"\"],[\"record\",8,false,\"Record\"]]",
        "[[\"ptr\",8,false,\"\"],[\"record\",0,false,\"bad;name\"]]",
        "[[\"ptr\",8,false,\"\"],[\"function\",0,false,\"\"]]"}) {
    Function->getArg(0)->addAttr(
        llvm::Attribute::get(Context, LLVMSourcePointerParameter, Text));
    EXPECT_FALSE(readLLVMSourcePointerParameter(*Function->getArg(0)))
        << Text.str();
  }
}
} // namespace
