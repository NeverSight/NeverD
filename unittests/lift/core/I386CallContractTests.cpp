//===- I386CallContractTests.cpp - i386 stack-passed call arguments -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// i386 passes every argument on the stack.  MSVC pushes them; GCC stores
// each at [esp+N] below one adjustment.  Stores to the function's own locals
// in between are no arguments.  An import thunk `jmp dword ptr [slot]`
// reaches its import and passes on the arguments its caller left.  Only a
// Windows import may take ECX and EDX (__fastcall); an ELF import never
// takes a register argument.
//
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineCallAbiDetail.h"
#include "SourceCallExecution.h"
#include "WindowsQualifiedFixture.h"
#include "WindowsVariadicFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x401000;
constexpr va_t Iat = 0x403000;

/// The slot of import \p Index.
constexpr va_t slot(size_t Index) { return Iat + 4 * Index; }

/// `call dword ptr [Slot]`.
std::vector<uint8_t> callThroughSlot(va_t Slot) {
  return {0xFF,
          0x15,
          static_cast<uint8_t>(Slot),
          static_cast<uint8_t>(Slot >> 8),
          static_cast<uint8_t>(Slot >> 16),
          static_cast<uint8_t>(Slot >> 24)};
}

/// A PE32 image with \p Code at Text, the function `_wrap` there, and the
/// msvcrt.dll imports \p Imports in consecutive slots at Iat.
BinaryImage makeImage(std::vector<uint8_t> Code,
                      const std::vector<const char *> &Imports) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = Iat;
  Idata.Size = Idata.FileSz = 4 * Imports.size();
  Idata.Data.resize(Idata.Size);
  Idata.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Idata));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = Imports[I];
    Imp.IATAddr = slot(I);
    Img.Imports.push_back(std::move(Imp));
  }
  Symbol Function = Symbol::makeFunc(Text);
  Function.Name = "_wrap";
  Img.Symbols.push_back(std::move(Function));
  return Img;
}

TEST(I386CallContract, PhysicalCallReturnSlotsDoNotKeepOldSourceConstants) {
  for (bool Rewrite : {false, true})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      // PUSH 19 releases exactly the four bytes overwritten by the next CALL.
      std::vector<uint8_t> Code = {0x83, 0xec, 16, 0x6a, 19, 0x83, 0xc4, 4};
      const auto Call = callThroughSlot(Iat);
      Code.insert(Code.end(), Call.begin(), Call.end());
      const va_t ReturnPC = Text + Code.size();
      if (Rewrite)
        Code.insert(Code.end(), {0xc7, 0x44, 0x24, 0xfc, 19, 0, 0, 0});
      Code.insert(Code.end(), {0x8b, 0x44, 0x24, 0xfc, 0x83, 0xc4, 16, 0xc3});
      auto Image = makeImage(Code, {"rand"});
      Image.Format = Format;
      if (Format == BinaryFormat::ELF)
        Image.Symbols.front().Name = "wrap";
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Iat, "rand", 0, ImportStorageEvidence::LoaderBind));
      for (bool LLVM : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(testing::Message() << Rewrite << ':' << unsigned(Format)
                                          << ':' << LLVM << ':' << NoOpt);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Text};
          Options.LiftMode = LLVM;
          Options.SourceProjection = true;
          Options.NoOpt = NoOpt;
          const auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Format;
          COptions.Image = &Image;
          COptions.EmitIncludes = false;
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(
                LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
          } else
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Output, COptions));
          EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
          const std::string Standalone =
              "#include <stdint.h>\nint rand(void);\n" + Source;
          source_call_execution_test::checkSyntax(
              Standalone, {"--target=i686-pc-windows-msvc", "-ffreestanding"});
#if defined(__linux__) && (defined(__i386__) || defined(__x86_64__))
          std::string Oracle;
          llvm::raw_string_ostream Native(Oracle);
          Native << R"(
#include <sys/mman.h>
#include <string.h>
static unsigned calls;
int rand(void) { ++calls; return 13; }
static int check_native(void) {
  static const uint8_t original[] = {)";
          for (uint8_t Byte : Code)
            Native << unsigned(Byte) << ',';
          Native << "};\n  const size_t size = " << Iat - Text + 4 << ";\n"
                 << R"(
  uint8_t *mapping = mmap(0, size + 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) return 1;
  // UBSan reads its function-type prefix before an indirect call target.
  uint8_t *code = mapping + 16;
  memcpy(code, original, sizeof(original));
  uint32_t target = (uint32_t)(uintptr_t)rand;
)";
          Native << "  uint32_t slot = (uint32_t)(uintptr_t)(code + "
                 << Iat - Text << ");\n"
                 << "  memcpy(code + " << Iat - Text << ", &target, 4);\n"
                 << "  memcpy(code + " << ReturnPC - Text - 4
                 << ", &slot, 4);\n"
                 << "  uint32_t expected = "
                 << (Rewrite ? "19"
                             : "(uint32_t)(uintptr_t)(code + " +
                                   std::to_string(ReturnPC - Text) + ")")
                 << ";\n"
                 << R"(
  calls = 0;
  __builtin___clear_cache((char *)code, (char *)code + size);
  uint32_t actual = ((uint32_t (*)(void))code)();
  int bad = actual != expected || calls != 1;
  if (munmap(mapping, size + 16)) return 1;
  return bad;
}
int main(void) {
  if (check_native()) return 1;
  calls = 0;
  return (uint32_t)wrap() != )"
                 << (Rewrite ? 19 : ReturnPC) << " || calls != 1;\n}\n";
          for (llvm::StringRef Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                "#define _GNU_SOURCE\n" + Standalone + Oracle,
                {"-m32", Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
#else
          RecordProperty("native_i386_execution", "requires Linux x86");
#endif
        }
    }
}

TEST(I386CallContract, HeaderFloatingImportsDefineTheNormalizedX87Result) {
  class Header : public NullDebugContext {
  public:
    uint16_t Bytes = 8;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "fixture_scalar_fp")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.PrototypeFromHeader = true;
      Function.PrototypeLibrary = "test-header";
      Function.CallConv = DebugCallConv::Cdecl;
      Function.ReturnType = NdType::makeFloat(Bytes);
      Function.Params = {{"left", NdType::makeFloat(Bytes)},
                         {"right", NdType::makeFloat(Bytes)}};
      const std::string Type = Bytes == 4 ? "float" : "double";
      Function.PortablePrototypeDeclaration =
          Type + " fixture_scalar_fp(" + Type + " left, " + Type + " right)";
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      auto Function =
          Address == Iat ? resolvePrototype("fixture_scalar_fp") : std::nullopt;
      if (Function)
        Function->Addr = Address;
      return Function;
    }
  } Debug;
  for (uint16_t Bytes : {4u, 8u}) {
    Debug.Bytes = Bytes;
    for (bool StoreResult : {false, true}) {
      std::vector<uint8_t> Code;
      auto Push = [&](uint32_t Value) {
        Code.push_back(0x68);
        for (unsigned Byte = 0; Byte < 4; ++Byte)
          Code.push_back(static_cast<uint8_t>(Value >> (8 * Byte)));
      };
      if (Bytes == 8) {
        Push(0xC01A0000);
        Push(0);
        Push(0x40040000);
        Push(0);
      } else {
        Push(0xC0D00000);
        Push(0x40200000);
      }
      const auto Call = callThroughSlot(Iat);
      Code.insert(Code.end(), Call.begin(), Call.end());
      Code.insert(Code.end(), {0x83, 0xC4, uint8_t(2 * Bytes)});
      if (StoreResult) {
        // fstp/fld must see the actual post-call TOP, not a stale entry slot.
        Code.insert(Code.end(), {0x83, 0xEC, uint8_t(Bytes),
                                 uint8_t(Bytes == 8 ? 0xDD : 0xD9), 0x1C, 0x24,
                                 uint8_t(Bytes == 8 ? 0xDD : 0xD9), 0x04, 0x24,
                                 0x83, 0xC4, uint8_t(Bytes)});
      }
      Code.push_back(0xC3);
      auto Image = makeImage(Code, {"fixture_scalar_fp"});
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Iat, "fixture_scalar_fp", 0, ImportStorageEvidence::LoaderBind));
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(StoreResult);
        SCOPED_TRACE(LLVM);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text};
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_EQ(Result.MedFuncs.size(), 1u);
        EXPECT_TRUE(Result.MedFuncs.front().FPReturnViaX87);
        ASSERT_TRUE(Result.MedFuncs.front().ReturnType);
        EXPECT_EQ(Result.MedFuncs.front().ReturnType->Size, Bytes);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out,
                                               COptions, &Debug, &Image)
                         : HighCEmitter().emit(Result.HighFuncs, Out, COptions,
                                               &Debug));
        if (!StoreResult) {
          EXPECT_EQ(Source.find("_BitInt(80)"), std::string::npos) << Source;
          EXPECT_EQ(Source.find("long double"), std::string::npos) << Source;
        }
        const std::string Type = Bytes == 4 ? "float" : "double";
        source_call_execution_test::compileAndRun(
            Source + "\n" + Type + " fixture_scalar_fp(" + Type + " left, " +
            Type +
            " right) {\n"
            "  if (left != 2.5 || right != -6.5) __builtin_trap();\n"
            "  return 42.25;\n}\n"
            "int main(void) { return wrap() != 42.25; }\n");
      }
    }
  }
}

TEST(I386CallContract, HeaderQualifiersKeepTheOuterCallingConvention) {
  auto Code = callThroughSlot(Iat);
  Code[1] = 0x25; // jmp [IAT], forwarding the two incoming stack arguments.
  auto Image = makeImage(Code, {"fixture_qualified"});
  ASSERT_TRUE(Image.recordImportStorageSlot(Iat, "fixture_qualified", 0,
                                            ImportStorageEvidence::LoaderBind));
  for (bool Stdcall : {false, true})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Stdcall);
      SCOPED_TRACE(LLVM);
      windows_qualified_test::Header Debug;
      Debug.Slot = Iat;
      Debug.PointerBytes = 4;
      Debug.Convention =
          Stdcall ? DebugCallConv::Stdcall : DebugCallConv::Cdecl;
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = LLVM;
      Options.SourceProjection = LLVM;
      Options.OnlyFunctionEntries = {Text};
      auto Result = Pipeline().run(Image, Context, Options, &Debug);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      EXPECT_EQ(Result.MedFuncs.front().Params.size(), 2u);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      COptions.EmitIncludes = false;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                        &Debug, &Image));
      } else {
        ASSERT_TRUE(
            HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
      }
      const std::string Convention = Stdcall ? "__attribute__((stdcall)) " : "";
      source_call_execution_test::checkSyntax(
          "#include <stdint.h>\n" + Source + "\ntypedef const uint16_t *(" +
              Convention +
              "*Expected)(const uint16_t *, volatile uint32_t *);\n"
              "_Static_assert(__builtin_types_compatible_p("
              "__typeof__(&fixture_qualified), Expected), \"qualified "
              "ABI\");\n",
          {"--target=i686-pc-windows-msvc", "-ffreestanding", "-Werror"});
    }
}

TEST(I386CallContract, LLVMCCallbackFactoryKeepsBothCallingConventions) {
  class Prototypes : public NullDebugContext {
  public:
    FunctionSym Signature;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      return Name == "SetHandler" ? std::optional(Signature) : std::nullopt;
    }
  } Debug;
  for (bool OuterStdcall : {false, true})
    for (bool InnerStdcall : {false, true}) {
      SCOPED_TRACE(OuterStdcall);
      SCOPED_TRACE(InnerStdcall);
      auto Pointer = NdType::makePtr(NdType::makeVoid());
      Pointer->Size = 4;
      auto Callback = NdType::makePtr(NdType::makeFunc(
          NdType::makeInt(4, true), {Pointer, NdType::makeInt(4, false)},
          InnerStdcall ? types::CallingConvention::Stdcall
                       : types::CallingConvention::Unspecified));
      Callback->Size = 4;
      Debug.Signature.Name = "SetHandler";
      Debug.Signature.PrototypeLibrary = "synthetic-windows";
      Debug.Signature.ReturnType = Callback;
      Debug.Signature.Params = {{"Handler", Callback}};
      Debug.Signature.CallConv =
          OuterStdcall ? DebugCallConv::Stdcall : DebugCallConv::Cdecl;
      llvm::LLVMContext Context;
      llvm::Module Module("callback-factory", Context);
      Module.setDataLayout("e-p:32:32");
      llvm::IRBuilder<> Builder(Context);
      auto *FunctionType = llvm::FunctionType::get(
          Builder.getInt32Ty(), {Builder.getInt32Ty()}, false);
      auto *Factory = llvm::Function::Create(FunctionType,
                                             llvm::GlobalValue::ExternalLinkage,
                                             "_SetHandler", Module);
      Factory->setCallingConv(OuterStdcall ? llvm::CallingConv::X86_StdCall
                                           : llvm::CallingConv::C);
      auto *Wrapper = llvm::Function::Create(
          FunctionType, llvm::GlobalValue::ExternalLinkage, "_wrap", Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Wrapper));
      auto *Call = Builder.CreateCall(Factory, {Wrapper->getArg(0)});
      Call->setCallingConv(Factory->getCallingConv());
      Builder.CreateRet(Call);
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Arch::X86;
      Options.Format = BinaryFormat::COFF;
      Options.EmitIncludes = false;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options, &Debug));
      ASSERT_NE(Source.find("SetHandler("), std::string::npos) << Source;
      const std::string Inner = InnerStdcall ? "__attribute__((stdcall)) " : "";
      const std::string Outer = OuterStdcall ? "__attribute__((stdcall)) " : "";
      source_call_execution_test::checkSyntax(
          "#include <stdint.h>\n" + Source + "\ntypedef int32_t (" + Inner +
              "*ExpectedCallback)(void*, uint32_t);\n"
              "typedef ExpectedCallback (" +
              Outer +
              "*ExpectedFactory)(ExpectedCallback);\n"
              "_Static_assert(__builtin_types_compatible_p("
              "__typeof__(&SetHandler), ExpectedFactory), \"factory ABI\");\n",
          {"--target=i686-pc-windows-msvc", "-ffreestanding", "-Werror"});
    }
}

TEST(I386CallContract, OmittedImportStackFactsDoNotDependOnTheSelection) {
  constexpr va_t Other = Text + 0x40;
  constexpr va_t Child = Text + 0x80;
  std::vector<uint8_t> Code(0x100, 0xCC);
  auto CallChild = [&](std::vector<uint8_t> &Body, va_t Entry) {
    const uint32_t Displacement =
        static_cast<uint32_t>(Child - (Entry + Body.size() + 5));
    Body.push_back(0xE8);
    for (unsigned Byte = 0; Byte != 4; ++Byte)
      Body.push_back(static_cast<uint8_t>(Displacement >> (8 * Byte)));
  };
  // caller_a(p): child(p). caller_b(p): memcpy(p,p,0); child(p).
  std::vector<uint8_t> First = {0x8B, 0x44, 0x24, 4, 0x50};
  CallChild(First, Text);
  First.insert(First.end(), {0x83, 0xC4, 4, 0xC3});
  std::copy(First.begin(), First.end(), Code.begin());
  std::vector<uint8_t> Second = {0x8B, 0x44, 0x24, 4, 0x6A, 0, 0x50, 0x50};
  const auto ImportCall = callThroughSlot(Iat);
  Second.insert(Second.end(), ImportCall.begin(), ImportCall.end());
  Second.insert(Second.end(), {0x83, 0xC4, 12, 0x8B, 0x44, 0x24, 4, 0x50});
  CallChild(Second, Other);
  Second.insert(Second.end(), {0x83, 0xC4, 4, 0xC3});
  std::copy(Second.begin(), Second.end(), Code.begin() + 0x40);
  // child(p): memcpy(p+4,p,4). Its import is absent from caller_a's body.
  std::vector<uint8_t> Body = {0x8B, 0x44, 0x24, 4,    0x8D, 0x50,
                               4,    0x6A, 4,    0x50, 0x52};
  Body.insert(Body.end(), ImportCall.begin(), ImportCall.end());
  Body.insert(Body.end(), {0x83, 0xC4, 12, 0xC3});
  std::copy(Body.begin(), Body.end(), Code.begin() + 0x80);
  for (const BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (const bool Warm : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Format));
      SCOPED_TRACE(Warm);
      BinaryImage Image = makeImage(Code, {"memcpy"});
      Image.Format = Format;
      for (const auto &[Entry, Name] :
           {std::pair{Other, "caller_b"}, {Child, "child"}}) {
        auto Function = Symbol::makeFunc(Entry);
        Function.Name = Name;
        Image.Symbols.push_back(std::move(Function));
      }
      PipelineAnalysisCache Cache;
      PipelineOptions Options;
      Options.AnalysisCache = &Cache;
      Options.EmitDumpOutput = false;
      if (Warm)
        ASSERT_TRUE(
            Pipeline().warmAnalysisCache(Image, Options, nullptr, {Child}));
      llvm::LLVMContext Context;
      for (const va_t Entry : {Text, Other, Text}) {
        Options.OnlyFunctionEntries = {Entry};
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_TRUE(Result.CalleeSignatureMemo);
        const auto *Read = Result.CalleeSignatureMemo->find(Child);
        ASSERT_TRUE(Read);
        ASSERT_EQ(Read->Conversions.size(), 1u);
        ASSERT_TRUE(Read->Conversions.front().Body);
        EXPECT_EQ(Read->Conversions.front().Body->Params.size(), 1u);
      }
    }
}

/// The call of \p Callee in the HighC of `wrap` in \p Img: the rest of its
/// line from the callee's name.
std::string callIn(const BinaryImage &Img, const std::string &Callee) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const size_t Body = Source.find(" wrap(");
  const size_t At = Source.find(Callee + "(", Body);
  if (Body == std::string::npos || At == std::string::npos) {
    ADD_FAILURE() << Source;
    return {};
  }
  return Source.substr(At, Source.find('\n', At) - At);
}

TEST(I386CallContract, WindowsNonlocalCallsRetainTheirControlFlowContract) {
  for (const char *Name :
       {"_setjmp3", "_setjmpex", "__intrinsic_setjmp", "__intrinsic_setjmpex",
        "__mingw_setjmp", "_longjmpex", "__mingw_longjmp", "opaque_call"})
    for (bool NoOpt : {false, true})
      for (bool RegisterCall : {false, true}) {
        SCOPED_TRACE(Name);
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(RegisterCall);
        const bool NoReturn =
            std::string_view(Name).find("longjmp") != std::string_view::npos;
        const bool ReturnsTwice =
            !NoReturn && std::string_view(Name) != "opaque_call";
        // Two initialized outgoing words, an import call, and an observable
        // ordinary continuation. Only the longjmp entries cut that edge.
        std::vector<uint8_t> Code{0x6a, 0, 0x6a, 0};
        auto Call = callThroughSlot(slot(0));
        if (RegisterCall) {
          Call[0] = 0x8b; // mov ecx,[slot]; call ecx
          Call[1] = 0x0d;
          Call.insert(Call.end(), {0xff, 0xd1});
        }
        Code.insert(Code.end(), Call.begin(), Call.end());
        const va_t Continuation = Text + Code.size();
        Code.insert(Code.end(), {0x83, 0xc4, 8, 0xb8, 7, 0, 0, 0, 0xc3});
        auto Image = makeImage(std::move(Code), {Name});
        ASSERT_TRUE(Image.recordImportStorageSlot(
            slot(0), Name, 0, ImportStorageEvidence::ImportDirectory));
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        Options.OnlyFunctionEntries = {Text};
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        bool SawContinuation = false;
        for (const auto &Function : Result.LowFuncs)
          for (const auto &Block : Function.Blocks)
            for (const auto &Boundary : Block.InstructionBoundaries)
              SawContinuation |= Boundary.Address == Continuation;
        if (!RegisterCall)
          EXPECT_EQ(SawContinuation, !NoReturn);
        const llvm::CallBase *ObservedCall = nullptr;
        for (const auto &Function : *Result.LlvmModule)
          for (const auto &Block : Function)
            for (const auto &Instruction : Block)
              if (const auto *Call =
                      llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
                if (const auto *Target = Call->getCalledFunction();
                    Target && Target->isIntrinsic())
                  continue;
                if (llvm_source_unknown::isValue(Call)) {
                  // The continuation overwrites the actual result and does
                  // not observe any volatile register's remaining state.
                  EXPECT_TRUE(Call->use_empty());
                  EXPECT_FALSE(Call->hasFnAttr(llvm::Attribute::ReturnsTwice));
                  EXPECT_FALSE(Call->hasFnAttr(llvm::Attribute::NoReturn));
                  continue;
                }
                ASSERT_EQ(ObservedCall, nullptr);
                ObservedCall = Call;
              }
        ASSERT_NE(ObservedCall, nullptr);
        EXPECT_EQ(ObservedCall->hasFnAttr(llvm::Attribute::NoReturn), NoReturn);
        EXPECT_EQ(ObservedCall->hasFnAttr(llvm::Attribute::ReturnsTwice),
                  ReturnsTwice);
      }
}

TEST(I386CallContract, AThunkJumpingThroughTheSlotForwardsItsArguments) {
  // `jmp dword ptr [__imp__calloc]`: the thunk calls calloc, not the value
  // it would load from an address, with the two arguments its caller left
  // on the stack.
  std::vector<uint8_t> Code = {0xFF,
                               0x25, // jmp dword ptr [slot]
                               static_cast<uint8_t>(slot(0)),
                               static_cast<uint8_t>(slot(0) >> 8),
                               static_cast<uint8_t>(slot(0) >> 16),
                               static_cast<uint8_t>(slot(0) >> 24)};
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_NE(Call.find("arg0"), std::string::npos) << Call;
  EXPECT_NE(Call.find("arg1"), std::string::npos) << Call;
  EXPECT_EQ(Call.find("unknown value"), std::string::npos) << Call;
}

namespace {
/// \p Code, at most 0x10 bytes at Text, which calls the thunk `_Validate@8`
/// an import library links at Text + 0x10, `jmp dword ptr
/// [__imp__Validate@8]`.
BinaryImage imageWithStdcallThunk(std::vector<uint8_t> Code) {
  EXPECT_LE(Code.size(), 0x10u);
  Code.resize(0x10, 0xCC);
  for (uint8_t B : {0xFF, 0x25, // jmp dword ptr [slot]
                    static_cast<int>(slot(0) & 0xFF),
                    static_cast<int>((slot(0) >> 8) & 0xFF),
                    static_cast<int>((slot(0) >> 16) & 0xFF),
                    static_cast<int>(slot(0) >> 24)})
    Code.push_back(B);
  BinaryImage Img = makeImage(std::move(Code), {"Validate"});
  Img.Imports[0].Module = "kernelbase.dll";
  Symbol Thunk = Symbol::makeFunc(Text + 0x10);
  Thunk.Name = "_Validate@8";
  Img.Symbols.push_back(std::move(Thunk));
  return Img;
}

/// The HighC of `wrap` in \p Img, its declarations included, or its LLVM-C.
std::string emitSource(const BinaryImage &Img, bool LLVMC = false) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  Opts.LiftMode = LLVMC;
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!LLVMC)
    EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  else if (!Result.LlvmModule)
    ADD_FAILURE() << "no LLVM module";
  else
    EXPECT_TRUE(
        LLVMCEmitter().emit(*Result.LlvmModule, OS, Options, nullptr, &Img));
  return Source;
}

/// The HighC of `wrap` in \p Img.
std::string wrapSource(const BinaryImage &Img) {
  const std::string Source = emitSource(Img);
  return Source.substr(std::min(Source.size(), Source.find(" wrap(")));
}
} // namespace

TEST(I386CallContract, AStdcallThunkTakesTheSlotsItsNameSpells) {
  // GCC loads an argument into EDX before it stores it, as MinGW's kernel32
  // calls kernelbase's undocumented NlsValidateLocale: no header declares
  // Validate, but its thunk's decoration says it pops two slots, so the
  // call passes those and no register.
  const std::string Call =
      callIn(imageWithStdcallThunk({0x8B, 0x54, 0x24, 0x04, // mov edx, [esp+4]
                                    0x6A, 0x00,             // push 0
                                    0x52,                   // push edx
                                    0xE8, 0x04, 0x00, 0x00,
                                    0x00,   // call Text + 0x10
                                    0xC3}), // ret
             "Validate");
  EXPECT_NE(Call.find("Validate(arg0, 0)"), std::string::npos) << Call;
}

TEST(I386CallContract, AStdcallImportPopsItsArguments) {
  // The caller reads its own argument at [esp+4] after the call: the stack
  // pointer is where it was before the pushes, since Validate popped them.
  const std::string Source = wrapSource(
      imageWithStdcallThunk({0x6A, 0x07,             // push 7
                             0xFF, 0x74, 0x24, 0x08, // push [esp+8]
                             0xE8, 0x05, 0x00, 0x00,
                             0x00,                   // call Text + 0x10
                             0x03, 0x44, 0x24, 0x04, // add eax, [esp+4]
                             0xC3}));                // ret
  EXPECT_NE(Source.find("Validate(arg0, 7)"), std::string::npos) << Source;
  const size_t Return = Source.find("return ");
  ASSERT_NE(Return, std::string::npos) << Source;
  const std::string Result =
      Source.substr(Return, Source.find(';', Return) - Return);
  // Without the pop the read lands on the pushed 7.
  EXPECT_NE(Result.find("arg0"), std::string::npos) << Source;
  EXPECT_EQ(Result.find("+ 7"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}

TEST(I386CallContract, HeaderRecordStdcallKeepsItsStackCleanup) {
  class Header : public NullDebugContext {
  public:
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "RecordStatus")
        return std::nullopt;
      auto Record = NdType::makeNamedRecord("Record", 8);
      Record->Alignment = 8;
      Record->Passing = NdRecordPassing::ByValue;
      Record->ScalarLeaves = {{0, 4, false}, {4, 4, false}};
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeInt(4, true);
      Function.Params = {{"Value", Record}};
      Function.CallConv = DebugCallConv::Stdcall;
      Function.PrototypeLibrary = "test-windows-header";
      Function.PrototypeFromHeader = true;
      return Function;
    }
  } Debug;
  // The record is two stack words. The load after the call must read the
  // wrapper's original argument, not the record word left at the old SP.
  std::vector<uint8_t> Code = {0x6A, 7, 0x6A, 3};
  const auto Call = callThroughSlot(Iat);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(), {0x8B, 0x44, 0x24, 4, 0xC3});
  BinaryImage Image = makeImage(Code, {"RecordStatus"});
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  const auto Result = Pipeline().run(Image, Context, Options, &Debug);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.CallEntryStackPops.count(Iat));
  EXPECT_EQ(Result.CallEntryStackPops.at(Iat), 8);
  CEmitterOptions COptions;
  COptions.TheArch = Image.Arch;
  COptions.Format = Image.Format;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, COptions));
  EXPECT_NE(Source.find("return arg0;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}

TEST(I386CallContract, AStdcallImportIsDeclaredToPopItsArguments) {
  // C compiled from the output leaves the two slots Validate pops to it
  // only by a stdcall declaration: HighC's counts the slots its thunk's
  // name spells, LLVM-C's types them as its call passes them.
  const BinaryImage Img =
      imageWithStdcallThunk({0x6A, 0x07,             // push 7
                             0xFF, 0x74, 0x24, 0x08, // push [esp+8]
                             0xE8, 0x05, 0x00, 0x00,
                             0x00,                   // call Text + 0x10
                             0x03, 0x44, 0x24, 0x04, // add eax, [esp+4]
                             0xC3});                 // ret
  const std::string HighC = emitSource(Img);
  EXPECT_NE(HighC.find("extern __attribute__((stdcall)) int "
                       "Validate(int32_t, int32_t);"),
            std::string::npos)
      << HighC;
  const std::string LLVMC = emitSource(Img, /*LLVMC=*/true);
  EXPECT_NE(LLVMC.find("__attribute__((stdcall)) uint64_t "
                       "Validate(uint32_t, uint32_t);"),
            std::string::npos)
      << LLVMC;
}

TEST(I386CallContract, ArgumentsStoredBelowOneAdjustmentAreAllPassed) {
  // MinGW's __getmainargs wrapper: GCC stores its fifth parameter for the
  // call first, then zeroes a local, and computes the other four arguments
  // in more ops than a fixed window of them reaches.
  std::vector<uint8_t> Code = {
      0x55,                                     // push ebp
      0x89, 0xE5,                               // mov ebp, esp
      0x53,                                     // push ebx
      0x83, 0xEC, 0x34,                         // sub esp, 0x34
      0x8B, 0x45, 0x18,                         // mov eax, [ebp+0x18]
      0xC7, 0x45, 0xEC, 0xFF, 0xFF, 0xFF, 0xFF, // mov [ebp-0x14], -1
      0xC7, 0x45, 0xF0, 0x00, 0x00, 0x00, 0x00, // mov [ebp-0x10], 0
      0x89, 0x44, 0x24, 0x10,                   // mov [esp+0x10], eax
      0x8B, 0x45, 0x14,                         // mov eax, [ebp+0x14]
      0xC7, 0x45, 0xF4, 0x00, 0x00, 0x00, 0x00, // mov [ebp-0xc], 0
      0x89, 0x44, 0x24, 0x0C,                   // mov [esp+0xc], eax
      0x8D, 0x45, 0xF4,                         // lea eax, [ebp-0xc]
      0x89, 0x44, 0x24, 0x08,                   // mov [esp+8], eax
      0x8D, 0x45, 0xF0,                         // lea eax, [ebp-0x10]
      0x89, 0x44, 0x24, 0x04,                   // mov [esp+4], eax
      0x8D, 0x45, 0xEC,                         // lea eax, [ebp-0x14]
      0x89, 0x04, 0x24,                         // mov [esp], eax
  };
  for (uint8_t B : callThroughSlot(slot(0)))
    Code.push_back(B);
  for (uint8_t B : {0x8B, 0x45, 0xEC, // mov eax, [ebp-0x14]
                    0x8B, 0x5D, 0xFC, // mov ebx, [ebp-4]
                    0xC9,             // leave
                    0xC3})            // ret
    Code.push_back(B);
  const std::string Call =
      callIn(makeImage(Code, {"__getmainargs"}), "__getmainargs");
  EXPECT_NE(Call.find("arg3, "), std::string::npos) << Call;
  EXPECT_NE(Call.find("arg4)"), std::string::npos) << Call;
}

TEST(I386CallContract, AStoreToALocalBetweenPushesIsNoArgument) {
  // calloc(n, size) pushed from the parameters, with a local written
  // between the two pushes.
  std::vector<uint8_t> Code = {
      0x55,                                     // push ebp
      0x89, 0xE5,                               // mov ebp, esp
      0x83, 0xEC, 0x08,                         // sub esp, 8
      0xFF, 0x75, 0x0C,                         // push [ebp+0xc]
      0xC7, 0x45, 0xF8, 0x07, 0x00, 0x00, 0x00, // mov [ebp-8], 7
      0xFF, 0x75, 0x08,                         // push [ebp+8]
  };
  for (uint8_t B : callThroughSlot(slot(0)))
    Code.push_back(B);
  for (uint8_t B : {0x83, 0xC4, 0x08, // add esp, 8
                    0xC9,             // leave
                    0xC3})            // ret
    Code.push_back(B);
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_NE(Call.find("(arg0, arg1)"), std::string::npos) << Call;
  EXPECT_EQ(Call.find('7'), std::string::npos) << Call;
}

TEST(I386CallContract, APoppedPushIsNoArgument) {
  // clang's get-PC `call $+5; pop ebx` pushes the return address and pops
  // it, and `push eax` reserves a slot: a regparm callee that takes ECX gets
  // neither as its argument.
  std::vector<uint8_t> Code = {0x53,                         // push ebx
                               0x50,                         // push eax
                               0xE8, 0x00, 0x00, 0x00, 0x00, // call $+5
                               0x5B,                         // pop ebx
                               0x8D, 0x4C, 0x49, 0x01, // lea ecx, [ecx+ecx*2+1]
                               0xE8, 0x08, 0x00, 0x00, 0x00, // call helper
                               0x83, 0xC4, 0x04,             // add esp, 4
                               0x5B,                         // pop ebx
                               0xC3,                         // ret
                               0xCC, 0xCC, 0xCC,             // padding
                               0x8D, 0x04, 0x09, // helper: lea eax, [ecx+ecx]
                               0xC3};            // ret
  BinaryImage Img = makeImage(Code, {});
  Symbol Helper = Symbol::makeFunc(Text + 0x19);
  Helper.Name = "_helper";
  Img.Symbols.push_back(std::move(Helper));
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text, Text + 0x19};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const size_t Call = Source.find("helper(", Source.find(" wrap("));
  ASSERT_NE(Call, std::string::npos) << Source;
  const std::string Line = Source.substr(Call, Source.find('\n', Call) - Call);
  // 0x401007 is the address `call $+5` pushes.
  EXPECT_EQ(Line.find("0x401007"), std::string::npos) << Source;
  EXPECT_EQ(Line.find("4198407"), std::string::npos) << Source;
  EXPECT_NE(Line.find("arg0"), std::string::npos) << Source;
}

TEST(I386CallContract, SelectingTheCallerPreservesTheCalleeArgumentContract) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Regparm : {false, true})
      for (bool Forwarder : {false, true})
        for (bool Selected : {false, true})
          for (bool NoOpt : {false, true})
            for (bool LLVM : {false, true}) {
              SCOPED_TRACE(std::to_string(static_cast<int>(Format)) + "/" +
                           std::to_string(Regparm) + "/" +
                           std::to_string(Forwarder) + "/" +
                           std::to_string(Selected) + "/" +
                           std::to_string(NoOpt) + "/" + std::to_string(LLVM));
              // Both register and stack setup are present. The callee's
              // actual reads decide which two values the call passes. ECX
              // is scratch, despite being first in the default register order.
              std::vector<uint8_t> Code = {
                  0xb8, 7,    0,    0,   0, // mov eax,7
                  0xba, 11,   0,    0,   0, // mov edx,11
                  0xb9, 99,   0,    0,   0, // mov ecx,99
                  0x6a, 47,   0x6a, 31,     // push 47; push 31
                  0xeb, 0,                  // jmp call_block
                  0xe8, 0x26, 0,    0,   0, // call helper at +0x40
                  0x83, 0xc4, 8,    0xc3};  // add esp,8; ret
              Code.resize(0x40, 0xcc);
              if (Forwarder) {
                Code.insert(Code.end(), {0xe9, 0x1b, 0, 0, 0});
                Code.resize(0x60, 0xcc);
              }
              const std::vector<uint8_t> Leaf =
                  Regparm ? std::vector<uint8_t>{0x8d, 0x04, 0x10, 0xc3}
                          : std::vector<uint8_t>{0x8b, 0x44, 0x24, 4,   0x03,
                                                 0x44, 0x24, 8,    0xc3};
              Code.insert(Code.end(), Leaf.begin(), Leaf.end());
              BinaryImage Img = makeImage(std::move(Code), {});
              Img.Format = Format;
              for (va_t Entry : {Text + 0x40, Text + 0x60}) {
                if (Entry == Text + 0x60 && !Forwarder)
                  continue;
                Symbol S = Symbol::makeFunc(Entry);
                S.Name = Entry == Text + 0x40 ? "helper" : "leaf";
                Img.Symbols.push_back(std::move(S));
              }
              llvm::LLVMContext Ctx;
              PipelineOptions Opts;
              Opts.EmitDumpOutput = false;
              Opts.NoOpt = NoOpt;
              Opts.LiftMode = LLVM;
              Opts.SourceProjection = true;
              Opts.OnlyFunctionEntries = {Text};
              if (!Selected) {
                Opts.OnlyFunctionEntries.insert(Text + 0x40);
                if (Forwarder)
                  Opts.OnlyFunctionEntries.insert(Text + 0x60);
              }
              const PipelineResult R = Pipeline().run(Img, Ctx, Opts);
              ASSERT_TRUE(R.Success) << R.Error;
              ASSERT_EQ(R.MedFuncs.size(), Selected ? 1u : Forwarder ? 3u : 2u);
              const MedFunc *Caller = nullptr;
              for (const MedFunc &F : R.MedFuncs)
                if (F.Entry == Text)
                  Caller = &F;
              ASSERT_NE(Caller, nullptr);
              const MedCallInfo *Call = nullptr;
              for (const MedCallInfo &CI : Caller->CallInfos)
                if (CI.TargetAddr == Text + 0x40)
                  Call = &CI;
              ASSERT_NE(Call, nullptr);
              if (Selected || !LLVM)
                ASSERT_EQ(Call->Args.size(), 2u);
              else
                ASSERT_GE(Call->Args.size(), 2u);
              // Follow only representation copies of the two literal inputs;
              // this oracle does not recover register or stack arguments.
              auto literal = [&](MedVar V) -> std::optional<uint64_t> {
                for (unsigned Depth = 0; Depth != 16; ++Depth) {
                  if (V.isConst())
                    return V.ConstVal;
                  const MedOp *Def = nullptr;
                  for (const MedBlock &B : Caller->Blocks)
                    for (const MedOp &O : B.Ops)
                      if (O.Output.Kind == V.Kind && O.Output.Id == V.Id &&
                          O.Output.SSAVer == V.SSAVer && O.Output.Size &&
                          (O.Opcode == NdOp::COPY ||
                           O.Opcode == NdOp::INT_ZEXT))
                        Def = &O;
                  if (!Def || Def->NumInputs != 1)
                    return std::nullopt;
                  V = Def->Inputs[0];
                }
                return std::nullopt;
              };
              EXPECT_EQ(literal(Call->Args[0]), Regparm ? 7u : 31u);
              EXPECT_EQ(literal(Call->Args[1]), Regparm ? 11u : 47u);
              if (LLVM) {
                ASSERT_NE(R.LlvmModule, nullptr);
                EXPECT_FALSE(llvm::verifyModule(*R.LlvmModule, &llvm::errs()));
              }
            }
}

TEST(I386CallContract, StoresThroughACopyOfESPAreArgumentsByOffset) {
  // clang -O0 addresses the outgoing area through a copy of ESP and stores
  // in any order, and keeps a computed argument in a callee-saved register.
  std::vector<uint8_t> Code = {
      0x55,             // push ebp
      0x89, 0xE5,       // mov ebp, esp
      0x56,             // push esi
      0x83, 0xEC, 0x14, // sub esp, 0x14
      0x8B, 0x45, 0x08, // mov eax, [ebp+8]
      0x8B, 0x75, 0x0C, // mov esi, [ebp+0xc]
      0x89, 0xE2,       // mov edx, esp
      0x89, 0x02,       // mov [edx], eax
      0x89, 0x72, 0x04, // mov [edx+4], esi
  };
  for (uint8_t B : callThroughSlot(slot(0)))
    Code.push_back(B);
  for (uint8_t B : {0x83, 0xC4, 0x14, // add esp, 0x14
                    0x5E,             // pop esi
                    0x5D,             // pop ebp
                    0xC3})            // ret
    Code.push_back(B);
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_NE(Call.find("(arg0, arg1)"), std::string::npos) << Call;
}

TEST(I386CallContract, AStoreWiderThanASlotFillsEachSlotItCovers) {
  for (bool RegisterTarget : {false, true})
    for (int Overwrite : {-1, 0, 1}) {
      // The MOVSD is a transport of two size_t arguments, not a double
      // argument. A later word store changes only its own argument.
      std::vector<uint8_t> Code = {
          0x55,                         // push ebp
          0x89, 0xE5,                   // mov ebp, esp
          0x83, 0xEC, 0x18,             // sub esp, 0x18
          0xF2, 0x0F, 0x10, 0x45, 0x08, // movsd xmm0, [ebp+8]
          0x89, 0xE0,                   // mov eax, esp
          0xF2, 0x0F, 0x11, 0x00,       // movsd [eax], xmm0
      };
      if (Overwrite >= 0)
        Code.insert(Code.end(), {0xC7, 0x40, uint8_t(Overwrite * 4), 0xEF, 0xBE,
                                 0xAD, 0xDE});
      auto Call = callThroughSlot(slot(0));
      if (RegisterTarget) {
        Call.erase(Call.begin(), Call.begin() + 2);
        Call.insert(Call.begin(), 0xa1);       // mov eax, [Iat]
        Call.insert(Call.end(), {0xff, 0xd0}); // call eax
      }
      Code.insert(Code.end(), Call.begin(), Call.end());
      Code.insert(Code.end(), {0x83, 0xC4, 0x18, 0x5D, 0xC3});
      auto Image = makeImage(Code, {"calloc"});
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Iat, "calloc", 0, ImportStorageEvidence::LoaderBind));
      for (bool LLVM : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(testing::Message() << RegisterTarget << ':' << Overwrite
                                          << ':' << LLVM << ':' << NoOpt);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Text};
          Options.LiftMode = LLVM;
          Options.SourceProjection = true;
          Options.NoOpt = NoOpt;
          const auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_EQ(Result.MedFuncs.size(), 1u);
          const auto &Function = Result.MedFuncs.front();
          ASSERT_EQ(Function.CallInfos.size(), 1u);
          const auto &Arguments = Function.CallInfos.front().Args;
          ASSERT_EQ(Arguments.size(), 2u);
          for (const auto &Argument : Arguments)
            EXPECT_EQ(Argument.Size, 4u);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          COptions.EmitIncludes = false;
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(
                LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
          } else
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Output, COptions));
          Source = "#include <stdint.h>\n#include <stddef.h>\n"
                   "void *calloc(size_t, size_t);\n" +
                   Source;
          EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
          source_call_execution_test::checkSyntax(
              Source, {"--target=i686-pc-windows-msvc", "-ffreestanding"});
#if defined(__linux__) && (defined(__i386__) || defined(__x86_64__))
          const std::string Oracle =
              "\n#undef calloc\nstatic unsigned calls;\n"
              "void *fixture_pair(size_t first, size_t second) {\n"
              "  ++calls;\n"
              "  return (void *)(uintptr_t)(first == " +
              std::string(Overwrite == 0 ? "0xDEADBEEFu" : "0x12345678u") +
              " && second == " +
              std::string(Overwrite == 1 ? "0xDEADBEEFu" : "0x76543210u") +
              " ? 37 : 41);\n}\n"
              "int main(void) { return wrap(0x12345678, 0x76543210) != 37 || "
              "calls != 1; }\n";
          for (llvm::StringRef Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                "#define calloc fixture_pair\n" + Source + Oracle,
                {"-m32", Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
#else
          RecordProperty("native_i386_execution", "requires Linux x86");
#endif
        }
    }
}

/// The HighC signature of the function at Text of \p Img, decompiled alone.
std::string signatureOf(const BinaryImage &Img, const std::string &Name) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const size_t At = Source.find(" " + Name + "(");
  if (At == std::string::npos) {
    ADD_FAILURE() << Source;
    return {};
  }
  const size_t Line = Source.rfind('\n', At) + 1;
  return Source.substr(Line, Source.find('\n', At) - Line);
}

TEST(I386CallContract, ARuntimeDoubleWordRoutineReturnsThePairAlone) {
  // libgcc's __udivdi3 returns its quotient in EDX:EAX.  Decompiled alone no
  // caller reads EDX after it, and its name says it returns the pair.
  const std::vector<uint8_t> Code = {0x8B, 0x44, 0x24, 0x04, // mov eax, [esp+4]
                                     0x8B, 0x54, 0x24, 0x08, // mov edx, [esp+8]
                                     0xC3};                  // ret
  BinaryImage Img = makeImage(Code, {});
  Img.Symbols.front().Name = "___udivdi3";
  const std::string Signature = signatureOf(Img, "__udivdi3");
  EXPECT_NE(Signature.find("64_t __udivdi3("), std::string::npos) << Signature;
  // Without that name nothing shows the high half is a result.
  const std::string Wrap = signatureOf(makeImage(Code, {}), "wrap");
  EXPECT_EQ(Wrap.find("64_t wrap("), std::string::npos) << Wrap;
}

TEST(I386CallContract, AStubOfAVariadicImportPassesItsArgumentsOn) {
  // `jmp dword ptr [__imp__fprintf]` passes every argument on, the variadic
  // ones too.  C passes no `...` on, so the stub hands them to vfprintf, which
  // takes them as fprintf does.
  const std::vector<uint8_t> Code = {0xFF,
                                     0x25, // jmp dword ptr [slot]
                                     static_cast<uint8_t>(slot(0)),
                                     static_cast<uint8_t>(slot(0) >> 8),
                                     static_cast<uint8_t>(slot(0) >> 16),
                                     static_cast<uint8_t>(slot(0) >> 24)};
  const BinaryImage Img = makeImage(Code, {"fprintf"});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  EXPECT_NE(Source.find("wrap jumps to the import fprintf"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("#include <stdarg.h>"), std::string::npos) << Source;
  EXPECT_NE(Source.find("int wrap(FILE *arg0, const char *format, ...) {"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("vfprintf(arg0, format, arguments);"),
            std::string::npos)
      << Source;
}

} // namespace

namespace {

MedVar register32(uint64_t Offset, int Version) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Offset);
  V.SSAVer = Version;
  V.Size = 4;
  V.RegOff = Offset;
  V.TheArch = Arch::X86;
  return V;
}

MedVar temporary32(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 4;
  V.TheArch = Arch::X86;
  return V;
}

void copyInto(MedBlock &Block, MedVar Output, MedVar Input) {
  MedOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = Output;
  Op.addInput(Input);
  Block.Ops.push_back(Op);
}

/// The call-site arguments of a caller that leaves EDX set in the block of a
/// call to the import `routine` at \p Slot, an import whose signature no
/// table knows, and ECX set in the block before it, with \p Format; and
/// whether the caller gained a parameter.
std::pair<std::vector<MedVar>, bool> importCallArguments(BinaryFormat Format,
                                                         bool SetECX) {
  constexpr va_t Slot = 0x5000;
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "caller";
  Func.Blocks.resize(2);
  Func.Blocks[0].Id = 0;
  Func.Blocks[0].Succs = {1};
  Func.Blocks[1].Id = 1;
  Func.Blocks[1].Preds = {0};
  if (SetECX)
    copyInto(Func.Blocks[0], register32(x86reg::RCX, 1), temporary32(1));
  copyInto(Func.Blocks[1], register32(x86reg::RDX, 1), temporary32(2));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = register32(x86reg::RAX, 1);
  Call.addInput(MedVar::makeConst(Slot, 4));
  Func.Blocks[1].Ops.push_back(Call);

  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = Format;
  Img.Imports.push_back({"libroutine", "routine", 0, Slot});
  const std::map<va_t, std::string> Names{{Slot, "routine"}};
  recoverCallAbi(Func, Arch::X86, Names, &Img);
  EXPECT_EQ(Func.CallInfos.size(), 1u);
  return {Func.CallInfos.empty() ? std::vector<MedVar>{}
                                 : Func.CallInfos[0].Args,
          !Func.Params.empty()};
}

TEST(I386CallContract, APICPLTEntryJumpsByTheGOTItsCallerHolds) {
  // A shared object's PLT entries, `jmp *name@GOT(%ebx)`, jump by the GOT
  // address the psABI has their caller hold in EBX, which the dynamic section
  // names (DT_PLTGOT).  Each passes its caller's argument on: islower's to
  // islower, and g's to the g the object defines, where the loader starts
  // g's slot.
  constexpr va_t G = 0x320, Plt = 0x3C0, GotPlt = 0x2000;
  constexpr va_t GSlot = GotPlt + 0xC, LowerSlot = GotPlt + 0x10;
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::ELF;
  Img.DynInfo.PltGotAddr = GotPlt;
  const auto AddCode = [&](const char *Name, va_t VA,
                           std::vector<uint8_t> Bytes) {
    Segment Code;
    Code.Name = Name;
    Code.VA = VA;
    Code.Data = std::move(Bytes);
    Code.Size = Code.FileSz = Code.Data.size();
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Segments.push_back(std::move(Code));
  };
  AddCode(".text", G, {0x8B, 0x44, 0x24, 0x04, 0xC3}); // mov eax, [esp+4]; ret
  std::vector<uint8_t> Entries = {0xFF, 0xA3, 0x0C, 0x00, 0x00, 0x00};
  Entries.resize(0x10, 0xCC);                            // jmp *0xc(%ebx)
  for (uint8_t B : {0xFF, 0xA3, 0x10, 0x00, 0x00, 0x00}) // jmp *0x10(%ebx)
    Entries.push_back(B);
  Entries.resize(0x20, 0xCC);
  AddCode(".plt", Plt, std::move(Entries));
  Segment Got;
  Got.Name = ".got.plt";
  Got.VA = GotPlt;
  Got.Size = Got.FileSz = 0x18;
  Got.Data.assign(Got.Size, 0);
  Got.Data[GSlot - GotPlt] = static_cast<uint8_t>(G);
  Got.Data[GSlot - GotPlt + 1] = static_cast<uint8_t>(G >> 8);
  Got.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Got));
  Img.Imports.push_back({"libc.so.6", "g", 0, GSlot});
  Img.Imports.push_back({"libc.so.6", "islower", 0, LowerSlot});
  Img.CodePtrRelocSlots.insert(GSlot);
  ASSERT_TRUE(Img.recordImportStorageSlot(LowerSlot, "islower", 0,
                                          ImportStorageEvidence::LoaderBind));
  Symbol Function = Symbol::makeFunc(G);
  Function.Name = "g";
  Img.Symbols.push_back(std::move(Function));
  scanImportThunks(Img);
  ASSERT_NE(Img.findImportStubAt(Plt), nullptr);
  ASSERT_NE(Img.findImportStubAt(Plt + 0x10), nullptr);

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {G, Plt, Plt + 0x10};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  for (const char *Call : {"sub_3C0(int32_t arg0) {\n    return g(arg0);",
                           "sub_3D0(int32_t arg0) {\n    return "
                           "(int32_t)islower(arg0);"})
    EXPECT_NE(Source.find(Call), std::string::npos) << Call << "\n" << Source;
  EXPECT_EQ(Source.find("unknown register"), std::string::npos) << Source;
}

TEST(I386CallContract, HeaderVariadicStackPrefixKeepsTheOpenTail) {
  windows_variadic_test::Header Debug;
  Debug.Slot = Iat;
  for (unsigned Tail : {0u, 1u, 3u}) {
    std::vector<uint8_t> Code;
    for (unsigned I = Tail; I > 0; --I)
      Code.insert(Code.end(), {0x6A, static_cast<uint8_t>(11 * I)});
    Code.insert(Code.end(), {0x6A, static_cast<uint8_t>(Tail)});
    Code.insert(Code.end(),
                {0xFF, 0x74, 0x24, static_cast<uint8_t>(4 * (Tail + 2))});
    const auto Call = callThroughSlot(Iat);
    Code.insert(Code.end(), Call.begin(), Call.end());
    Code.insert(Code.end(),
                {0x83, 0xC4, static_cast<uint8_t>(4 * (Tail + 2)), 0xC3});
    auto Image = makeImage(Code, {"fixture_variadic"});
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Tail);
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = LLVM;
      Options.SourceProjection = LLVM;
      Options.OnlyFunctionEntries = {Text};
      auto Result = Pipeline().run(Image, Context, Options, &Debug);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                        &Debug, &Image));
      } else {
        ASSERT_TRUE(
            HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
      }
      source_call_execution_test::compileAndRun(
          Source + windows_variadic_test::oracle(Tail, "wrap"));
    }
  }
}

TEST(I386CallContract, HeaderVariadicTailJumpRetainsItsFixedStackInputs) {
  windows_variadic_test::Header Debug;
  Debug.Slot = Iat;
  auto Code = callThroughSlot(Iat);
  Code[1] = 0x25; // jmp [Iat], forwarding the incoming fixed stack prefix
  auto Image = makeImage(Code, {"fixture_variadic"});
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = LLVM;
    Options.SourceProjection = LLVM;
    Options.OnlyFunctionEntries = {Text};
    auto Result = Pipeline().run(Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_EQ(Result.MedFuncs.front().Params.size(), 2u);
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions, &Debug,
                                      &Image));
    } else {
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
    }
    std::string Oracle = windows_variadic_test::oracle(0, "wrap");
    const auto Call = Oracle.find("wrap(-7)");
    ASSERT_NE(Call, std::string::npos);
    Oracle.replace(Call, std::string("wrap(-7)").size(), "wrap(-7, 0)");
    source_call_execution_test::compileAndRun(Source + Oracle);
  }
}

} // namespace

// An ELF import takes every argument on the stack, whether or not its
// signature is known: the ECX and EDX its caller left are no arguments, and
// a live-in ECX below them is no parameter the caller passes on.
TEST(I386CallContract, AnELFImportTakesNoRegisterArgument) {
  const auto [Arguments, GainedParameter] =
      importCallArguments(BinaryFormat::ELF, /*SetECX=*/true);
  EXPECT_TRUE(Arguments.empty());
  const auto [Forwarded, Promoted] =
      importCallArguments(BinaryFormat::ELF, /*SetECX=*/false);
  EXPECT_TRUE(Forwarded.empty());
  EXPECT_FALSE(Promoted);
}

// A Windows import of unknown signature may be __fastcall, so the ECX and
// EDX set before it remain its arguments.
TEST(I386CallContract, AWindowsImportMayTakeECXAndEDX) {
  const auto [Arguments, GainedParameter] =
      importCallArguments(BinaryFormat::COFF, /*SetECX=*/true);
  ASSERT_EQ(Arguments.size(), 2u);
  EXPECT_EQ(Arguments[0], temporary32(1));
  EXPECT_EQ(Arguments[1], temporary32(2));
}

// A register argument a block's entry PHI holds reaches a call only where no
// earlier call in the block changes the register.  Here EDX joins a stored
// callback's address with what an import call left, the join block calls the
// import again, and then a routine reading ECX and EDX: EDX holds that call's
// leftover there, not the join, whose code-address arm the LLVM emitter could
// not give one identity.
TEST(I386CallContract, ABlockPhiIsNoArgumentPastAnEarlierCallInItsBlock) {
  std::vector<uint8_t> Code = {
      0x55,                         // push ebp
      0x8B, 0xEC,                   // mov ebp, esp
      0x53,                         // push ebx
      0x8B, 0x5D, 0x08,             // mov ebx, [ebp+8]
      0x85, 0xDB,                   // test ebx, ebx
      0x74, 0x07,                   // je 0x401012
      0xBA, 0x60, 0x10, 0x40, 0x00, // mov edx, offset callback
      0xEB, 0x0B,                   // jmp 0x40101D
      0xBA, 0x07, 0x00, 0x00, 0x00, // 0x401012: mov edx, 7
  };
  for (uint8_t Byte : callThroughSlot(slot(0))) // call dword ptr [rand]
    Code.push_back(Byte);
  for (uint8_t Byte : callThroughSlot(slot(0))) // 0x40101D: the join
    Code.push_back(Byte);
  for (uint8_t Byte : {0x8B, 0xCB,                   // mov ecx, ebx
                       0xE8, 0x26, 0x00, 0x00, 0x00, // call reads_two
                       0x5B, 0x5D, 0xC3})            // pop; pop; ret
    Code.push_back(Byte);
  Code.resize(0x50, 0xCC);
  for (uint8_t Byte : {0x8D, 0x04, 0x11, 0xC3}) // 0x401050: reads_two
    Code.push_back(Byte);                       // lea eax, [ecx+edx]; ret
  Code.resize(0x60, 0xCC);
  for (uint8_t Byte : {0x33, 0xC0, 0xC3}) // 0x401060: callback
    Code.push_back(Byte);                 // xor eax, eax; ret
  BinaryImage Img = makeImage(Code, {"rand"});
  const std::pair<va_t, const char *> Routines[] = {{Text + 0x50, "_reads_two"},
                                                    {Text + 0x60, "_callback"}};
  for (const auto &[Address, Name] : Routines) {
    Symbol Routine = Symbol::makeFunc(Address);
    Routine.Name = Name;
    Img.Symbols.push_back(std::move(Routine));
  }
  // The base relocation of the mov's imm32 at 0x40100C.
  Img.CodeAddressRelocOperands.emplace(
      Text + 0x0C, RelocatedAddressField{.EncodedValue = Text + 0x60,
                                         .TargetVA = Text + 0x60,
                                         .Width = 4,
                                         .TargetOwnerVA = Text + 0x60});
  const std::set<va_t> Entries = {Text, Text + 0x50, Text + 0x60};

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = Entries;
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const auto Wrap =
      std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                   [](const MedFunc &F) { return F.Entry == Text; });
  ASSERT_NE(Wrap, Result.MedFuncs.end());
  const MedCallInfo *Call = nullptr;
  const MedBlock *CallBlock = nullptr;
  for (const MedBlock &Block : Wrap->Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Text + 0x50) {
        Call = Wrap->findCall(Block.Id, static_cast<int>(I));
        CallBlock = &Block;
      }
    }
  ASSERT_NE(Call, nullptr);
  ASSERT_GE(Call->Args.size(), 2u);
  for (const PhiNode &Phi : CallBlock->Phis)
    EXPECT_FALSE(Call->Args[1].Kind == Phi.Output.Kind &&
                 Call->Args[1].Id == Phi.Output.Id &&
                 Call->Args[1].SSAVer == Phi.Output.SSAVer)
        << "EDX holds what the call before it left, not the block's join";

  llvm::LLVMContext LiftCtx;
  PipelineOptions Lift;
  Lift.EmitDumpOutput = false;
  Lift.LiftMode = true;
  Lift.OnlyFunctionEntries = Entries;
  const PipelineResult Lifted = Pipeline().run(Img, LiftCtx, Lift);
  ASSERT_TRUE(Lifted.Success) << Lifted.Error;
  ASSERT_NE(Lifted.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Lifted.LlvmModule, &llvm::errs()));
}
