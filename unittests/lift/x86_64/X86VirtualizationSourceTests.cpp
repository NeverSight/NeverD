//===- X86VirtualizationSourceTests.cpp - VMX/SVM source tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the shared pipeline and both source routes across x86/x64 native
/// formats. Privileged success is not assumed; isolated system-memory probes
/// compare faults with the original bytes on the same host.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86AmxState.h"
#include "../../../lib/backend/c/render/X86/X86Sgx.h"
#include "../../../lib/backend/c/render/X86/X86SystemState.h"
#include "../../../lib/backend/c/render/X86/X86Virtualization.h"
#include "X86AMXFixtures.h"
#include "X86SHA512Reference.h"
#include "X86SystemStateFixtures.h"
#include "X86VirtualizationFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86AmxAsm.h"
#include "neverd/backend/llvm/X86/X86AmxNative.h"
#include "neverd/backend/llvm/X86/X86DescriptorStateNative.h"
#include "neverd/backend/llvm/X86/X86ScalarSystemNative.h"
#include "neverd/backend/llvm/X86/X86SgxAsm.h"
#include "neverd/backend/llvm/X86/X86SgxNative.h"
#include "neverd/backend/llvm/X86/X86SystemStateAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::test;

namespace {
constexpr va_t Entry = 0x1000;

BinaryImage image(Arch A, BinaryFormat Format, std::vector<uint8_t> Bytes) {
  BinaryImage I;
  I.Arch = A;
  I.Format = Format;
  I.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  I.Base = I.Entry = Entry;
  Segment Text;
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Text.Size = Text.FileSz = Text.Data.size();
  auto FunctionSymbol = Symbol::makeFunc(Entry, Text.Size);
  FunctionSymbol.Name = "virtualization_test";
  I.Symbols.push_back(std::move(FunctionSymbol));
  I.Segments.push_back(std::move(Text));
  return I;
}

bool anchored(const CSourceMap &Map, va_t Address, llvm::StringRef Source,
              llvm::StringRef Mnemonic) {
  return std::any_of(
      Map.Anchors.begin(), Map.Anchors.end(), [&](const auto &A) {
        return A.Function == Entry && A.Span.End <= Source.size() &&
               Source.slice(A.Span.Begin, A.Span.End).contains(Mnemonic) &&
               std::any_of(A.Occurrences.begin(), A.Occurrences.end(),
                           [&](const auto &O) { return O.Address == Address; });
      });
}

void sourceRoutes(BinaryImage &Image, bool NoOpt, va_t Address,
                  llvm::StringRef Mnemonic, std::string *HighOutput = nullptr,
                  std::string *LLVMOutput = nullptr,
                  llvm::StringRef HighMnemonic = {},
                  llvm::StringRef LLVMMnemonic = {},
                  std::string *NativeOutput = nullptr,
                  std::vector<uint8_t> *NativeObject = nullptr,
                  uint16_t RequiredReturnBytes = 0,
                  bool *UsesForkNative = nullptr) {
  if (HighMnemonic.empty())
    HighMnemonic = Mnemonic;
  if (LLVMMnemonic.empty())
    LLVMMnemonic = Mnemonic;
  PipelineOptions O;
  O.EmitDumpOutput = false;
  O.NoOpt = NoOpt;
  O.OnlyFunctionEntries = {Entry};
  llvm::LLVMContext Context;
  auto R = Pipeline().run(Image, Context, O);
  ASSERT_TRUE(R.Success) << R.Error;
  ASSERT_EQ(R.HighFuncs.size(), 1u);
  ASSERT_EQ(R.MedFuncs.size(), 1u);
  if (RequiredReturnBytes) {
    ASSERT_FALSE(R.MedFuncs.front().ReturnsNoValue);
    ASSERT_FALSE(R.HighFuncs.front().ReturnsNoValue);
    ASSERT_TRUE(R.HighFuncs.front().ReturnType);
    EXPECT_EQ(R.HighFuncs.front().ReturnType->Size, RequiredReturnBytes);
  }
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  Options.Image = &Image;
  CSourceMap HighMap;
  HighMap.HighSources = &R.HighSources;
  Options.SourceMap = &HighMap;
  std::string High;
  llvm::raw_string_ostream HighOS(High);
  ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, HighOS, Options));
  if (!Mnemonic.empty())
    EXPECT_TRUE(anchored(HighMap, Address, High, HighMnemonic)) << High;
  if (Image.Format == BinaryFormat::COFF && Mnemonic == "cpuid") {
    EXPECT_NE(High.find("__cpuidex("), std::string::npos) << High;
    EXPECT_NE(High.find("_ReadWriteBarrier();"), std::string::npos) << High;
  }

  MedLLVMEmitter Emitter;
  LLVMSourceMap Sources;
  Emitter.setSourceMap(&Sources);
  auto Module = Emitter.emit(R.MedFuncs, Context, "virtualization", Image.Arch,
                             {}, &Image, Image.Format);
  ASSERT_NE(Module, nullptr);
  if (RequiredReturnBytes) {
    const auto *Function = Module->getFunction("virtualization_test");
    ASSERT_NE(Function, nullptr);
    EXPECT_TRUE(
        Function->getReturnType()->isIntegerTy(8 * RequiredReturnBytes));
  }
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  if (NoOpt)
    Pipeline::promoteScaffoldingAllocas(*Module);
  else
    (void)Pipeline::optimizeOrPromoteModule(*Module, &Sources);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  CSourceMap LLVMMap;
  LLVMMap.LLVMSources = &Sources;
  Options.SourceMap = &LLVMMap;
  std::string LLVM;
  llvm::raw_string_ostream LLVMOS(LLVM);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, LLVMOS, Options));
  if (!Mnemonic.empty())
    EXPECT_TRUE(anchored(LLVMMap, Address, LLVM, LLVMMnemonic)) << LLVM;
  if (NativeOutput) {
    llvm::raw_string_ostream OS(*NativeOutput);
    Module->print(OS, nullptr);
  }
  if (UsesForkNative)
    for (const auto &F : *Module)
      *UsesForkNative |= isX86VirtualizationNativeName(F.getName()) ||
                         (isX86SystemStateNativeName(F.getName()) &&
                          F.getName() != "llvm.x86.xabort") ||
                         isX86ScalarSystemNativeName(F.getName()) ||
                         isX86DescriptorStateNativeName(F.getName()) ||
                         isX86SystemRegisterNativeName(F.getName()) ||
                         isX86SgxNativeName(F.getName().str()) ||
                         x86AmxNativeDescriptor(F.getName().str()).has_value();
  if (NativeObject) {
    auto Copy = llvm::CloneModule(*Module);
    auto Object = Codegen().compile(*Copy, Image.Arch, Image.Format);
    ASSERT_TRUE(Object.Success);
    ASSERT_FALSE(Object.ObjectData.empty());
    *NativeObject = std::move(Object.ObjectData);
  }
  if (HighOutput)
    *HighOutput = High;
  if (LLVMOutput)
    *LLVMOutput = LLVM;
}

std::vector<uint8_t> function(Arch A, const std::vector<uint8_t> &Instruction,
                              size_t &InstructionOffset) {
  // Own enough initialized frame storage even for the 16-byte invalidation
  // descriptors. Distinct constants keep field/value operand ordering visible.
  std::vector<uint8_t> Bytes;
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0x83, 0xec, 0x20});
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0xc7, 0x04, 0x24, 0x55, 0, 0, 0});
  Bytes.insert(Bytes.end(), {0xb8, 0x56, 0x34, 0, 0, 0xb9, 0x34, 0x12, 0, 0,
                             0xba, 0x98, 0x76, 0, 0});
  InstructionOffset = Bytes.size();
  Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
  // Consume both flags before any arithmetic changes them.
  Bytes.insert(Bytes.end(),
               {0x0f, 0x92, 0xc0, 0x0f, 0x94, 0xc1, 0x0f, 0xb6, 0xc0, 0x0f,
                0xb6, 0xc9, 0xc1, 0xe1, 0x08, 0x09, 0xc8});
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0x83, 0xc4, 0x20, 0xc3});
  return Bytes;
}

std::vector<Encoding> nativeVirtualizationEncodings(Arch A) {
  std::vector<Encoding> Result(std::begin(Encodings), std::end(Encodings));
  if (A == Arch::X64)
    for (const auto &E : Encodings)
      if (x86VirtualizationContract(E.Id).PhysicalAddress) {
        auto Narrow = E;
        Narrow.Bytes.insert(Narrow.Bytes.begin(), 0x67);
        Result.push_back(std::move(Narrow));
      }
  return Result;
}

uint16_t physicalAddressBytes(const Encoding &E, Arch A) {
  if (!x86VirtualizationContract(E.Id).PhysicalAddress)
    return 0;
  return A == Arch::X86 || E.Bytes.front() == 0x67 ? 4 : 8;
}
} // namespace

TEST(X86VirtualizationSource, EveryOwnedInstructionSurvivesBothSourceRoutes) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const auto &Encoding : Encodings) {
          const auto C = x86VirtualizationContract(Encoding.Id);
          SCOPED_TRACE(C.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Bytes = function(A, Encoding.Bytes, Offset);
          auto Image = image(A, Format, std::move(Bytes));
          sourceRoutes(Image, NoOpt, Entry + Offset, C.Mnemonic);
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86SystemRegisterSource, NativeMovesClobberUndefinedArithmeticFlags) {
  // CR/DR MOVs leave OF/SF/ZF/AF/PF/CF undefined. The compiler must not carry
  // its own condition-code values across the native instruction.
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const std::vector<uint8_t> &Instruction :
             {std::vector<uint8_t>{0x0f, 0x20, 0xd8},    // MOV RAX/EAX, CR3
              std::vector<uint8_t>{0x0f, 0x22, 0xd8},    // MOV CR3, RAX/EAX
              std::vector<uint8_t>{0x0f, 0x21, 0xf8},    // MOV RAX/EAX, DR7
              std::vector<uint8_t>{0x0f, 0x23, 0xf8}}) { // MOV DR7, RAX/EAX
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          std::vector<uint8_t> Bytes = Instruction;
          Bytes.push_back(0xc3);
          auto Image = image(A, Format, std::move(Bytes));
          std::string Native;
          std::vector<uint8_t> Object;
          sourceRoutes(Image, NoOpt, Entry, {}, nullptr, nullptr, {}, {},
                       &Native, &Object);
          ASSERT_FALSE(Object.empty());
          const std::string Name =
              "llvm.x86." + std::string(Instruction[1] & 2 ? "write" : "read") +
              (Instruction[1] & 1 ? "dr" : "cr") + (A == Arch::X86 ? "32" : "");
          if (llvm::Intrinsic::lookupIntrinsicID(Name)) {
            EXPECT_NE(Native.find("@" + Name + "(i32 " +
                                  (Instruction[1] & 1 ? "7" : "3")),
                      std::string::npos)
                << Native;
            EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
          } else
            EXPECT_NE(Native.find("~{flags}"), std::string::npos) << Native;
        }
}

TEST(X86UnsupportedSource, LegacyPlaceholdersCannotBecomeNoOps) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : {Intrinsic::BndMk, Intrinsic::BndMov, Intrinsic::BndCl,
                    Intrinsic::BndCu, Intrinsic::BndCn, Intrinsic::BndLdx,
                    Intrinsic::BndStx, Intrinsic::Xacquire, Intrinsic::Xrelease,
                    Intrinsic::ShaGeneric}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Id));
      MedFunc Function;
      Function.Entry = Entry;
      Function.Name = "unsupported_instruction";
      Function.CC =
          A == Arch::X64 ? CallingConv::SysV_AMD64 : CallingConv::CDECL;
      Function.ReturnType = NdType::makeVoid();
      Function.ReturnsNoValue = true;
      Function.Blocks.resize(1);
      auto &Block = Function.Blocks.front();
      Block.Id = 0;
      Block.StartAddr = Entry;
      Block.EndAddr = Entry + 2;
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Addr = Entry;
      Op.addInput(MedVar::makeConst(static_cast<unsigned>(Id), 2));
      Block.Ops.push_back(Op);
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = Entry + 1;
      Block.Ops.push_back(Return);
      EXPECT_DEATH(
          {
            llvm::LLVMContext Context;
            (void)MedLLVMEmitter().emit({Function}, Context, Function.Name, A);
          },
          "unimplemented x86 intrinsic requires an owned semantic contract");
      for (bool Gnu : {false, true})
        EXPECT_DEATH(
            {
              bool HasIntrinsics = false;
              (void)renderX86IntrinsicCall(Id, {}, HasIntrinsics, Gnu);
            },
            "unimplemented x86 intrinsic requires an owned semantic contract");
    }
}

TEST(X86SystemRegisterSource, MalformedCallsCannotBecomeIntrinsicWritesOfZero) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Write : {false, true})
        for (bool Debug : {false, true})
          for (unsigned Mutation = 0; Mutation < 23; ++Mutation) {
            SCOPED_TRACE(static_cast<unsigned>(A));
            SCOPED_TRACE(static_cast<unsigned>(Format));
            SCOPED_TRACE(Write);
            SCOPED_TRACE(Debug);
            SCOPED_TRACE(Mutation);
            llvm::LLVMContext Context;
            llvm::Module Module("sysreg-contract", Context);
            Module.setDataLayout(A == Arch::X64 ? "e-p:64:64" : "e-p:32:32");
            llvm::IRBuilder<> B(Context);
            auto *Native = B.getIntNTy(A == Arch::X64 ? 64 : 32);
            auto *Value = Mutation == 4 ? B.getInt16Ty() : Native;
            llvm::Type *Return = Write ? B.getVoidTy() : Value;
            const unsigned Arity = Mutation == 12 ? !Write : Write;
            std::vector<llvm::Type *> Types(Arity, Value);
            if (Mutation == 17 && Write)
              Types[0] = B.getPtrTy();
            auto *Signature = llvm::FunctionType::get(Return, Types, false);
            auto *Function = llvm::Function::Create(
                Signature, llvm::GlobalValue::ExternalLinkage, "effect",
                Module);
            B.SetInsertPoint(
                llvm::BasicBlock::Create(Context, "entry", Function));
            std::vector<llvm::Value *> Values;
            for (auto &Arg : Function->args())
              Values.push_back(&Arg);
            const std::string Reg = Debug ? "%dr7" : "%cr3";
            std::string AsmText =
                Write ? "mov $0, " + Reg : "mov " + Reg + ", $0";
            std::string Constraints = Write ? "" : "=r,";
            if (Arity)
              Constraints += "r,";
            if (Mutation != 2)
              Constraints += "~{flags},";
            Constraints += "~{memory}";
            if (Mutation == 1)
              Constraints.erase(Constraints.find(",~{memory}"));
            if (Mutation == 5)
              AsmText += "\n\tnop";
            if (Mutation == 6)
              AsmText.replace(AsmText.find(Reg), Reg.size(),
                              Debug ? "%dr8" : "%cr1");
            if (Mutation == 18)
              AsmText.replace(0, 3, "movq");
            auto *Asm = llvm::InlineAsm::get(
                Signature, AsmText, Constraints, Mutation != 3, Mutation == 13,
                Mutation == 7 ? llvm::InlineAsm::AD_Intel
                              : llvm::InlineAsm::AD_ATT,
                Mutation == 14);
            llvm::SmallVector<llvm::OperandBundleDef> Bundles;
            if (Mutation == 15)
              Bundles.emplace_back("deopt", std::vector<llvm::Value *>{});
            auto *Call = B.CreateCall(Signature, Asm, Values, Bundles);
            if (Mutation == 8)
              Call->setOnlyReadsMemory();
            if (Mutation == 9)
              Call->setOnlyWritesMemory();
            if (Mutation == 10)
              Call->setDoesNotReturn();
            if (Mutation == 19)
              Call->setMemoryEffects(llvm::MemoryEffects::argMemOnly());
            if (Mutation == 20)
              Call->setMemoryEffects(
                  llvm::MemoryEffects::inaccessibleMemOnly());
            if (Mutation == 21)
              Call->setMemoryEffects(
                  llvm::MemoryEffects::inaccessibleOrArgMemOnly());
            if (Mutation == 22)
              Call->setMemoryEffects(llvm::MemoryEffects::none());
            if (Write)
              B.CreateRetVoid();
            else if (Mutation == 16)
              B.CreateRet(llvm::ConstantInt::get(Return, 0));
            else
              B.CreateRet(Call);
            ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
            CEmitterOptions Options;
            Options.TheArch = Mutation == 11 ? Arch::AArch64 : A;
            Options.Format = Format;
            std::string Text;
            llvm::raw_string_ostream OS(Text);
            if (Mutation == 0 || Mutation == 16 || (Mutation == 17 && !Write)) {
              EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Text;
              if (Format == BinaryFormat::COFF) {
                EXPECT_NE(Text.find("#include <intrin.h>"), std::string::npos)
                    << Text;
                EXPECT_NE(Text.find(Write ? "__write" : "__read"),
                          std::string::npos)
                    << Text;
              } else {
                EXPECT_NE(Text.find("__asm__ volatile"), std::string::npos)
                    << Text;
                EXPECT_NE(Text.find("\"cc\", \"memory\""), std::string::npos)
                    << Text;
                EXPECT_EQ(Text.find("#include <intrin.h>"), std::string::npos)
                    << Text;
              }
            } else {
              EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                           std::invalid_argument);
            }
          }
}

TEST(X86SystemRegisterSource, TextAloneCannotAuthorizeASystemRegisterAccess) {
  for (const auto &Text : {"mov $0, %cr3", "mov %dr7, $0", "mov $0, %dr8"})
    EXPECT_THROW(renderInlineAsm(Arch::X64, Text, false, "result", true, {}),
                 std::invalid_argument);
}

TEST(X86VirtualizationSource,
     CpuidAndSiblingIntrinsicsRetainTheirEffectAnchors) {
  struct Encoding {
    std::vector<uint8_t> Bytes;
    const char *Mnemonic;
    const char *CName = nullptr;
  };
  for (const auto &E : std::vector<Encoding>{{{0x0f, 0xa2}, "cpuid"},
                                             {{0x0f, 0x01, 0xd0}, "xgetbv"},
                                             {{0x0f, 0x31}, "rdtsc"},
                                             {{0x0f, 0x01, 0xf9}, "rdtscp"},
                                             {{0x0f, 0xae, 0xe8}, "lfence"},
                                             {{0x0f, 0xae, 0xf0}, "mfence"},
                                             {{0x0f, 0xae, 0xf8}, "sfence"},
                                             {{0xfa}, "cli", "_disable"},
                                             {{0xfb}, "sti", "_enable"}}) {
    SCOPED_TRACE(E.Mnemonic);
    for (const auto A : {Arch::X86, Arch::X64})
      for (const auto Format :
           {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
        for (bool NoOpt : {false, true}) {
          // All query outputs are dead. The call/fence remains an ordered
          // effect.
          std::vector<uint8_t> Bytes = {0x53, 0xb8, 0x07, 0, 0, 0,
                                        0xb9, 0x01, 0,    0, 0};
          const auto Offset = Bytes.size();
          Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
          Bytes.insert(Bytes.end(), {0x31, 0xc0, 0x5b, 0xc3});
          auto Image = image(A, Format, std::move(Bytes));
          sourceRoutes(
              Image, NoOpt, Entry + Offset, E.Mnemonic, nullptr, nullptr,
              E.CName && Format == BinaryFormat::COFF ? E.CName : E.Mnemonic,
              E.CName ? E.CName : E.Mnemonic);
          if (::testing::Test::HasFailure())
            return;
        }
  }
}

TEST(X86VirtualizationSource, VmreadMemoryInvalidatesThePriorStackValue) {
  for (bool NoOpt : {false, true}) {
    auto Image =
        image(Arch::X64, BinaryFormat::ELF,
              {0x48, 0x83, 0xec, 0x08, 0x48, 0xc7, 0x04, 0x24, 0x55, 0,
               0,    0,    0xb9, 0x34, 0x12, 0,    0,    0x0f, 0x78, 0x0c,
               0x24, 0x48, 0x8b, 0x04, 0x24, 0x48, 0x83, 0xc4, 0x08, 0xc3});
    PipelineOptions O;
    O.NoOpt = NoOpt;
    O.EmitDumpOutput = false;
    O.OnlyFunctionEntries = {Entry};
    llvm::LLVMContext Context;
    auto R = Pipeline().run(Image, Context, O);
    ASSERT_TRUE(R.Success) << R.Error;
    MedLLVMEmitter Emitter;
    auto Module = Emitter.emit(R.MedFuncs, Context, "vmread_memory", Image.Arch,
                               {}, &Image, Image.Format);
    ASSERT_NE(Module, nullptr);
    if (NoOpt)
      Pipeline::promoteScaffoldingAllocas(*Module);
    else
      (void)Pipeline::optimizeOrPromoteModule(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    bool LoadedAfterRead = false;
    for (const auto &F : *Module)
      for (const auto &B : F) {
        bool Read = false;
        for (const auto &I : B) {
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (const auto *Callee = Call->getCalledFunction())
              Read |= Callee->getName() == "llvm.x86.vmread";
            if (const auto *Asm =
                    llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand()))
              Read |=
                  llvm::StringRef(Asm->getAsmString()).starts_with("vmread");
          }
          LoadedAfterRead |= Read && llvm::isa<llvm::LoadInst>(I) &&
                             I.getType()->isIntegerTy(64);
        }
      }
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    Module->print(OS, nullptr);
    EXPECT_TRUE(LoadedAfterRead) << IR;
  }
}

TEST(X86VirtualizationSource, PhysicalRegistersKeepTheirArchitecture) {
  MedOp Op;
  Op.NumInputs = 1;
  Op.Inputs[0].Kind = MedVar::Const;
  Op.Inputs[0].ConstVal = static_cast<uint16_t>(Intrinsic::VmxOff);
  Op.Inputs[0].Size = 2;
  Op.Output.Kind = MedVar::Reg;
  Op.Output.Id = 0;
  Op.Output.Size = 2;
  Op.Output.TheArch = Arch::X64;
  EXPECT_FALSE(x86VirtualizationShapeIsValid(
      Intrinsic::VmxOff, x86VirtualizationMedShape(Op, Arch::X86)));
  Op.Output.TheArch = Arch::X86;
  EXPECT_TRUE(x86VirtualizationShapeIsValid(
      Intrinsic::VmxOff, x86VirtualizationMedShape(Op, Arch::X86)));
  // A temporary's default TheArch does not establish physical ownership.
  Op.Output.Kind = MedVar::Temp;
  Op.Output.TheArch = Arch::X64;
  EXPECT_TRUE(x86VirtualizationShapeIsValid(
      Intrinsic::VmxOff, x86VirtualizationMedShape(Op, Arch::X86)));
}

TEST(X86VirtualizationSource, CpuidCExecutesOnceWithSubleafAndUnsignedOutputs) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-cpuid-source", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  for (const auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true}) {
      // Store every architectural result into a 64-bit cell. High-bit words
      // expose accidental sign extension of MSVC's signed int output array.
      std::vector<uint8_t> Bytes = {
          0x53, 0x49,
          0x89, Format == BinaryFormat::COFF ? uint8_t{0xc9} : uint8_t{0xf9},
          0xb8, 7,
          0,    0,
          0,    0xb9,
          1,    0,
          0,    0,
          0x0f, 0xa2,
          0x49, 0x89,
          0x01, 0x49,
          0x89, 0x59,
          0x08, 0x49,
          0x89, 0x49,
          0x10, 0x49,
          0x89, 0x51,
          0x18, 0x31,
          0xc0, 0x5b,
          0xc3};
      auto Image = image(Arch::X64, Format, std::move(Bytes));
      std::string High;
      sourceRoutes(Image, NoOpt, Entry + 14, "cpuid", &High);
      if (::testing::Test::HasFailure())
        return;
      std::string Program = R"C(
#include <stdint.h>
static unsigned calls, leaf_seen, subleaf_seen;
static const uint32_t words[4] = {0x80000000u, 0xfedcba98u, 0x87654321u, 0xffffffffu};
static void query(unsigned leaf, unsigned subleaf, unsigned *a, unsigned *b,
                  unsigned *c, unsigned *d) {
  ++calls; leaf_seen = leaf; subleaf_seen = subleaf;
  *a = words[0]; *b = words[1]; *c = words[2]; *d = words[3];
}
#define __cpuid(l,a,b,c,d) query(l,0,&(a),&(b),&(c),&(d))
#define __cpuid_count(l,s,a,b,c,d) query(l,s,&(a),&(b),&(c),&(d))
#define __cpuidex(o,l,s) query(l,s,(unsigned *)&o[0],(unsigned *)&o[1],(unsigned *)&o[2],(unsigned *)&o[3])
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
)C";
      // Only the intrinsic/header environment is replaced. Keep the emitted
      // declaration and body unchanged, including all four output transports.
      llvm::SmallVector<llvm::StringRef> Lines;
      llvm::StringRef(High).split(Lines, '\n');
      for (const auto Line : Lines)
        if (!Line.starts_with("#include"))
          Program += Line.str() + '\n';
      Program += R"C(
int main(void) {
  uint64_t out[4] = {0, 0, 0, 0};
  (void)virtualization_test(out);
  if (calls != 1 || leaf_seen != 7 || subleaf_seen != 1) return 1;
  for (unsigned i = 0; i < 4; ++i) if (out[i] != words[i]) return 2 + i;
  return 0;
}
)C";
      llvm::SmallString<128> Input(Directory), Executable(Directory),
          ErrorFile(Directory);
      llvm::sys::path::append(Input, "cpuid.c");
      llvm::sys::path::append(Executable, "cpuid.exe");
      llvm::sys::path::append(ErrorFile, "clang.err");
      std::error_code Error;
      llvm::raw_fd_ostream OS(Input, Error);
      ASSERT_FALSE(Error);
      OS << Program;
      OS.close();
      ASSERT_FALSE(OS.has_error());
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Optimization);
        const std::vector<llvm::StringRef> Arguments = {
            Compiler, "-std=gnu11", "-fno-strict-aliasing", Optimization, Input,
            "-o",     Executable};
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, std::nullopt, ErrorFile.str()};
        const int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                     std::nullopt, Redirects);
        const auto Diagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
        ASSERT_EQ(Status, 0)
            << (Diagnostics ? (*Diagnostics)->getBuffer().str() : "")
            << Program;
        EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0)
            << High;
      }
    }
}

namespace {
std::string freestandingC(std::string Source) {
  // Cross-target checks use the real compiler intrinsic headers without
  // requiring a target CRT installation. Only the standard memory prototypes
  // are supplied; generated declarations, bodies and assembly stay intact.
  const std::string Include = "#include <string.h>";
  if (const auto At = Source.find(Include); At != std::string::npos)
    Source.replace(At, Include.size(),
                   "extern void *memcpy(void *, const void *, __SIZE_TYPE__);\n"
                   "extern void *memset(void *, int, __SIZE_TYPE__);");
  return Source;
}

std::string faultDriver(const BinaryImage &Image) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  OS << R"C(
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern void virtualization_test(void);
extern void neverd_original(void);
static int result_fd;
static void fault(int signal, siginfo_t *info, void *context) {
  int result[2] = {signal, info->si_code};
  (void)context;
  _exit(write(result_fd, result, sizeof(result)) != sizeof(result));
}
static int observe(void (*fn)(void), int result[2]) {
  int descriptors[2];
  if (pipe(descriptors)) return 1;
  pid_t pid = fork();
  if (pid < 0) return 2;
  if (!pid) {
    close(descriptors[0]);
    result_fd = descriptors[1];
    struct sigaction action = {0};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, 0) || sigaction(SIGSEGV, &action, 0) ||
        sigaction(SIGBUS, &action, 0)) _exit(3);
    fn();
    _exit(4); // Fixtures deliberately require an architectural fault.
  }
  close(descriptors[1]);
  ssize_t bytes = read(descriptors[0], result, 2 * sizeof(int));
  close(descriptors[0]);
  int status;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) || bytes != 2 * sizeof(int)) return 5;
  return 0;
}
int main(void) {
  int expected[2], actual[2];
  if (observe(neverd_original, expected) ||
      observe(virtualization_test, actual)) return 1;
  return expected[0] != actual[0] || expected[1] != actual[1];
}
__asm__(".text\n.globl neverd_original\nneverd_original:\n"
)C";
  for (const auto Byte : Image.Segments.front().Data)
    OS << "\".byte " << unsigned(Byte) << "\\n\"\n";
  OS << ");\n";
  return Source;
}

// Compile the actual LLVM assembly and both emitted C routes, including all
// output constraints. Success fixtures execute against an independent expected
// value; selected system-memory fixtures instead compare faults with raw bytes.
void compileRoutes(BinaryImage &Image, bool NoOpt, va_t Address,
                   llvm::StringRef Mnemonic,
                   std::optional<uint64_t> Expected = std::nullopt,
                   bool OriginalBytes = false, bool CompareFaults = false,
                   llvm::StringRef CustomDriver = {},
                   llvm::ArrayRef<uint8_t> ExpectedInstruction = {},
                   uint16_t RequiredReturnBytes = 0,
                   bool CompileForImageFormat = false,
                   llvm::StringRef NativeCCompiler = {}) {
  const bool Execute = Expected || CompareFaults || !CustomDriver.empty();
#if !defined(__linux__) || !defined(__x86_64__)
  if (Execute)
    GTEST_SKIP() << "Native execution fixture requires Linux x86-64";
#endif
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  auto LinkArguments = [&](llvm::StringRef Driver, llvm::StringRef Object,
                           llvm::StringRef Executable) {
    std::vector<llvm::StringRef> Args = {Compiler, Driver, Object, "-o",
                                         Executable};
    if (Image.Arch == Arch::X86)
      Args.insert(Args.end(), {"--target=i386-unknown-linux-gnu", "-nostdlib",
                               "-no-pie", "-fno-builtin", "-Wl,-e,_start"});
    return Args;
  };
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-x86-source", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  std::string High, LLVM, Native;
  std::vector<uint8_t> NativeObject;
  bool UsesForkNative = false;
  sourceRoutes(Image, NoOpt, Address, Mnemonic, &High, &LLVM, {}, {}, &Native,
               &NativeObject, RequiredReturnBytes, &UsesForkNative);
  if (::testing::Test::HasFailure())
    return;
  if (!ExpectedInstruction.empty())
    EXPECT_NE(std::search(NativeObject.begin(), NativeObject.end(),
                          ExpectedInstruction.begin(),
                          ExpectedInstruction.end()),
              NativeObject.end());
  unsigned Route = 0;
  for (auto Source : {High, LLVM, Native}) {
    SCOPED_TRACE(Route);
    const bool IR = Route++ == 2;
    // A stock Clang cannot consume the fork's new native intrinsic IDs.
    // sourceRoutes already compiles these with the linked SDK, and the native
    // object execution below uses that result. Both C routes still compile at
    // O0/O2 with stock Clang to check their assembly fallback.
    if (IR && UsesForkNative)
      continue;
    if (!IR)
      Source = freestandingC(std::move(Source));
    llvm::SmallString<128> Input(Directory), Output(Directory),
        ErrorFile(Directory), Driver(Directory), Executable(Directory);
    llvm::sys::path::append(Input, IR ? "source.ll" : "source.c");
    llvm::sys::path::append(Output, "source.o");
    llvm::sys::path::append(ErrorFile, "clang.err");
    llvm::sys::path::append(Driver, "driver.c");
    llvm::sys::path::append(Executable, "check");
    std::error_code Error;
    llvm::raw_fd_ostream OS(Input, Error);
    ASSERT_FALSE(Error);
    OS << Source;
    OS.close();
    ASSERT_FALSE(OS.has_error());
    if (Execute) {
      llvm::raw_fd_ostream DriverOS(Driver, Error);
      ASSERT_FALSE(Error);
      if (!CustomDriver.empty())
        DriverOS << CustomDriver;
      else if (CompareFaults) {
        ASSERT_EQ(Image.Arch, Arch::X64);
        DriverOS << faultDriver(Image);
      } else if (Image.Arch == Arch::X86) {
        DriverOS << R"C(
void *memcpy(void *d, const void *s, unsigned n) {
  for (unsigned i = 0; i < n; ++i) ((unsigned char *)d)[i] = ((const unsigned char *)s)[i];
  return d;
}
void *memset(void *d, int c, unsigned n) {
  for (unsigned i = 0; i < n; ++i) ((unsigned char *)d)[i] = c;
  return d;
}
extern unsigned virtualization_test(void);
void _start(void) {
  unsigned status = virtualization_test() != )C"
                 << *Expected << R"C(U;
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
)C";
      } else
        DriverOS << "extern unsigned long long virtualization_test(void);\n"
                    "int main(void) { return virtualization_test() != "
                 << *Expected << "ULL; }\n";
      DriverOS.close();
      ASSERT_FALSE(DriverOS.has_error());
    }
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const char *Target = Image.Arch == Arch::X86
                               ? "--target=i386-unknown-linux-gnu"
                               : "--target=x86_64-unknown-linux-gnu";
      if (CompileForImageFormat && !Execute) {
        if (Image.Format == BinaryFormat::COFF)
          Target = Image.Arch == Arch::X86 ? "--target=i686-pc-windows-msvc"
                                           : "--target=x86_64-pc-windows-msvc";
        else if (Image.Format == BinaryFormat::MachO)
          Target = Image.Arch == Arch::X86 ? "--target=i386-apple-darwin"
                                           : "--target=x86_64-apple-darwin";
      }
      const llvm::StringRef SourceCompiler =
          !IR && !NativeCCompiler.empty() ? NativeCCompiler : Compiler;
      SCOPED_TRACE(SourceCompiler.str());
      std::vector<llvm::StringRef> Arguments = {
          SourceCompiler, Target, Optimization, "-c", Input, "-o", Output};
      if (!IR && !NativeCCompiler.empty())
        Arguments.insert(Arguments.end(),
                         {"-fno-lto", "-fno-builtin-mimalloc"});
      // Match Codegen's x86 SSE2 baseline. Clang's plain i386 target has no
      // XMM register class for the emitted floating-point assembly operands.
      if (Image.Arch == Arch::X86)
        Arguments.push_back("-msse2");
      if (!IR)
        Arguments.insert(Arguments.end(),
                         {"-std=gnu11", "-ffreestanding",
                          "-fno-strict-aliasing",
                          "-Werror=implicit-function-declaration"});
      if (!IR && Expected)
        Arguments.push_back("-Werror=return-type");
      const std::optional<llvm::StringRef> Redirects[] = {
          std::nullopt, std::nullopt, ErrorFile.str()};
      const int Status = llvm::sys::ExecuteAndWait(SourceCompiler, Arguments,
                                                   std::nullopt, Redirects);
      const auto Diagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
      ASSERT_EQ(Status, 0) << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                           : "")
                           << Source;
      if (!ExpectedInstruction.empty()) {
        const auto Object = llvm::MemoryBuffer::getFile(Output);
        ASSERT_TRUE(Object);
        const auto Bytes = (*Object)->getBuffer();
        const llvm::StringRef Instruction(
            reinterpret_cast<const char *>(ExpectedInstruction.data()),
            ExpectedInstruction.size());
        EXPECT_TRUE(Bytes.contains(Instruction)) << Source;
      }
      if (Execute) {
        const int Linked = llvm::sys::ExecuteAndWait(
            Compiler, LinkArguments(Driver, Output, Executable), std::nullopt,
            Redirects);
        const auto LinkDiagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
        ASSERT_EQ(Linked, 0)
            << (LinkDiagnostics ? (*LinkDiagnostics)->getBuffer().str() : "");
        EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0)
            << Source;
      }
    }
  }
  if (Execute) {
    // The repository's LLVM fork is the native oracle. The system Clang above
    // independently checks emitted IR compatibility and compiles generated C.
    llvm::SmallString<128> Object(Directory), Driver(Directory),
        Executable(Directory);
    llvm::sys::path::append(Object, "neverd.o");
    llvm::sys::path::append(Driver, "driver.c");
    llvm::sys::path::append(Executable, "neverd-check");
    std::error_code Error;
    llvm::raw_fd_ostream OS(Object, Error);
    ASSERT_FALSE(Error);
    OS.write(reinterpret_cast<const char *>(NativeObject.data()),
             NativeObject.size());
    OS.close();
    ASSERT_FALSE(OS.has_error());
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  Compiler, LinkArguments(Driver, Object, Executable)),
              0);
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0);
    if (OriginalBytes) {
      llvm::SmallString<128> Assembly(Directory);
      llvm::sys::path::append(Assembly, "original.s");
      llvm::raw_fd_ostream Original(Assembly, Error);
      ASSERT_FALSE(Error);
      Original << ".text\n.globl virtualization_test\nvirtualization_test:\n";
      for (const auto Byte : Image.Segments.front().Data)
        Original << ".byte " << unsigned(Byte) << '\n';
      Original << ".section .note.GNU-stack,\"\",@progbits\n";
      Original.close();
      ASSERT_FALSE(Original.has_error());
      ASSERT_EQ(llvm::sys::ExecuteAndWait(
                    Compiler, LinkArguments(Driver, Assembly, Executable)),
                0);
      EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0);
    }
  }
}

std::vector<uint8_t> fpEncodingFunction(Arch A) {
  if (A == Arch::X86)
    return {0xb8, 3, 0, 0, 0, 0xf2, 0x0f, 0x2a, 0xc8, 0xb8, 7, 0, 0, 0, 0xf2,
            0x0f, 0x2a, 0xd0, 0xc4, 0xc1, 0x33, 0x58, 0xc2, // VADDSD;
                                                            // ignored
                                                            // B/vvvv[3].
            0xc4, 0xc1, 0x7b, 0x2c, 0xc0, // VCVTTSD2SI; ignored B.
            0xc3};
  return {0x48, 0x83, 0xec, 0x28, // SUB rsp,40.
          0x48, 0xb8, 0,    0,    0,    0,    0,    0,
          0xf0, 0x3f, 0x48, 0x89, 0x04, 0x24, // [rsp] = 1.0.
          0x48, 0xb8, 0,    0,    0,    0,    0,    0,
          0,    0x40, 0x48, 0x89, 0x44, 0x24, 0x08, // [rsp+8] = 2.0.
          0x66, 0x0f, 0x10, 0x04, 0x24,             // MOVUPD xmm0,[rsp].
          0xd5, 0x48, 0x8d, 0x04, 0x24,             // LEA r16,[rsp].
          0x66, 0xd5, 0xd0, 0x58, 0x00,             // ADDPD xmm0,[r16].
          0xf2, 0xd5, 0xc8, 0x2c, 0xc8,             // CVTTSD2SI r17,xmm0.
          0xd5, 0x48, 0x89, 0xc8,                   // MOV rax,r17.
          0x48, 0x83, 0xc4, 0x28, 0xc3};
}
} // namespace

namespace {
struct SystemRegisterEncoding {
  bool Debug;
  unsigned Index;
  bool Write;
  bool Dead;
  size_t Offset;
  std::vector<uint8_t> Bytes;

  std::string nativeName(Arch A) const {
    return "llvm.x86." + std::string(Write ? "write" : "read") +
           (Debug ? "dr" : "cr") + (A == Arch::X86 ? "32" : "");
  }
};

std::vector<SystemRegisterEncoding> systemRegisterEncodings(Arch A) {
  std::vector<SystemRegisterEncoding> Result;
  for (bool Debug : {false, true})
    for (unsigned Index = 0; Index <= 8; ++Index) {
      if (Debug ? Index == 8
                : (Index != 0 && Index != 2 && Index != 3 && Index != 4 &&
                   (Index != 8 || A != Arch::X64)))
        continue;
      for (bool Write : {false, true})
        for (bool Dead : {false, true}) {
          if (Dead && Write)
            continue;
          std::vector<uint8_t> Bytes;
          if (Write)
            Bytes = A == Arch::X64
                        ? std::vector<uint8_t>{0x48, 0xb8, 0x55, 0xaa, 0x23,
                                               0x81, 0x67, 0x45, 0xfe, 0xab}
                        : std::vector<uint8_t>{0xb8, 0x55, 0xaa, 0x23, 0x81};
          const auto Offset = Bytes.size();
          if (Index == 8)
            Bytes.push_back(0x44);
          Bytes.insert(Bytes.end(), {0x0f, uint8_t(0x20 + Debug + 2 * Write),
                                     uint8_t(0xc0 + 8 * (Index & 7))});
          if (Dead)
            Bytes.insert(Bytes.end(), {0x31, 0xc0});
          Bytes.push_back(0xc3);
          Result.push_back(
              {Debug, Index, Write, Dead, Offset, std::move(Bytes)});
        }
    }
  return Result;
}

struct SystemRegisterTarget {
  Arch TheArch;
  BinaryFormat Format;
  const char *Name;
};

void PrintTo(const SystemRegisterTarget &Target, std::ostream *OS) {
  *OS << Target.Name;
}

class X86SystemRegisterAccess
    : public testing::TestWithParam<SystemRegisterTarget> {};
} // namespace

TEST_P(X86SystemRegisterAccess,
       EveryLegalAccessCompilesAndRetainsNativeFaults) {
  const auto A = GetParam().TheArch;
  const auto Format = GetParam().Format;
  unsigned Fixtures = 0;
  for (const auto &E : systemRegisterEncodings(A)) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    SCOPED_TRACE(static_cast<unsigned>(Format));
    SCOPED_TRACE(E.Debug);
    SCOPED_TRACE(E.Index);
    SCOPED_TRACE(E.Write);
    SCOPED_TRACE(E.Dead);
    auto Image = image(A, Format, E.Bytes);
    // Allocation may change the GPR in ModRM. The opcode still
    // distinguishes CR/DR reads/writes, including dead reads.
    const std::vector<uint8_t> Opcode = {0x0f,
                                         uint8_t(0x20 + E.Debug + 2 * E.Write)};
    // Every CPL3 access must match the original instruction's
    // fault, including DR4/5. Other targets are compiled only.
    compileRoutes(Image, false, Entry + E.Offset, {}, std::nullopt, false,
                  A == Arch::X64 && Format == BinaryFormat::ELF, {}, Opcode, 0,
                  true);
    if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped())
      return;
    ++Fixtures;
  }
  EXPECT_EQ(Fixtures, A == Arch::X64 ? 39U : 36U);
  RecordProperty("source_fixtures", Fixtures);
}

// Each architecture/format keeps every CR/DR input and source route within its
// own ordinary CTest deadline. The six cells retain all 225 native inputs.
INSTANTIATE_TEST_SUITE_P(
    NativeFormats, X86SystemRegisterAccess,
    testing::Values(
        SystemRegisterTarget{Arch::X86, BinaryFormat::ELF, "X86ELF"},
        SystemRegisterTarget{Arch::X86, BinaryFormat::COFF, "X86COFF"},
        SystemRegisterTarget{Arch::X86, BinaryFormat::MachO, "X86MachO"},
        SystemRegisterTarget{Arch::X64, BinaryFormat::ELF, "X64ELF"},
        SystemRegisterTarget{Arch::X64, BinaryFormat::COFF, "X64COFF"},
        SystemRegisterTarget{Arch::X64, BinaryFormat::MachO, "X64MachO"}),
    [](const testing::TestParamInfo<SystemRegisterTarget> &Info) {
      return Info.param.Name;
    });

TEST(X86FPEncodingSource, BothModesSurviveEveryFormatAndSourceRoute) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        auto Image = image(A, Format, fpEncodingFunction(A));
        ASSERT_NO_FATAL_FAILURE(sourceRoutes(Image, NoOpt, Entry, {}));
      }
}

TEST(X86FPEncodingSource, Rex2ExecutesThroughEveryRouteWithoutAPXHardware) {
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Image =
        image(Arch::X64, BinaryFormat::ELF, fpEncodingFunction(Arch::X64));
    ASSERT_NO_FATAL_FAILURE(compileRoutes(Image, NoOpt, Entry, {}, 2));
  }
}

TEST(X86FPEncodingSource, Mode32IgnoredFieldsAgreeWithOriginalNativeBytes) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "Original VEX instruction oracle requires AVX";
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Image =
        image(Arch::X86, BinaryFormat::ELF, fpEncodingFunction(Arch::X86));
    ASSERT_NO_FATAL_FAILURE(compileRoutes(Image, NoOpt, Entry, {}, 10, true));
  }
}

TEST(X86VectorEncodingSource, Mode32IgnoredExtensionsExecuteThroughEveryRoute) {
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Image =
        image(Arch::X86, BinaryFormat::ELF,
              {0xb8, 7,    0,    0,    0,    0x66, 0x0f, 0x6e, 0xc8, 0xb8,
               11,   0,    0,    0,    0x66, 0x0f, 0x6e, 0xd0, 0x62, 0xc1,
               0x35, 0x08, 0xfe, 0xc2, 0x66, 0x0f, 0x7e, 0xc0, 0xc3});
    // vpaddd xmm0, xmm1, xmm2: R', B and vvvv[3] are ignored in this mode.
    // No host AVX-512 feature is needed for the lifted integer arithmetic.
    compileRoutes(Image, NoOpt, Entry + 18, {}, 18);
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(X86VirtualizationSource,
     GeneratedAssemblyCompilesForBothWordSizesAtO0AndO2) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : Encodings) {
      const auto C = x86VirtualizationContract(E.Id);
      SCOPED_TRACE(C.Mnemonic);
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
      compileRoutes(Image, false, Entry + Offset, C.Mnemonic);
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86VirtualizationSource, QualifiedSdkUsesNativeVirtualizationCalls) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.vmread.address"))
    GTEST_SKIP() << "requires the qualified native virtualization SDK";
  unsigned Cases = 0;
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const auto &E : nativeVirtualizationEncodings(A)) {
          const auto C = x86VirtualizationContract(E.Id);
          for (const auto Space :
               {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
                NdMemoryAddressSpace::X86GS}) {
            const auto Plan = x86VirtualizationNative(
                E.Id, A, Space, physicalAddressBytes(E, A));
            if (!Plan)
              continue;
            SCOPED_TRACE(Plan->LLVMName);
            SCOPED_TRACE(static_cast<unsigned>(Format));
            SCOPED_TRACE(NoOpt);
            auto Bytes = E.Bytes;
            if (Space != NdMemoryAddressSpace::Default)
              Bytes.insert(Bytes.begin(), Plan->SegmentPrefix);
            size_t Offset;
            auto Image = image(A, Format, function(A, Bytes, Offset));
            std::string IR;
            std::vector<uint8_t> Object;
            sourceRoutes(Image, NoOpt, Entry + Offset, C.Mnemonic, nullptr,
                         nullptr, {}, {}, &IR, &Object);
            if (::testing::Test::HasFailure())
              return;
            EXPECT_NE(IR.find("@" + Plan->LLVMName + "("), std::string::npos)
                << IR;
            EXPECT_EQ(IR.find("asm sideeffect \"" + std::string(C.Mnemonic)),
                      std::string::npos)
                << IR;
            ++Cases;
          }
        }
  RecordProperty("native_modules", Cases);
}

namespace {
void compileNativeC(const std::string &Compiler, llvm::StringRef Directory,
                    BinaryFormat Format, const std::string &Source,
                    const std::string &LLVMName, unsigned &Cases,
                    std::optional<uint16_t> RequiredPort = std::nullopt,
                    bool CompileObject = false, bool NeverC = true) {
  llvm::SmallString<128> Input(Directory), Output(Directory), Errors(Directory);
  llvm::sys::path::append(Input, "source.c");
  llvm::sys::path::append(Output, "source.ll");
  llvm::sys::path::append(Errors, "compiler.err");
  std::error_code Error;
  llvm::raw_fd_ostream OS(Input, Error);
  ASSERT_FALSE(Error);
  OS << freestandingC(Source);
  OS.close();
  for (const char *Opt : {"-O0", "-O2"}) {
    SCOPED_TRACE(Opt);
    const char *Target =
        Format == BinaryFormat::COFF    ? "--target=x86_64-pc-windows-msvc"
        : Format == BinaryFormat::MachO ? "--target=x86_64-apple-darwin"
                                        : "--target=x86_64-linux-gnu";
    std::vector<llvm::StringRef> Arguments = {
        Compiler,     Target,
        "-std=gnu23", "-ffreestanding",
        "-fno-lto",   "-Werror=implicit-function-declaration",
        "-S",         "-emit-llvm",
        Opt,          Input,
        "-o",         Output};
    if (NeverC)
      Arguments.push_back("-fno-builtin-mimalloc");
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    const int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                 std::nullopt, Redirects, 30);
    const auto Diagnostics = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Status, 0) << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                         : "")
                         << Source;
    const auto IR = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(IR);
    EXPECT_TRUE((*IR)->getBuffer().contains("@" + LLVMName + "("))
        << (*IR)->getBuffer().str() << '\n'
        << Source;
    EXPECT_FALSE((*IR)->getBuffer().contains(" asm "))
        << (*IR)->getBuffer().str();
    if (RequiredPort && llvm::StringRef(Opt) == "-O2")
      EXPECT_TRUE((*IR)->getBuffer().contains("@" + LLVMName + "(i16 " +
                                              std::to_string(*RequiredPort)))
          << (*IR)->getBuffer().str();
    if (CompileObject) {
      auto ObjectArguments = Arguments;
      ObjectArguments.erase(std::find(ObjectArguments.begin(),
                                      ObjectArguments.end(), "-emit-llvm"));
      *std::find(ObjectArguments.begin(), ObjectArguments.end(), "-S") = "-c";
      ObjectArguments.emplace_back("-mllvm");
      ObjectArguments.emplace_back("-verify-machineinstrs");
      const int ObjectStatus = llvm::sys::ExecuteAndWait(
          Compiler, ObjectArguments, std::nullopt, Redirects, 30);
      const auto ObjectDiagnostics = llvm::MemoryBuffer::getFile(Errors);
      ASSERT_EQ(ObjectStatus, 0)
          << (ObjectDiagnostics ? (*ObjectDiagnostics)->getBuffer().str() : "")
          << Source;
      const auto Object = llvm::MemoryBuffer::getFile(Output);
      ASSERT_TRUE(Object);
      EXPECT_FALSE((*Object)->getBuffer().empty());
    }
    ++Cases;
  }
}
} // namespace

TEST_P(X86SystemRegisterAccess, NeverCCompilesBothRoutesToNativeIntrinsics) {
  if (GetParam().TheArch != Arch::X64)
    GTEST_SKIP() << "NeverC targets x64";
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-native-sysreg-c",
                                                    Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (bool NoOpt : {false, true})
    for (const auto &E : systemRegisterEncodings(Arch::X64)) {
      SCOPED_TRACE(E.nativeName(Arch::X64));
      SCOPED_TRACE(E.Index);
      SCOPED_TRACE(E.Dead);
      SCOPED_TRACE(NoOpt);
      auto Image = image(Arch::X64, GetParam().Format, E.Bytes);
      std::string High, LLVM, Native;
      sourceRoutes(Image, NoOpt, Entry + E.Offset, {}, &High, &LLVM, {}, {},
                   &Native);
      if (::testing::Test::HasFailure())
        return;
      const auto Name = E.nativeName(Arch::X64);
      if (llvm::Intrinsic::lookupIntrinsicID(Name)) {
        EXPECT_NE(Native.find("@" + Name + "(i32 " + std::to_string(E.Index)),
                  std::string::npos)
            << Native;
        EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
      }
      for (const auto &Source : {High, LLVM}) {
        EXPECT_NE(Source.find("__builtin_ia32_" + Name.substr(9) + "(" +
                              std::to_string(E.Index)),
                  std::string::npos)
            << Source;
        compileNativeC(Compiler, Directory, GetParam().Format, Source, Name,
                       Cases);
        if (::testing::Test::HasFailure())
          return;
      }
    }
  EXPECT_EQ(Cases, 312u);
  RecordProperty("native_system_register_c_compilations", Cases);
}

namespace {
struct DescriptorStateEncoding {
  X86DescriptorStateNative Plan;
  size_t Offset;
  std::vector<uint8_t> Bytes;
};

std::vector<DescriptorStateEncoding> descriptorStateEncodings(Arch A) {
  struct Encoding {
    Intrinsic Id;
    uint8_t Opcode;
    uint8_t Extension;
  };
  const Encoding Opcodes[] = {
      {Intrinsic::Lgdt, 1, 2},      {Intrinsic::Lidt, 1, 3},
      {Intrinsic::Sgdt, 1, 0},      {Intrinsic::Sidt, 1, 1},
      {Intrinsic::Invlpg, 1, 7},    {Intrinsic::Lldt, 0, 2},
      {Intrinsic::Ltr, 0, 3},       {Intrinsic::Lmsw, 1, 6},
      {Intrinsic::Sldt, 0, 0},      {Intrinsic::Str, 0, 1},
      {Intrinsic::Smsw, 1, 4},      {Intrinsic::Verr, 0, 4},
      {Intrinsic::Verw, 0, 5},      {Intrinsic::X86Lgdt16, 1, 2},
      {Intrinsic::X86Lidt16, 1, 3}, {Intrinsic::X86Sgdt16, 1, 0},
      {Intrinsic::X86Sidt16, 1, 1}};
  std::vector<DescriptorStateEncoding> Result;
  for (const auto &E : Opcodes)
    for (bool Memory : {false, true})
      for (unsigned Size : {0u, 1u, 2u, 4u, 8u})
        for (auto Space :
             {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
              NdMemoryAddressSpace::X86GS}) {
          // Verify's dead result is covered by the independent raw-call tests;
          // here return native ZF, rather than the pre-instruction EAX seed.
          if (isX86DescriptorVerify(E.Id) && Size != 1)
            continue;
          const auto Plan =
              x86DescriptorStateNative(E.Id, A, Memory, Size, Space);
          if (!Plan)
            continue;
          std::vector<uint8_t> Bytes = {0x31, 0xc0};
          const size_t Offset = Bytes.size();
          if (Space != NdMemoryAddressSpace::Default)
            Bytes.push_back(Space == NdMemoryAddressSpace::X86FS ? 0x64 : 0x65);
          if (isX86DescriptorTable16(E.Id) || (!Memory && Size == 2))
            Bytes.push_back(0x66);
          if (!Memory && Size == 8)
            Bytes.push_back(0x48);
          Bytes.insert(Bytes.end(),
                       {0x0f, E.Opcode,
                        uint8_t((Memory ? 0 : 0xc0) | (E.Extension << 3))});
          if (Plan->Verify)
            Bytes.insert(Bytes.end(), {0x0f, 0x94, 0xc0});
          Bytes.push_back(0xc3);
          Result.push_back({*Plan, Offset, std::move(Bytes)});
        }
  const auto Live = Result;
  for (const auto &E : Live) {
    if (!E.Plan.ResultBytes)
      continue;
    auto Dead = E;
    Dead.Bytes.insert(Dead.Bytes.end() - 1, {0x31, 0xc0});
    Result.push_back(std::move(Dead));
  }

  return Result;
}
} // namespace

TEST_P(X86SystemRegisterAccess, DescriptorStateFormsSurviveBothSourceRoutes) {
  const auto A = GetParam().TheArch;
  unsigned Cases = 0;
  for (const auto &E : descriptorStateEncodings(A)) {
    SCOPED_TRACE(E.Plan.LLVMName);
    SCOPED_TRACE(E.Plan.Segment);
    auto Image = image(A, GetParam().Format, E.Bytes);
    const bool Fault = A == Arch::X64 &&
                       GetParam().Format == BinaryFormat::ELF &&
                       E.Plan.Memory && !E.Plan.Address;
    compileRoutes(Image, false, Entry + E.Offset, {}, std::nullopt, false,
                  Fault, {}, {}, 0, true);
    if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped())
      return;
    ++Cases;
  }
  RecordProperty("descriptor_state_source_fixtures", Cases);
  EXPECT_GT(Cases, 40u);
}

TEST_P(X86SystemRegisterAccess, DescriptorStateUsesNeverCNativeBuiltins) {
  if (GetParam().TheArch != Arch::X64)
    GTEST_SKIP() << "NeverC targets x64";
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-descriptor-state-c",
                                                    Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (const auto &E : descriptorStateEncodings(Arch::X64)) {
    SCOPED_TRACE(E.Plan.LLVMName);
    SCOPED_TRACE(E.Plan.Segment);
    for (bool NoOpt : {false, true}) {
      auto Image = image(Arch::X64, GetParam().Format, E.Bytes);
      std::string High, LLVM, Native;
      sourceRoutes(Image, NoOpt, Entry + E.Offset, {}, &High, &LLVM, {}, {},
                   &Native);
      if (::testing::Test::HasFailure())
        return;
      if (llvm::Intrinsic::lookupIntrinsicID(E.Plan.LLVMName)) {
        EXPECT_NE(Native.find("@" + E.Plan.LLVMName + "("), std::string::npos)
            << Native;
        EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
      }
      const auto NativeName =
          E.Plan.LLVMName +
          (E.Plan.Memory && !E.Plan.Address ? ".address" : "");
      for (const auto &Source : {High, LLVM}) {
        compileNativeC(Compiler, Directory, GetParam().Format, Source,
                       NativeName, Cases);
        if (::testing::Test::HasFailure())
          return;
      }
    }
  }
  EXPECT_EQ(Cases, descriptorStateEncodings(Arch::X64).size() * 8);
  RecordProperty("native_descriptor_state_c_compilations", Cases);
}

TEST(X86DescriptorStateSource,
     SharedShapesRejectLostWidthsAndForeignRegisters) {
  for (auto A : {Arch::X86, Arch::X64})
    for (const auto &E : descriptorStateEncodings(A)) {
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.NumInputs = E.Plan.OperandBytes ? 2 : 1;
      Op.Inputs[0].Kind = MedVar::Const;
      Op.Inputs[0].ConstVal = unsigned(E.Plan.Id);
      Op.Inputs[0].Size = 2;
      Op.Inputs[1].Kind = MedVar::Temp;
      Op.Inputs[1].Size = E.Plan.Memory ? 8 : E.Plan.OperandBytes;
      Op.Output.Kind = MedVar::Temp;
      Op.Output.Size = E.Plan.ResultBytes;
      Op.MemoryAddressSpace =
          E.Plan.Segment == "fs"   ? NdMemoryAddressSpace::X86FS
          : E.Plan.Segment == "gs" ? NdMemoryAddressSpace::X86GS
                                   : NdMemoryAddressSpace::Default;
      ASSERT_TRUE(x86DescriptorStateMedShapeIsValid(Op, A));
      if (Op.NumInputs == 2) {
        auto Invalid = Op;
        Invalid.Inputs[1].Size = 1;
        EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Invalid, A));
        Invalid = Op;
        Invalid.Inputs[1].Kind = MedVar::Reg;
        Invalid.Inputs[1].TheArch = Arch::AArch64;
        EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Invalid, A));
      } else {
        auto Invalid = Op;
        Invalid.Output.Size = 0;
        EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Invalid, A));
      }
      if (Op.Output.Size) {
        auto Invalid = Op;
        Invalid.Output.Kind = MedVar::Reg;
        Invalid.Output.TheArch = Arch::AArch64;
        EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Invalid, A));
        auto Flag = Op;
        Flag.Output.Kind = MedVar::Flag;
        Flag.Output.TheArch = A;
        EXPECT_EQ(x86DescriptorStateMedShapeIsValid(Flag, A), E.Plan.Verify);
        Flag.Output.TheArch = Arch::AArch64;
        EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Flag, A));
      }
      auto Invalid = Op;
      ++Invalid.NumInputs;
      EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Invalid, A));
      Invalid = Op;
      Invalid.Inputs[0].Size = 8;
      EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Invalid, A));
      Invalid = Op;
      Invalid.Inputs[0].ConstVal += UINT64_C(1) << 16;
      EXPECT_FALSE(x86DescriptorStateMedShapeIsValid(Invalid, A));
    }
}

TEST(X86DescriptorStateSource, VerifyResultsAgreeWithOriginalInstructions) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires native Linux x86-64 descriptor checks";
#endif
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  for (bool Write : {false, true})
    for (bool Memory : {false, true})
      for (bool ValidSelector : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(Write);
          SCOPED_TRACE(Memory);
          SCOPED_TRACE(ValidSelector);
          SCOPED_TRACE(NoOpt);
          std::vector<uint8_t> Bytes = {0x31, 0xc0};
          // Linux's user CS is readable but not writable. A null selector
          // fails both checks. No privileged operation is executed here.
          if (ValidSelector)
            Bytes.insert(Bytes.end(), {0x8c, 0xc8}); // mov ax, cs
          if (Memory)
            Bytes.push_back(0x50); // push rax: independent m16 selector cell
          const auto Offset = Bytes.size();
          Bytes.insert(
              Bytes.end(),
              {0x0f, 0x00, uint8_t((Memory ? 0x24 : 0xe0) + (Write ? 8 : 0))});
          if (Memory)
            Bytes.push_back(0x24);                       // SIB [rsp]
          Bytes.insert(Bytes.end(), {0x0f, 0x94, 0xc0}); // setz al
          if (Memory)
            Bytes.insert(Bytes.end(), {0x48, 0x83, 0xc4, 8});
          Bytes.insert(Bytes.end(), {0x0f, 0xb6, 0xc0, 0xc3});
          auto Image = image(Arch::X64, BinaryFormat::ELF, Bytes);
          const uint64_t Expected = ValidSelector && !Write;
          compileRoutes(Image, NoOpt, Entry + Offset, {}, Expected, true);
          if (::testing::Test::HasFailure())
            return;
          if (NeverC && llvm::sys::fs::exists(NeverC)) {
            compileRoutes(Image, NoOpt, Entry + Offset, {}, Expected, true,
                          false, {}, {}, 0, false, NeverC);
            if (::testing::Test::HasFailure())
              return;
          }
        }
}

TEST(X86DescriptorStateSource, LegacyVerifyCallsKeepDeadResultsAndSegments) {
  for (auto A : {Arch::X86, Arch::X64})
    for (bool Memory : {false, true})
      for (bool Dead : {false, true})
        for (auto Space :
             {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
              NdMemoryAddressSpace::X86GS}) {
          const auto P = x86DescriptorStateNative(Intrinsic::Verw, A, Memory,
                                                  Dead ? 0 : 1, Space);
          if (!P)
            continue;
          llvm::LLVMContext Context;
          llvm::Module Module("legacy_descriptor", Context);
          llvm::IRBuilder<> B(Context);
          std::string Text = P->Text, Constraints = P->Constraints;
          if (P->Address)
            Constraints.replace(Constraints.find("{bx}"), 4, "r");
          if (Dead) {
            Text = Text.substr(0, Text.find("\n\tsetz $0"));
            Text.replace(Text.find("$1"), 2, "$0");
            Constraints.erase(0, 3);
          }
          auto *OperandTy = B.getIntNTy(8 * P->OperandBytes);
          auto *FnTy = llvm::FunctionType::get(
              Dead ? B.getVoidTy() : B.getInt8Ty(), {OperandTy}, false);
          auto *Function = llvm::Function::Create(
              FnTy, llvm::Function::ExternalLinkage, "probe", Module);
          B.SetInsertPoint(
              llvm::BasicBlock::Create(Context, "entry", Function));
          auto *Call =
              B.CreateCall(llvm::InlineAsm::get(FnTy, Text, Constraints, true),
                           {Function->getArg(0)});
          if (Dead)
            B.CreateRetVoid();
          else
            B.CreateRet(Call);
          ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
          const auto Code = renderX86DescriptorStateCall(
              A, BinaryFormat::ELF, *Call, Dead ? "" : "result", !Dead,
              {"input"});
          ASSERT_TRUE(Code);
          if (A == Arch::X64)
            EXPECT_NE(Code->Code.find(P->Builtin), std::string::npos)
                << Code->Code;
          if (Dead)
            EXPECT_TRUE(llvm::StringRef(Code->Code).starts_with("(void)"))
                << Code->Code;
        }
}

TEST(X86DescriptorStateSource, NativeCallsRequireExactControlsTypesAndEffects) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.verr.mem.address"))
    GTEST_SKIP() << "requires the explicit descriptor-state address SDK";
  for (auto A : {Arch::X86, Arch::X64})
    for (const auto &E : descriptorStateEncodings(A)) {
      auto P = E.Plan;
      const auto ID = llvm::Intrinsic::lookupIntrinsicID(P.LLVMName);
      ASSERT_NE(ID, llvm::Intrinsic::not_intrinsic);
      SCOPED_TRACE(P.LLVMName);
      llvm::LLVMContext Context;
      llvm::Module Module("descriptor_state", Context);
      auto *Callee = llvm::Intrinsic::getOrInsertDeclaration(&Module, ID);
      auto *Function = llvm::Function::Create(Callee->getFunctionType(),
                                              llvm::Function::ExternalLinkage,
                                              "probe", Module);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
      std::vector<llvm::Value *> Inputs;
      for (auto &Arg : Function->args())
        Inputs.push_back(&Arg);
      if (P.Address) {
        Inputs[1] = B.getInt32(P.Address->Bits);
        Inputs[2] = B.getInt32(P.Address->Prefix);
      }
      auto *Call = B.CreateCall(Callee, Inputs);
      if (P.ResultBytes)
        B.CreateRet(Call);
      else
        B.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      const std::vector<std::string> Args(Inputs.size(), "input");
      auto Render = [&] {
        return renderX86DescriptorStateCall(A, BinaryFormat::ELF, *Call,
                                            "result", P.ResultBytes != 0, Args);
      };
      EXPECT_TRUE(Render());
      EXPECT_TRUE(renderX86DescriptorStateCall(A, BinaryFormat::ELF, *Call, "",
                                               false, Args));
      for (const auto Effects :
           {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
            llvm::MemoryEffects::writeOnly(),
            llvm::MemoryEffects::argMemOnly()}) {
        Call->setMemoryEffects(Effects);
        EXPECT_THROW((void)Render(), std::invalid_argument);
      }
      Call->setMemoryEffects(llvm::MemoryEffects::unknown());
      if (P.Address) {
        for (unsigned Bits : {0u, 8u, 128u}) {
          Call->setArgOperand(1, B.getInt32(Bits));
          EXPECT_THROW((void)Render(), std::invalid_argument);
        }
        Call->setArgOperand(1, Inputs[1]);
        Call->setArgOperand(2, B.getInt32(1));
        EXPECT_THROW((void)Render(), std::invalid_argument);
        Call->setArgOperand(2, &*Function->arg_begin());
        EXPECT_THROW((void)Render(), std::invalid_argument);
        Call->setArgOperand(2, Inputs[2]);
      }
      if (!Inputs.empty()) {
        Call->setArgOperand(0, B.getInt8(0));
        EXPECT_THROW((void)Render(), std::invalid_argument);
      }
    }
}

namespace {
struct ScalarSystemEncoding {
  X86ScalarSystemNative Plan;
  bool Immediate;
  bool Dead;
  size_t Offset;
  std::vector<uint8_t> Bytes;
};

std::vector<ScalarSystemEncoding> scalarSystemEncodings() {
  std::vector<ScalarSystemEncoding> Result;
  for (const auto &Plan : X86ScalarSystemNativeInventory)
    for (bool Immediate : {false, true}) {
      if (Immediate && !Plan.Port)
        continue;
      for (bool Dead : {false, true}) {
        if (Dead && Plan.Write)
          continue;
        std::vector<uint8_t> Bytes = {0xb8, 0x12, 0x34, 0x56, 0x87,
                                      0xb9, 0x00, 0x01, 0x00, 0xc0,
                                      0xba, 0x34, 0x12, 0x00, 0x00};
        const auto Offset = Bytes.size();
        if (Plan.Port) {
          if (Plan.Bytes == 2)
            Bytes.push_back(0x66);
          Bytes.push_back(uint8_t((Immediate ? 0xe4 : 0xec) + 2 * Plan.Write +
                                  (Plan.Bytes != 1)));
          if (Immediate)
            Bytes.push_back(0x34);
        } else {
          Bytes.insert(Bytes.end(), {0x0f, uint8_t(Plan.Write ? 0x30 : 0x32)});
        }
        if (Dead || Plan.Write)
          Bytes.insert(Bytes.end(), {0x31, 0xc0});
        Bytes.push_back(0xc3);
        Result.push_back({Plan, Immediate, Dead, Offset, std::move(Bytes)});
      }
    }
  return Result;
}
} // namespace

TEST_P(X86SystemRegisterAccess,
       ScalarMsrAndPortEffectsSurviveBothSourceRoutes) {
  const auto A = GetParam().TheArch;
  const auto Format = GetParam().Format;
  unsigned Fixtures = 0;
  for (const auto &E : scalarSystemEncodings()) {
    SCOPED_TRACE(E.Plan.LLVMName);
    SCOPED_TRACE(E.Immediate);
    SCOPED_TRACE(E.Dead);
    auto Image = image(A, Format, E.Bytes);
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt);
      compileRoutes(Image, NoOpt, Entry + E.Offset, {}, std::nullopt, false,
                    A == Arch::X64 && Format == BinaryFormat::ELF, {}, {}, 0,
                    true);
      if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped())
        return;
    }
    ++Fixtures;
  }
  EXPECT_EQ(Fixtures, 21u);
  RecordProperty("scalar_system_source_fixtures", Fixtures);
}

TEST_P(X86SystemRegisterAccess, ScalarMsrAndPortsUseNeverCNativeIntrinsics) {
  if (GetParam().TheArch != Arch::X64)
    GTEST_SKIP() << "NeverC targets x64";
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-scalar-system-c",
                                                    Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (bool NoOpt : {false, true})
    for (const auto &E : scalarSystemEncodings()) {
      SCOPED_TRACE(E.Plan.LLVMName);
      SCOPED_TRACE(E.Immediate);
      SCOPED_TRACE(E.Dead);
      SCOPED_TRACE(NoOpt);
      auto Image = image(Arch::X64, GetParam().Format, E.Bytes);
      std::string High, LLVM, Native;
      sourceRoutes(Image, NoOpt, Entry + E.Offset, {}, &High, &LLVM, {}, {},
                   &Native);
      if (::testing::Test::HasFailure())
        return;
      if (llvm::Intrinsic::lookupIntrinsicID(E.Plan.LLVMName)) {
        EXPECT_NE(Native.find(std::string("@") + E.Plan.LLVMName + "("),
                  std::string::npos)
            << Native;
        EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
        if (!NoOpt && E.Plan.Port)
          EXPECT_NE(Native.find(std::string(E.Plan.LLVMName) + "(i16 " +
                                (E.Immediate ? "52" : "4660")),
                    std::string::npos)
              << Native;
      }
      for (const auto &Source : {High, LLVM}) {
        EXPECT_NE(Source.find(std::string(E.Plan.Builtin) + "("),
                  std::string::npos)
            << Source;
        compileNativeC(Compiler, Directory, GetParam().Format, Source,
                       E.Plan.LLVMName, Cases,
                       E.Plan.Port
                           ? std::optional<uint16_t>(E.Immediate ? 52 : 4660)
                           : std::nullopt);
        if (::testing::Test::HasFailure())
          return;
      }
    }
  EXPECT_EQ(Cases, 168u);
  RecordProperty("native_scalar_system_c_compilations", Cases);
}

TEST(X86ScalarSystemSource, NativeCallsRequireExactTypesAndEffects) {
  for (const auto &P : X86ScalarSystemNativeInventory) {
    const auto ID = llvm::Intrinsic::lookupIntrinsicID(P.LLVMName);
    if (!ID)
      GTEST_SKIP() << "requires the scalar MSR/port intrinsic SDK";
    SCOPED_TRACE(P.LLVMName);
    llvm::LLVMContext Context;
    llvm::Module Module("scalar_system", Context);
    auto *Callee = llvm::Intrinsic::getOrInsertDeclaration(&Module, ID);
    auto *Function = llvm::Function::Create(Callee->getFunctionType(),
                                            llvm::Function::ExternalLinkage,
                                            "scalar_system", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    std::vector<llvm::Value *> Inputs;
    for (auto &Argument : Function->args())
      Inputs.push_back(&Argument);
    auto *Call = Builder.CreateCall(Callee, Inputs);
    if (P.Write)
      Builder.CreateRetVoid();
    else
      Builder.CreateRet(Call);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    const std::vector<std::string> Arguments(Inputs.size(), "input");
    auto Render = [&](Arch Target, bool Live = true,
                      const std::string &Result = "result") {
      return renderX86ScalarSystemCall(Target, BinaryFormat::ELF, *Call, Result,
                                       Live && !P.Write, Arguments);
    };
    for (auto A : {Arch::X86, Arch::X64}) {
      EXPECT_TRUE(Render(A));
      EXPECT_TRUE(Render(A, false, ""));
    }
    EXPECT_THROW((void)Render(Arch::AArch64), std::invalid_argument);
    for (const auto Effects :
         {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
          llvm::MemoryEffects::writeOnly(), llvm::MemoryEffects::argMemOnly(),
          llvm::MemoryEffects::inaccessibleMemOnly()}) {
      Call->setMemoryEffects(Effects);
      EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
    }
    Call->setMemoryEffects(llvm::MemoryEffects::unknown());
    Call->setDoesNotReturn();
    EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
    Call->removeFnAttr(llvm::Attribute::NoReturn);
    Call->setCallingConv(llvm::CallingConv::Fast);
    EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
    Call->setCallingConv(llvm::CallingConv::C);
    if (!P.Write)
      EXPECT_THROW((void)Render(Arch::X64, true, ""), std::invalid_argument);
    EXPECT_THROW((void)renderX86ScalarSystemCall(Arch::X64, BinaryFormat::ELF,
                                                 *Call, "result", !P.Write, {}),
                 std::invalid_argument);
    Call->setArgOperand(0, Builder.getInt64(1));
    EXPECT_THROW((void)Render(Arch::X64), std::invalid_argument);
  }
}

TEST(X86SystemRegisterSource, NativeCallsRequireExactRegisterModeAndEffects) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.readcr"))
    GTEST_SKIP() << "requires the native control/debug register SDK";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const bool Debug : {false, true})
      for (const bool Write : {false, true}) {
        const std::string Name =
            "llvm.x86." + std::string(Write ? "write" : "read") +
            (Debug ? "dr" : "cr") + (A == Arch::X86 ? "32" : "");
        SCOPED_TRACE(Name);
        llvm::LLVMContext Context;
        llvm::Module Module("system_register_contract", Context);
        auto *Callee = llvm::Intrinsic::getOrInsertDeclaration(
            &Module, llvm::Intrinsic::lookupIntrinsicID(Name));
        auto *Function = llvm::Function::Create(Callee->getFunctionType(),
                                                llvm::Function::ExternalLinkage,
                                                "system_register", Module);
        llvm::IRBuilder<> Builder(
            llvm::BasicBlock::Create(Context, "entry", Function));
        std::vector<llvm::Value *> Inputs = {Builder.getInt32(3)};
        if (Write)
          Inputs.push_back(Function->getArg(1));
        auto *Call = Builder.CreateCall(Callee, Inputs);
        if (Write)
          Builder.CreateRetVoid();
        else
          Builder.CreateRet(Call);
        ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
        const std::vector<std::string> Arguments(Inputs.size(), "input");
        auto Render = [&](Arch Target, bool Live = true,
                          const std::string &Result = "result") {
          return renderX86SystemRegisterCall(Target, BinaryFormat::ELF, *Call,
                                             Result, Live && !Write, Arguments);
        };
        for (unsigned Index = 0; Index <= 9; ++Index) {
          SCOPED_TRACE(Index);
          Call->setArgOperand(0, Builder.getInt32(Index));
          const bool Valid = Debug ? Index < 8
                                   : Index == 0 || Index == 2 || Index == 3 ||
                                         Index == 4 ||
                                         (Index == 8 && A == Arch::X64);
          if (Valid) {
            EXPECT_TRUE(Render(A));
            EXPECT_TRUE(Render(A, false, ""));
          } else
            EXPECT_THROW((void)Render(A), std::invalid_argument);
        }
        Call->setArgOperand(0, Builder.getInt32(UINT32_MAX));
        EXPECT_THROW((void)Render(A), std::invalid_argument);
        Call->setArgOperand(0, Function->getArg(0));
        EXPECT_THROW((void)Render(A), std::invalid_argument);
        Call->setArgOperand(0, Inputs[0]);
        EXPECT_THROW((void)Render(A == Arch::X86 ? Arch::X64 : Arch::X86),
                     std::invalid_argument);
        EXPECT_THROW((void)Render(Arch::AArch64), std::invalid_argument);
        for (const auto Effects :
             {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
              llvm::MemoryEffects::writeOnly(),
              llvm::MemoryEffects::argMemOnly(),
              llvm::MemoryEffects::inaccessibleMemOnly(),
              llvm::MemoryEffects::inaccessibleOrArgMemOnly()}) {
          Call->setMemoryEffects(Effects);
          EXPECT_THROW((void)Render(A), std::invalid_argument);
        }
        Call->setMemoryEffects(llvm::MemoryEffects::unknown());
        Call->setDoesNotReturn();
        EXPECT_THROW((void)Render(A), std::invalid_argument);
        Call->removeFnAttr(llvm::Attribute::NoReturn);
        Call->setCallingConv(llvm::CallingConv::Fast);
        EXPECT_THROW((void)Render(A), std::invalid_argument);
        Call->setCallingConv(llvm::CallingConv::C);
        if (!Write)
          EXPECT_THROW((void)Render(A, true, ""), std::invalid_argument);
        EXPECT_THROW((void)renderX86SystemRegisterCall(
                         A, BinaryFormat::ELF, *Call, "result", !Write, {}),
                     std::invalid_argument);
      }
}

TEST(X86VirtualizationSource, NeverCCompilesBothRoutesToNativeIntrinsics) {
  const char *Configured = std::getenv("NEVERD_TEST_NEVERC");
  if (!Configured || !llvm::sys::fs::exists(Configured))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  const std::string Compiler = Configured;
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-native-vmx-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (const auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true})
      for (const auto &E : nativeVirtualizationEncodings(Arch::X64)) {
        const auto C = x86VirtualizationContract(E.Id);
        for (const auto Space :
             {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
              NdMemoryAddressSpace::X86GS}) {
          const auto Plan = x86VirtualizationNative(
              E.Id, Arch::X64, Space, physicalAddressBytes(E, Arch::X64));
          if (!Plan)
            continue;
          SCOPED_TRACE(Plan->LLVMName);
          SCOPED_TRACE(NoOpt);
          SCOPED_TRACE(static_cast<unsigned>(Format));
          auto Bytes = E.Bytes;
          if (Space != NdMemoryAddressSpace::Default)
            Bytes.insert(Bytes.begin(), Plan->SegmentPrefix);
          size_t Offset;
          auto Image =
              image(Arch::X64, Format, function(Arch::X64, Bytes, Offset));
          std::string High, LLVM;
          sourceRoutes(Image, NoOpt, Entry + Offset, C.Mnemonic, &High, &LLVM);
          if (::testing::Test::HasFailure())
            return;
          for (const auto &Source : {High, LLVM}) {
            compileNativeC(Compiler, Directory, Format, Source, Plan->LLVMName,
                           Cases);
            if (::testing::Test::HasFailure())
              return;
          }
        }
      }
  RecordProperty("native_c_compilations", Cases);
}

namespace {
// Intel SDM 093 and APX revision 9: RAX is the data operand and RCX the
// register selector. In particular, URDMSR writes ModRM.r/m, not ModRM.reg.
const std::vector<SystemEncoding> MsrEncodings = {
    {{0xc4, 0xe7, 0x7b, 0xf6, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "rdmsr"},
    {{0xc4, 0xe7, 0x7a, 0xf6, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "wrmsrns"},
    {{0x62, 0xf7, 0x7f, 0x08, 0xf6, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "rdmsr"},
    {{0x62, 0xf7, 0x7e, 0x08, 0xf6, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "wrmsrns"},
    {{0xf2, 0x0f, 0x38, 0xf8, 0xc8}, Intrinsic::X86MsrAccess, "urdmsr"},
    {{0xf3, 0x0f, 0x38, 0xf8, 0xc8}, Intrinsic::X86MsrAccess, "uwrmsr"},
    {{0xc4, 0xe7, 0x7b, 0xf8, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "urdmsr"},
    {{0xc4, 0xe7, 0x7a, 0xf8, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "uwrmsr"},
    {{0x62, 0xf4, 0x7f, 0x08, 0xf8, 0xc8}, Intrinsic::X86MsrAccess, "urdmsr"},
    {{0x62, 0xf4, 0x7e, 0x08, 0xf8, 0xc8}, Intrinsic::X86MsrAccess, "uwrmsr"},
    {{0x62, 0xf7, 0x7f, 0x08, 0xf8, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "urdmsr"},
    {{0x62, 0xf7, 0x7e, 0x08, 0xf8, 0xc0, 0xff, 0xff, 0xff, 0xff},
     Intrinsic::X86MsrAccess,
     "uwrmsr"},
};

std::vector<uint8_t> msrFunction(const SystemEncoding &E, bool DeadResult,
                                 size_t &Offset) {
  std::vector<uint8_t> Bytes = {0x48, 0xb8, 0x88, 0x77, 0x66, 0x55,
                                0x44, 0x33, 0x22, 0x11, 0x48, 0xc7,
                                0xc1, 0xff, 0xff, 0xff, 0xff};
  Offset = Bytes.size();
  Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
  if (DeadResult)
    Bytes.insert(Bytes.end(), {0x31, 0xc0});
  Bytes.push_back(0xc3);
  return Bytes;
}
std::string msrFaultDriver(const BinaryImage &Image, const SystemEncoding &E,
                           bool Dead) {
  std::string Driver = faultDriver(Image);
  for (const auto Name : {"virtualization_test", "neverd_original"}) {
    const auto Declaration = "extern void " + std::string(Name);
    Driver.replace(Driver.find(Declaration), Declaration.size(),
                   "extern unsigned long long " + std::string(Name));
  }
  const std::string FunctionPointer = "void (*fn)(void)";
  Driver.replace(Driver.find(FunctionPointer), FunctionPointer.size(),
                 "unsigned long long (*fn)(void)");
  const std::string Main = "int main(void)";
  Driver.replace(Driver.find(Main), Main.size(),
                 "static int compare_faults(void)");
  Driver = "#define _GNU_SOURCE\n#include <stdint.h>\n#include <ucontext.h>\n" +
           Driver;
  llvm::raw_string_ostream OS(Driver);
  OS << "\nstatic const unsigned char instruction[] = {";
  for (auto Byte : E.Bytes)
    OS << unsigned(Byte) << ',';
  const bool Write = llvm::StringRef(E.Mnemonic).contains("wrmsr");
  OS << "};\nstatic const int writes_msr = " << Write << ";\n"
     << "static const int register_selector = " << (E.Bytes.size() <= 6)
     << ";\nstatic const uint64_t expected = "
     << (Dead    ? "0"
         : Write ? "UINT64_C(0x1122334455667788)"
                 : "UINT64_C(0xfedcba9876543210)")
     << ";\n";
  // The second observer supplies a controlled register completion after the
  // real fault comparison. It verifies data transport, not MSR accessibility.
  OS << R"C(
static volatile sig_atomic_t events;
static void complete(int signal, siginfo_t *info, void *context) {
  ucontext_t *uc = context;
  greg_t *g = uc->uc_mcontext.gregs;
  const unsigned char *ip = (const unsigned char *)(uintptr_t)g[REG_RIP];
  (void)signal; (void)info;
  for (unsigned i = 0; i < sizeof(instruction); ++i)
    if (ip[i] != instruction[i]) _exit(21);
  if (register_selector && (uint64_t)g[REG_RCX] != UINT64_MAX) _exit(22);
  if (writes_msr) {
    if ((uint64_t)g[REG_RAX] != UINT64_C(0x1122334455667788)) _exit(23);
  } else {
    g[REG_RAX] = (greg_t)UINT64_C(0xfedcba9876543210);
  }
  g[REG_RIP] += sizeof(instruction);
  ++events;
}
int main(void) {
  if (compare_faults()) return 10;
  struct sigaction action = {0};
  action.sa_sigaction = complete;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, 0) || sigaction(SIGSEGV, &action, 0)) return 11;
  events = 0;
  if (neverd_original() != expected || events != 1) return 12;
  events = 0;
  if (virtualization_test() != expected || events != 1) return 13;
  return 0;
}
)C";
  return Driver;
}
} // namespace

TEST(X86MsrAccessSource, EveryEncodingRetainsItsNativeOperandsAndEffects) {
  for (const auto &E : MsrEncodings)
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (bool Dead : {false, true}) {
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          SCOPED_TRACE(Dead);
          size_t Offset;
          auto Image = image(Arch::X64, Format, msrFunction(E, Dead, Offset));
          std::string High, LLVM;
          std::vector<uint8_t> Object;
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, &High, &LLVM,
                       {}, {}, nullptr, &Object, Dead ? 0 : 8);
          for (const auto *Source : {&High, &LLVM}) {
            EXPECT_NE(Source->find("__asm__ volatile"), std::string::npos);
            EXPECT_NE(Source->find("\"memory\""), std::string::npos);
          }
          EXPECT_NE(std::search(Object.begin(), Object.end(), E.Bytes.begin(),
                                E.Bytes.end()),
                    Object.end());
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86MsrAccessSource, FaultsAndRegisterTransportMatchAcrossEveryRoute) {
  // CPL3 forbids the privileged forms. USER_MSR selectors exceed 0x3fff, so
  // these probes also fail on hosts that enable USER_MSR. No MSR is changed.
  for (const auto &E : MsrEncodings)
    for (bool NoOpt : {false, true})
      for (bool Dead : {false, true}) {
        SCOPED_TRACE(E.Mnemonic);
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Dead);
        size_t Offset;
        auto Image =
            image(Arch::X64, BinaryFormat::ELF, msrFunction(E, Dead, Offset));
        compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                      false, false, msrFaultDriver(Image, E, Dead), E.Bytes,
                      Dead ? 0 : 8);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86MsrAccessSource, MalformedLLVMAssemblyCannotBypassTheContract) {
  for (unsigned K = 0; K < static_cast<unsigned>(X86MsrAccessKind::Count); ++K)
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(K);
      SCOPED_TRACE(Mutation);
      const auto Kind = static_cast<X86MsrAccessKind>(K);
      const bool Write = x86MsrAccessIsWrite(Kind);
      const bool Immediate = x86MsrAccessHasImmediateSelector(Kind);
      if ((Mutation == 8 && !Immediate) || (Mutation == 10 && Write))
        continue;
      const auto P = x86MsrAccessAsm(Kind, Arch::X64);
      ASSERT_TRUE(P);
      llvm::LLVMContext Context;
      llvm::Module Module("msr-contract", Context);
      Module.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(Context);
      std::vector<llvm::Type *> Types;
      for (const auto Width : P->InputBytes)
        Types.push_back(B.getIntNTy(8 * Width));
      if (Mutation == 4)
        Types[0] = B.getInt16Ty();
      llvm::Type *Return = Write ? B.getVoidTy() : B.getInt64Ty();
      if (Mutation == 10)
        Return = B.getInt32Ty();
      auto *Signature = llvm::FunctionType::get(Return, Types, false);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, "effect", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      std::vector<llvm::Value *> Values;
      for (unsigned I = 0; I < Types.size(); ++I)
        Values.push_back(I == 0 && Immediate && Mutation != 8
                             ? llvm::ConstantInt::getAllOnesValue(Types[I])
                             : static_cast<llvm::Value *>(Function->getArg(I)));
      std::string Constraints = P->Constraints;
      if (Mutation == 1)
        Constraints.erase(Constraints.find(",~{memory}"));
      if (Mutation == 11)
        Constraints.replace(Constraints.find("{ax}"), 4, "{dx}");
      auto *Asm = llvm::InlineAsm::get(
          Signature, P->Text + (Mutation == 3 ? "\n\tnop" : ""), Constraints,
          Mutation != 2, false,
          Mutation == 6 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
      auto *Call = B.CreateCall(Asm, Values);
      if (Mutation == 5)
        Call->setOnlyReadsMemory();
      if (Mutation == 7)
        Call->setOnlyWritesMemory();
      if (Write)
        B.CreateRetVoid();
      else
        B.CreateRet(Call);
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Mutation == 9 ? Arch::X86 : Arch::X64;
      Options.EmitIncludes = false;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      if (Mutation == 0)
        EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Text;
      else
        EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                     std::invalid_argument);
    }
}

TEST(X86MsrAccessSource, RegisterFormsAcceptRecoveredFunctionArguments) {
  for (const auto &E : MsrEncodings) {
    if (E.Bytes.size() > 6)
      continue;
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(E.Mnemonic);
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        // (selector, data): System V uses RDI/RSI, Win64 uses RCX/RDX.
        std::vector<uint8_t> Bytes =
            Format == BinaryFormat::COFF
                ? std::vector<uint8_t>{0x48, 0x89, 0xd0}
                : std::vector<uint8_t>{0x48, 0x89, 0xf0, 0x48, 0x89, 0xf9};
        const auto Offset = Bytes.size();
        Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
        Bytes.push_back(0xc3);
        auto Image = image(Arch::X64, Format, std::move(Bytes));
        if (Format == BinaryFormat::ELF)
          compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                        false, false, {}, E.Bytes);
        else
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic);
        if (::testing::Test::HasFailure())
          return;
      }
  }
}

namespace {
std::vector<uint8_t> nativeSystemFunction(Arch A, const SystemEncoding &E,
                                          size_t &Offset) {
  auto Bytes = function(A, E.Bytes, Offset);
  if (A == Arch::X64) {
    // RSI/RDI are implicit inputs to LKGS/MSRLIST. They are not Win64
    // parameters, so initialize them rather than qualifying an unknown-state
    // trap that an optimizer can remove together with the tested instruction.
    const std::vector<uint8_t> Inputs = {
        0x48, 0xbe, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x48, 0xbf, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    Bytes.insert(Bytes.begin() + Offset, Inputs.begin(), Inputs.end());
    Offset += Inputs.size();
  }
  return Bytes;
}

std::string nativeSystemName(const SystemEncoding &E, Arch A) {
  switch (E.Id) {
  case Intrinsic::Clac:
    return "llvm.x86.clac";
  case Intrinsic::Stac:
    return "llvm.x86.stac";
  case Intrinsic::Xend:
    return "llvm.x86.xend";
  case Intrinsic::Clzero:
    return E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65
               ? ""
               : "llvm.x86.clzero";
  case Intrinsic::X86LoadAccessRights:
  case Intrinsic::X86LoadAccessRightsMemory:
  case Intrinsic::X86LoadSegmentLimit:
  case Intrinsic::X86LoadSegmentLimitMemory: {
    const bool Segmented = E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65;
    const bool Memory = E.Id == Intrinsic::X86LoadAccessRightsMemory ||
                        E.Id == Intrinsic::X86LoadSegmentLimitMemory;
    const auto Prefix = E.Bytes[Segmented ? 1 : 0];
    const unsigned Width = Prefix == 0x66 ? 16 : Prefix == 0x48 ? 64 : 32;
    return "llvm.x86." + std::string(E.Mnemonic) + (Memory ? ".mem" : ".reg") +
           std::to_string(Width) +
           (Segmented ? A == Arch::X86 ? ".address32" : ".address" : "");
  }
  case Intrinsic::X86ClearTaskSwitched:
    return "llvm.x86.clts";
  case Intrinsic::X86WriteMsrNonSerializing:
    return "llvm.x86.wrmsrns";
  case Intrinsic::X86ReadMsrList:
    return "llvm.x86.rdmsrlist";
  case Intrinsic::X86WriteMsrList:
    return "llvm.x86.wrmsrlist";
  case Intrinsic::X86LoadKernelGs:
    return "llvm.x86.lkgs.reg";
  case Intrinsic::X86LoadKernelGsMemory:
    if (E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65)
      return "llvm.x86.lkgs.address";
    return "llvm.x86.lkgs.mem";
  case Intrinsic::X86InvalidateBroadcast:
    return A == Arch::X86            ? "llvm.x86.invlpgb32"
           : E.Bytes.front() == 0x67 ? "llvm.x86.invlpgb.addr32"
                                     : "llvm.x86.invlpgb";
  case Intrinsic::X86TlbSync:
    return "llvm.x86.tlbsync";
  default:
    return {};
  }
}
} // namespace

TEST(X86SystemStateSource, QualifiedSdkUsesNativeSystemCalls) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.invlpgb.addr32"))
    GTEST_SKIP() << "requires the qualified native system SDK";
  unsigned Cases = 0;
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const bool NoOpt : {false, true})
        for (const auto &E : systemEncodings(A)) {
          const auto Name = nativeSystemName(E, A);
          if (Name.empty())
            continue;
          SCOPED_TRACE(Name);
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Image = image(A, Format, nativeSystemFunction(A, E, Offset));
          std::string IR;
          std::vector<uint8_t> Object;
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, nullptr,
                       nullptr, {}, {}, &IR, &Object);
          if (::testing::Test::HasFailure())
            return;
          EXPECT_NE(IR.find("@" + Name + "("), std::string::npos) << IR;
          EXPECT_EQ(IR.find(" asm "), std::string::npos) << IR;
          ++Cases;
        }
  EXPECT_EQ(Cases, 312u);
  RecordProperty("native_system_modules", Cases);
}

TEST(X86SystemStateSource, SmapPreservesMemoryAccessOrderAcrossBothControls) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.invlpgb.addr32"))
    GTEST_SKIP() << "requires the qualified native system SDK";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        std::vector<uint8_t> Bytes;
        if (A == Arch::X86)
          Bytes = {0x8b, 0x4c, 0x24, 0x04}; // mov ecx, [esp+4]
        else if (Format != BinaryFormat::COFF)
          Bytes = {0x48, 0x89, 0xf9};            // mov rcx, rdi
        Bytes.insert(Bytes.end(), {0x8b, 0x11}); // mov edx, [rcx/ecx]
        const auto Offset = Bytes.size();
        Bytes.insert(Bytes.end(), {0x0f, 0x01, 0xcb, // stac
                                   0x8b, 0x01,       // mov eax, [rcx/ecx]
                                   0x89, 0x11,       // mov [rcx/ecx], edx
                                   0x0f, 0x01, 0xca, // clac
                                   0x33, 0x01,       // xor eax, [rcx/ecx]
                                   0xc3});
        auto Image = image(A, Format, std::move(Bytes));
        std::string High, LLVM, IR;
        std::vector<uint8_t> Object;
        sourceRoutes(Image, NoOpt, Entry + Offset, "stac", &High, &LLVM, {}, {},
                     &IR, &Object);
        if (::testing::Test::HasFailure())
          return;
        const auto Before = IR.find("load i32");
        const auto Open = IR.find("call void @llvm.x86.stac()");
        const auto During = IR.find("load i32", Before + 1);
        const auto Store = IR.find("store i32");
        const auto Close = IR.find("call void @llvm.x86.clac()");
        const auto After = IR.find("load i32", During + 1);
        ASSERT_NE(After, std::string::npos) << IR;
        EXPECT_LT(Before, Open) << IR;
        EXPECT_LT(Open, During) << IR;
        EXPECT_LT(During, Store) << IR;
        EXPECT_LT(Store, Close) << IR;
        EXPECT_LT(Close, After) << IR;
        for (const auto &Source : {High, LLVM}) {
          EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
          EXPECT_NE(Source.find("clac"), std::string::npos) << Source;
        }
        for (const uint8_t Last : {0xcau, 0xcbu}) {
          const std::vector<uint8_t> Instruction = {0x0f, 0x01, Last};
          EXPECT_NE(std::search(Object.begin(), Object.end(),
                                Instruction.begin(), Instruction.end()),
                    Object.end());
        }
        // Compile both emitted routes at O0/O2 without executing privileged
        // controls on the host. The native SDK object is checked above.
        if (Format == BinaryFormat::ELF)
          compileRoutes(Image, NoOpt, Entry + Offset, "stac");
        if (::testing::Test::HasFailure())
          return;
      }
}

void checkTransactionMemory(bool Abort) {
  const char *Mnemonic = Abort ? "xabort" : "xend";
  const char *Native =
      Abort ? "call void @llvm.x86.xabort(i8" : "call void @llvm.x86.xend()";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        std::vector<uint8_t> Bytes;
        if (A == Arch::X86)
          Bytes = {0x8b, 0x4c, 0x24, 0x04}; // mov ecx, [esp+4]
        else if (Format != BinaryFormat::COFF)
          Bytes = {0x48, 0x89, 0xf9}; // mov rcx, rdi
        Bytes.insert(Bytes.end(), {0xc7, 0x01, 7, 0, 0, 0});
        const auto Offset = Bytes.size();
        const std::vector<uint8_t> Instruction =
            Abort ? std::vector<uint8_t>{0xc6, 0xf8, 0x80}
                  : std::vector<uint8_t>{0x0f, 0x01, 0xd5};
        Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
        Bytes.insert(Bytes.end(), {0x8b, 0x01, 0xc3}); // mov eax, [rcx]; ret
        auto Image = image(A, Format, std::move(Bytes));
        std::string IR;
        sourceRoutes(Image, NoOpt, Entry + Offset, Mnemonic, nullptr, nullptr,
                     {}, {}, &IR);
        if (::testing::Test::HasFailure())
          return;
        const auto Store = IR.find("store i32 7,");
        const auto Boundary = IR.find(Native);
        ASSERT_NE(Boundary, std::string::npos) << IR;
        const auto Load = IR.find("load i32", Boundary);
        ASSERT_NE(Load, std::string::npos) << IR;
        EXPECT_LT(Store, Boundary) << IR;
        EXPECT_LT(Boundary, Load) << IR;
        if (Abort || Format == BinaryFormat::ELF)
          compileRoutes(Image, NoOpt, Entry + Offset, Mnemonic, std::nullopt,
                        false, false, {}, Instruction, 4, Abort);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86SystemStateSource, TransactionCommitRetainsSurroundingMemory) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.invlpgb.addr32"))
    GTEST_SKIP() << "requires the qualified native system SDK";
  checkTransactionMemory(false);
}

TEST(X86SystemStateSource, TransactionAbortRetainsSurroundingMemory) {
  checkTransactionMemory(true);
}

TEST(X86SystemStateSource, TransactionAbortUsesNativeByteAcrossEveryRoute) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  unsigned Cases = 0, NeverCCases = 0;
  for (Arch A : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (uint8_t Code : {0, 127, 128, 255}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          SCOPED_TRACE(Code);
          std::vector<uint8_t> Bytes{0xc6, 0xf8, Code};
          // A dword memory result fixes the observable return width. A bare
          // constant can legitimately be inferred as a full-register value.
          if (A == Arch::X86)
            Bytes.insert(Bytes.end(), {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00});
          else
            Bytes.insert(
                Bytes.end(),
                {0x8b, static_cast<uint8_t>(
                           Format == BinaryFormat::COFF ? 0x01 : 0x07)});
          Bytes.push_back(0xc3);
          auto Image = image(A, Format, std::move(Bytes));
          std::string High, C, IR;
          sourceRoutes(Image, NoOpt, Entry, "xabort", &High, &C, {}, {}, &IR);
          if (::testing::Test::HasFailure())
            return;
          EXPECT_NE(IR.find("call void @llvm.x86.xabort(i8"), std::string::npos)
              << IR;
          EXPECT_EQ(IR.find("asm sideeffect"), std::string::npos) << IR;
          for (const auto &Source : {High, C}) {
            EXPECT_NE(Source.find("_xabort("), std::string::npos) << Source;
            EXPECT_EQ(Source.find("__asm__"), std::string::npos) << Source;
          }
          const std::vector<uint8_t> Instruction{0xc6, 0xf8, Code};
          compileRoutes(Image, NoOpt, Entry, "xabort", std::nullopt, false,
                        false, {}, Instruction, 4, true);
          if (::testing::Test::HasFailure())
            return;
          ++Cases;
          if (A == Arch::X64 && NeverC && llvm::sys::fs::exists(NeverC)) {
            compileRoutes(Image, NoOpt, Entry, "xabort", std::nullopt, false,
                          false, {}, Instruction, 4, true, NeverC);
            if (::testing::Test::HasFailure())
              return;
            ++NeverCCases;
          }
        }
  EXPECT_EQ(Cases, 48u);
  RecordProperty("transaction_abort_fixtures", Cases);
  RecordProperty("transaction_abort_neverc_fixtures", NeverCCases);
}

TEST(X86SystemStateSource, TransactionAbortRequiresConstantTypedEffects) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Mutation);
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.NumInputs = 2;
      Op.Inputs[0] =
          MedVar::makeConst(static_cast<uint16_t>(Intrinsic::Xabort), 2);
      Op.Inputs[1] = MedVar::makeConst(128, 1);
      auto Call =
          HighExpr::makeCall("xabort", 0, {HighExpr::makeConst(128, 1)});
      Call->IntrinsicId = Intrinsic::Xabort;
      Call->Type = NdType::makeVoid();
      switch (Mutation) {
      case 1:
        Op.NumInputs = 1;
        Call->Operands.clear();
        break;
      case 2:
        Op.Inputs[1] = MedVar::makeUnspecified(1, A);
        Call->Operands[0]->Kind = ExprKind::Var;
        break;
      case 3:
        Op.Inputs[1].Size = 4;
        Call->Operands[0]->Type = NdType::makeInt(4, false);
        break;
      case 4:
        Op.Inputs[1].ConstVal = 256;
        Call->Operands[0]->ConstVal = 256;
        break;
      case 5:
        Op.Output.Size = 1;
        Call->Type = NdType::makeInt(1, false);
        break;
      case 6:
        Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        Call->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      case 7:
        Op.MemoryOrdering = NdMemoryOrdering::Acquire;
        Call->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      }
      EXPECT_EQ(x86SystemStateShapeIsValid(Intrinsic::Xabort,
                                           x86SystemStateMedShape(Op, A)),
                Mutation == 0);
      bool HasIntrinsics = false;
      const std::vector<std::string> Arguments(Call->Operands.size(), "128");
      if (Mutation)
        EXPECT_THROW(renderX86SystemStateExpression(A, *Call, Arguments, false,
                                                    HasIntrinsics),
                     std::invalid_argument);
      else {
        EXPECT_EQ(renderX86SystemStateExpression(A, *Call, Arguments, false,
                                                 HasIntrinsics),
                  "_xabort(128)");
        EXPECT_TRUE(HasIntrinsics);
      }
    }
    for (bool LegacyAssembly : {false, true}) {
      SCOPED_TRACE(LegacyAssembly);
      llvm::LLVMContext Context;
      llvm::Module Module("transaction-abort", Context);
      llvm::IRBuilder<> Builder(Context);
      auto *Signature = llvm::FunctionType::get(Builder.getVoidTy(),
                                                {Builder.getInt8Ty()}, false);
      auto *Function =
          llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                                 "transaction_abort", Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Function));
      llvm::CallInst *Call;
      if (LegacyAssembly) {
        auto *Assembly =
            llvm::InlineAsm::get(Signature, "xabort $0", "i,~{memory}", true);
        Call = Builder.CreateCall(Assembly, {Builder.getInt8(128)});
      } else {
        auto *Callee = llvm::Intrinsic::getOrInsertDeclaration(
            &Module, llvm::Intrinsic::lookupIntrinsicID("llvm.x86.xabort"));
        Call = Builder.CreateCall(Callee, {Builder.getInt8(128)});
      }
      Builder.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      const std::vector<std::string> Arguments{"128"};
      auto Render = [&]() {
        return LegacyAssembly
                   ? renderX86SystemStateCall(A, *Call, "", false, Arguments)
                   : renderX86SystemStateNativeCall(A, *Call, "", false,
                                                    Arguments);
      };
      EXPECT_EQ(Render(), "_xabort(128);\n");
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = BinaryFormat::ELF;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
      EXPECT_NE(Source.find("#include <immintrin.h>"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("_xabort(128)"), std::string::npos) << Source;
      EXPECT_NE(Source.find("target(\"rtm\")"), std::string::npos) << Source;
      Call->setArgOperand(0, Function->getArg(0));
      EXPECT_THROW(Render(), std::invalid_argument);
      Call->setArgOperand(0, Builder.getInt8(128));
      for (const auto Effects :
           {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
            llvm::MemoryEffects::writeOnly(), llvm::MemoryEffects::argMemOnly(),
            llvm::MemoryEffects::inaccessibleMemOnly(),
            llvm::MemoryEffects::inaccessibleOrArgMemOnly()}) {
        Call->setMemoryEffects(Effects);
        EXPECT_THROW(Render(), std::invalid_argument);
      }
      Call->setMemoryEffects(llvm::MemoryEffects::unknown());
      Call->addFnAttr(llvm::Attribute::Speculatable);
      EXPECT_THROW(Render(), std::invalid_argument);
      Call->removeFnAttr(llvm::Attribute::Speculatable);
      Call->setDoesNotReturn();
      EXPECT_THROW(Render(), std::invalid_argument);
    }
  }
}

TEST(X86SystemStateSource, TransactionQueryRetainsFlagsAcrossNativeAndCRoutes) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  for (Arch A : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        // Seed OF/SF/PF/AF with signed overflow and CF with STC. Capture
        // XTEST's six arithmetic flags through LAHF/SETO, then return them.
        auto Image =
            image(A, Format,
                  {0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1,    0xf9, 0x0f,
                   0x01, 0xd6, 0x9f, 0x0f, 0x90, 0xc2, 0x0f, 0xb6, 0xc4, 0x0f,
                   0xb6, 0xd2, 0xc1, 0xe2, 8,    0x09, 0xd0, 0xc3});
        const std::vector<uint8_t> Instruction{0x0f, 0x01, 0xd6};
        compileRoutes(Image, NoOpt, Entry + 9, "xtest", std::nullopt, false,
                      false, {}, Instruction, 4, true);
        if (::testing::Test::HasFailure())
          return;
        if (A == Arch::X64 && NeverC && llvm::sys::fs::exists(NeverC)) {
          compileRoutes(Image, NoOpt, Entry + 9, "xtest", std::nullopt, false,
                        false, {}, Instruction, 4, true, NeverC);
          if (::testing::Test::HasFailure())
            return;
        }
      }
}

TEST(X86SystemStateSource, CacheZeroKeepsMemoryEffectsAndTheImplicitAddress) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const bool NoOpt : {false, true}) {
        std::vector<uint8_t> Bytes;
        if (A == Arch::X86)
          Bytes = {0x8b, 0x4c, 0x24, 0x04}; // mov ecx, [esp+4]
        else if (Format != BinaryFormat::COFF)
          Bytes = {0x48, 0x89, 0xf9}; // mov rcx, rdi
        if (A == Arch::X64)
          Bytes.push_back(0x48);
        Bytes.insert(Bytes.end(), {0x89, 0xc8, // mov rax/eax, rcx/ecx
                                   0xc7, 0x01, 7, 0, 0, 0});
        const auto Offset = Bytes.size();
        Bytes.insert(Bytes.end(), {0x0f, 0x01, 0xfc, // clzero
                                   0x8b, 0x01, 0xc3});
        auto Image = image(A, Format, std::move(Bytes));
        std::string High, LLVM, IR;
        sourceRoutes(Image, NoOpt, Entry + Offset, "clzero", &High, &LLVM, {},
                     {}, &IR);
        if (::testing::Test::HasFailure())
          return;
        const auto Store = IR.find("store i32 7,");
        // Older SDKs may know the stock intrinsic without the native system
        // contract capability. Either checked representation must stay ordered.
        auto Clear = IR.find("call void @llvm.x86.clzero(ptr ");
        if (Clear == std::string::npos)
          Clear = IR.find("call void asm sideeffect \"clzero\", "
                          "\"{ax},~{memory}\"(ptr ");
        const auto Load = IR.find("load i32");
        ASSERT_NE(Clear, std::string::npos) << IR;
        ASSERT_NE(Load, std::string::npos) << IR;
        EXPECT_LT(Store, Clear) << IR;
        EXPECT_LT(Clear, Load) << IR;
        for (const auto &Source : {High, LLVM}) {
          EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
          EXPECT_EQ(Source.find("memset("), std::string::npos) << Source;
        }
        if (Format == BinaryFormat::ELF)
          compileRoutes(Image, NoOpt, Entry + Offset, "clzero");
        if (::testing::Test::HasFailure())
          return;
      }
}

namespace {
std::string cacheZeroDriver(const BinaryImage &Image, bool Address32,
                            bool Dead) {
  auto Driver = faultDriver(Image);
  for (const auto Name : {"virtualization_test", "neverd_original"}) {
    const auto Declaration = "extern void " + std::string(Name);
    Driver.replace(Driver.find(Declaration), Declaration.size(),
                   "extern unsigned long long " + std::string(Name));
  }
  const std::string Pointer = "void (*fn)(void)";
  Driver.replace(Driver.find(Pointer), Pointer.size(),
                 "unsigned long long (*fn)(void)");
  const std::string Main = "int main(void)";
  Driver.replace(Driver.find(Main), Main.size(),
                 "static int compare_faults(void)");
  Driver = "#define _GNU_SOURCE\n#include <stdint.h>\n#include <ucontext.h>\n" +
           Driver;
  llvm::raw_string_ostream OS(Driver);
  OS << "\nstatic const uint64_t mask = "
     << (Address32 ? "UINT64_C(0xffffffff)" : "UINT64_MAX") << ";\n"
     << "static const uint64_t expected = "
     << (Dead ? "0" : "UINT64_C(0x11223344ffffedab)") << ";\n";
  // First compare real faults. The controlled completion below checks the
  // instruction's actual RAX input and preserved return value; it does not
  // stand in for native cache-line writes on a host without CLZERO support.
  OS << R"C(
static volatile sig_atomic_t events;
static void complete(int signal, siginfo_t *info, void *context) {
  ucontext_t *uc = context;
  greg_t *g = uc->uc_mcontext.gregs;
  const unsigned char *ip = (const unsigned char *)(uintptr_t)g[REG_RIP];
  unsigned prefix = 0;
  (void)signal; (void)info;
  while (prefix < 2 && (ip[prefix] == 0x64 || ip[prefix] == 0x65 ||
                        ip[prefix] == 0x67)) ++prefix;
  if (ip[prefix] != 0x0f || ip[prefix + 1] != 0x01 ||
      ip[prefix + 2] != 0xfc) _exit(21);
  if (((uint64_t)g[REG_RAX] & mask) != (UINT64_C(0x11223344ffffedab) & mask))
    _exit(22);
  g[REG_RIP] += prefix + 3;
  ++events;
}
int main(void) {
  if (compare_faults()) return 10;
  struct sigaction action = {0};
  action.sa_sigaction = complete;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, 0) || sigaction(SIGSEGV, &action, 0) ||
      sigaction(SIGBUS, &action, 0)) return 11;
  events = 0;
  if (neverd_original() != expected || events != 1) return 12;
  events = 0;
  if (virtualization_test() != expected || events != 1) return 13;
  return 0;
}
)C";
  return Driver;
}
} // namespace

TEST(X86SystemStateSource, CacheZeroFaultsAndAddressTransportMatchRawBytes) {
  for (const auto &E : cacheZeroEncodings())
    for (const bool NoOpt : {false, true})
      for (const bool Dead : {false, true}) {
        std::vector<uint8_t> Bytes = {0x48, 0xb8, 0xab, 0xed, 0xff,
                                      0xff, 0x44, 0x33, 0x22, 0x11};
        const auto Offset = Bytes.size();
        Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
        if (Dead)
          Bytes.insert(Bytes.end(), {0x31, 0xc0});
        Bytes.push_back(0xc3);
        auto Image = image(Arch::X64, BinaryFormat::ELF, std::move(Bytes));
        const bool Address32 =
            std::find(E.Bytes.begin(), E.Bytes.end(), 0x67) != E.Bytes.end();
        compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                      false, false, cacheZeroDriver(Image, Address32, Dead),
                      {0x0f, 0x01, 0xfc}, 8);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86SystemStateSource,
     StockSystemBuiltinsCompileWithoutCallerFeatureFlags) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-rtm-native-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (const bool Zero : {false, true})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const bool NoOpt : {false, true}) {
        const std::string Mnemonic = Zero ? "clzero" : "xend";
        auto Image = image(Arch::X64, Format,
                           {0xb8, 0x11, 0, 0, 0, 0x0f, 0x01,
                            static_cast<uint8_t>(Zero ? 0xfc : 0xd5), 0xc3});
        std::string High, LLVM;
        sourceRoutes(Image, NoOpt, Entry + 5, Mnemonic, &High, &LLVM);
        if (::testing::Test::HasFailure())
          return;
        for (const auto &Source : {High, LLVM}) {
          // Stock Clang owns these builtins too. Select the native branch,
          // without claiming support for the fork's other system builtins.
          ASSERT_NE(
              Source.find(Zero ? "target(\"clzero\")" : "target(\"rtm\")"),
              std::string::npos)
              << Source;
          const auto Native =
              "#define NEVERD_X86_SYSTEM_STATE(native, ...) native\n" + Source;
          compileNativeC(Compiler, Directory, Format, Native,
                         "llvm.x86." + Mnemonic, Cases, std::nullopt, true,
                         false);
          if (::testing::Test::HasFailure())
            return;
        }
      }
  EXPECT_EQ(Cases, 48u);
  RecordProperty("stock_system_native_c_compilations", Cases);
}

TEST(X86SystemStateSource, NeverCCompilesBothRoutesToNativeSystemCalls) {
  const char *Configured = std::getenv("NEVERD_TEST_NEVERC");
  if (!Configured || !llvm::sys::fs::exists(Configured))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  const std::string Compiler = Configured;
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-native-system-c",
                                                    Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (const auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (const bool NoOpt : {false, true})
      for (const auto &E : systemEncodings(Arch::X64)) {
        const auto Name = nativeSystemName(E, Arch::X64);
        if (Name.empty())
          continue;
        SCOPED_TRACE(Name);
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        size_t Offset;
        auto Image = image(Arch::X64, Format,
                           nativeSystemFunction(Arch::X64, E, Offset));
        std::string High, LLVM;
        sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, &High, &LLVM);
        if (::testing::Test::HasFailure())
          return;
        for (const auto &Source : {High, LLVM}) {
          compileNativeC(Compiler, Directory, Format, Source, Name, Cases);
          if (::testing::Test::HasFailure())
            return;
        }
      }
  EXPECT_EQ(Cases, 792u);
  RecordProperty("native_system_c_compilations", Cases);
}

TEST(X86SystemStateSource, NativeCallsCannotLoseModeResultsOrEffects) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.invlpgb.addr32"))
    GTEST_SKIP() << "requires the qualified native system SDK";
  for (const char *Name : {"llvm.x86.clac",
                           "llvm.x86.stac",
                           "llvm.x86.xend",
                           "llvm.x86.clzero",
                           "llvm.x86.clts",
                           "llvm.x86.wrmsrns",
                           "llvm.x86.rdmsrlist",
                           "llvm.x86.wrmsrlist",
                           "llvm.x86.lkgs.reg",
                           "llvm.x86.lkgs.mem",
                           "llvm.x86.invlpgb",
                           "llvm.x86.invlpgb32",
                           "llvm.x86.invlpgb.addr32",
                           "llvm.x86.tlbsync",
                           "llvm.x86.lar.reg16",
                           "llvm.x86.lar.reg32",
                           "llvm.x86.lar.reg64",
                           "llvm.x86.lar.mem16",
                           "llvm.x86.lar.mem32",
                           "llvm.x86.lar.mem64",
                           "llvm.x86.lsl.reg16",
                           "llvm.x86.lsl.reg32",
                           "llvm.x86.lsl.reg64",
                           "llvm.x86.lsl.mem16",
                           "llvm.x86.lsl.mem32",
                           "llvm.x86.lsl.mem64",
                           "llvm.x86.lkgs.address",
                           "llvm.x86.lar.mem16.address",
                           "llvm.x86.lar.mem32.address",
                           "llvm.x86.lar.mem64.address",
                           "llvm.x86.lsl.mem16.address",
                           "llvm.x86.lsl.mem32.address",
                           "llvm.x86.lsl.mem64.address",
                           "llvm.x86.lar.mem16.address32",
                           "llvm.x86.lar.mem32.address32",
                           "llvm.x86.lsl.mem16.address32",
                           "llvm.x86.lsl.mem32.address32"}) {
    SCOPED_TRACE(Name);
    llvm::LLVMContext Context;
    llvm::Module Module("native-system-contract", Context);
    const auto IID = llvm::Intrinsic::lookupIntrinsicID(Name);
    ASSERT_NE(IID, llvm::Intrinsic::not_intrinsic);
    auto *Callee = llvm::Intrinsic::getOrInsertDeclaration(&Module, IID);
    auto *Function = llvm::Function::Create(Callee->getFunctionType(),
                                            llvm::GlobalValue::ExternalLinkage,
                                            "native_system", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    std::vector<llvm::Value *> Inputs;
    for (auto &Argument : Function->args())
      Inputs.push_back(&Argument);
    const bool Address = llvm::StringRef(Name).contains(".address");
    const Arch A = llvm::StringRef(Name) == "llvm.x86.invlpgb32" ||
                           llvm::StringRef(Name).ends_with(".address32")
                       ? Arch::X86
                       : Arch::X64;
    if (Address) {
      Inputs[1] = Builder.getInt32(A == Arch::X64 ? 32 : 16);
      Inputs[2] = Builder.getInt32(0x65);
    }
    auto *Call = Builder.CreateCall(Callee, Inputs);
    if (Call->getType()->isVoidTy())
      Builder.CreateRetVoid();
    else
      Builder.CreateRet(Call);
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    const bool Live = !Call->getType()->isVoidTy();
    const std::vector<std::string> Arguments(Inputs.size(), "input");
    EXPECT_TRUE(
        renderX86SystemStateNativeCall(A, *Call, "result", Live, Arguments));
    if (Address) {
      for (const auto Bits : {0u, 8u, 24u, 128u, A == Arch::X64 ? 16u : 64u}) {
        Call->setArgOperand(1, Builder.getInt32(Bits));
        EXPECT_THROW((void)renderX86SystemStateNativeCall(A, *Call, "result",
                                                          Live, Arguments),
                     std::invalid_argument);
      }
      Call->setArgOperand(1, Inputs[1]);
      for (const auto Segment : {1u, 0x66u, 0x67u, 0xffffffffu}) {
        Call->setArgOperand(2, Builder.getInt32(Segment));
        EXPECT_THROW((void)renderX86SystemStateNativeCall(A, *Call, "result",
                                                          Live, Arguments),
                     std::invalid_argument);
      }
      Call->setArgOperand(2, Inputs[2]);
      for (unsigned Index : {1u, 2u}) {
        Call->setArgOperand(Index, Function->getArg(Index));
        EXPECT_THROW((void)renderX86SystemStateNativeCall(A, *Call, "result",
                                                          Live, Arguments),
                     std::invalid_argument);
        Call->setArgOperand(Index, Inputs[Index]);
      }
      for (const auto Bits :
           {A == Arch::X64 ? 32u : 16u, A == Arch::X64 ? 64u : 32u})
        for (const auto Segment :
             {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u}) {
          Call->setArgOperand(1, Builder.getInt32(Bits));
          Call->setArgOperand(2, Builder.getInt32(Segment));
          EXPECT_TRUE(renderX86SystemStateNativeCall(A, *Call, "result", Live,
                                                     Arguments));
        }
      Call->setArgOperand(1, Inputs[1]);
      Call->setArgOperand(2, Inputs[2]);
    }
    for (const auto Effects :
         {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
          llvm::MemoryEffects::writeOnly()}) {
      Call->setMemoryEffects(Effects);
      EXPECT_THROW((void)renderX86SystemStateNativeCall(A, *Call, "result",
                                                        Live, Arguments),
                   std::invalid_argument);
    }
    Call->setMemoryEffects(llvm::MemoryEffects::unknown());
    Call->setDoesNotReturn();
    EXPECT_THROW((void)renderX86SystemStateNativeCall(A, *Call, "result", Live,
                                                      Arguments),
                 std::invalid_argument);
    Call->removeFnAttr(llvm::Attribute::NoReturn);
    if (Live)
      EXPECT_THROW(
          (void)renderX86SystemStateNativeCall(A, *Call, "", true, Arguments),
          std::invalid_argument);
    if (Address || llvm::StringRef(Name).starts_with("llvm.x86.invlpgb"))
      EXPECT_THROW((void)renderX86SystemStateNativeCall(
                       A == Arch::X86 ? Arch::X64 : Arch::X86, *Call, "result",
                       Live, Arguments),
                   std::invalid_argument);
  }
}

namespace {
void checkDescriptorAddressControls(bool State) {
  if (!llvm::Intrinsic::lookupIntrinsicID(State ? "llvm.x86.verr.mem.address"
                                                : "llvm.x86.lar.mem16.address"))
    GTEST_SKIP() << "requires the explicit descriptor-address SDK";
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
      "neverd-descriptor-address-c", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned NativeFunctions = 0, CCompilations = 0, NeverCCompilations = 0;
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      llvm::LLVMContext Context;
      llvm::Module Module("descriptor-address-controls", Context);
      const unsigned Word = A == Arch::X64 ? 64 : 32;
      const std::string Triple =
          std::string(A == Arch::X64 ? "x86_64" : "i386") +
          (Format == BinaryFormat::COFF    ? "-pc-windows-msvc"
           : Format == BinaryFormat::MachO ? "-apple-darwin"
                                           : "-linux-gnu");
      Module.setTargetTriple(llvm::Triple(Triple));
      Module.setDataLayout(A == Arch::X64 ? "e-p:64:64-i64:64-i128:128"
                                          : "e-p:32:32-i64:64");
      std::vector<std::pair<Intrinsic, uint16_t>> Forms;
      if (State) {
        for (const auto Id : X86DescriptorStateInventory)
          Forms.emplace_back(Id, isX86DescriptorVerify(Id) ? 1 : 0);
      } else {
        for (const auto Id : {Intrinsic::X86LoadAccessRightsMemory,
                              Intrinsic::X86LoadSegmentLimitMemory})
          for (uint16_t Width : {2, 4, 8})
            Forms.emplace_back(Id, Width);
        Forms.emplace_back(Intrinsic::X86LoadKernelGsMemory, 0);
      }
      for (const auto &[Id, Width] : Forms)
        for (const unsigned Bits : {Word / 2, Word})
          for (const unsigned Segment :
               {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u}) {
            std::optional<std::string> Name;
            if (State) {
              if (const auto P = x86DescriptorStateNative(
                      Id, A, true, Width, NdMemoryAddressSpace::Default,
                      X86DescriptorAddress{Bits, Segment}))
                Name = P->LLVMName;
            } else if (const auto P = x86SystemStateNative(
                           Id, A, Width, NdMemoryAddressSpace::Default,
                           X86SystemStateNativeAddress{Bits, Segment})) {
              Name = P->LLVMName;
            }
            if (!Name)
              continue;
            const auto IID = llvm::Intrinsic::lookupIntrinsicID(*Name);
            ASSERT_NE(IID, llvm::Intrinsic::not_intrinsic);
            auto *Callee =
                llvm::Intrinsic::getOrInsertDeclaration(&Module, IID);
            const auto FunctionName =
                "query_" + std::to_string(NativeFunctions++);
            auto *Function = llvm::Function::Create(
                Callee->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                FunctionName, Module);
            Function->getArg(0)->setName("neverd_descriptor");
            llvm::IRBuilder<> B(
                llvm::BasicBlock::Create(Context, "entry", Function));
            std::vector<llvm::Value *> Inputs;
            for (auto &Argument : Function->args())
              Inputs.push_back(&Argument);
            Inputs[1] = B.getInt32(Bits);
            Inputs[2] = B.getInt32(Segment);
            auto *Call = B.CreateCall(Callee, Inputs);
            if (Call->getType()->isVoidTy())
              B.CreateRetVoid();
            else
              B.CreateRet(Call);
          }
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      auto Native = llvm::CloneModule(Module);
      const auto Object = Codegen().compile(*Native, A, Format);
      ASSERT_TRUE(Object.Success);
      ASSERT_FALSE(Object.ObjectData.empty());
      CEmitterOptions Options;
      Options.TheArch = A;
      Options.Format = Format;
      Options.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
      llvm::SmallString<128> Input(Directory), Output(Directory),
          Errors(Directory);
      llvm::sys::path::append(Input, "fallback.c");
      llvm::sys::path::append(Output, "fallback.o");
      llvm::sys::path::append(Errors, "fallback.err");
      std::error_code Error;
      llvm::raw_fd_ostream File(Input, Error);
      ASSERT_FALSE(Error);
      File << "#define NEVERD_X86_SYSTEM_STATE(native, ...) __VA_ARGS__\n"
              "#define NEVERD_X86_DESCRIPTOR_STATE(native, msvc, ...) "
              "__VA_ARGS__\n"
           << freestandingC(Source);
      File.close();
      const std::string Target = "--target=" + Triple;
      for (const char *Opt : {"-O0", "-O2"}) {
        SCOPED_TRACE(Triple);
        SCOPED_TRACE(Opt);
        const std::vector<llvm::StringRef> Arguments = {Compiler,
                                                        Target,
                                                        "-std=gnu23",
                                                        "-ffreestanding",
                                                        "-c",
                                                        Opt,
                                                        "-Werror=uninitialized",
                                                        Input,
                                                        "-o",
                                                        Output};
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, std::nullopt, Errors.str()};
        const auto Status = llvm::sys::ExecuteAndWait(
            Compiler, Arguments, std::nullopt, Redirects, 30);
        const auto Diagnostics = llvm::MemoryBuffer::getFile(Errors);
        ASSERT_EQ(Status, 0)
            << (Diagnostics ? (*Diagnostics)->getBuffer().str() : "") << Source;
        ++CCompilations;
      }
      const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
      if (A == Arch::X64 && NeverC && llvm::sys::fs::exists(NeverC))
        compileNativeC(NeverC, Directory, Format, Source,
                       State ? "llvm.x86.verr.mem.address"
                             : "llvm.x86.lar.mem16.address",
                       NeverCCompilations);
      if (::testing::Test::HasFailure())
        return;
    }
  EXPECT_EQ(NativeFunctions, State ? 1260u : 462u);
  EXPECT_EQ(CCompilations, 12u);
  testing::Test::RecordProperty("descriptor_address_native_functions",
                                NativeFunctions);
  testing::Test::RecordProperty("descriptor_address_c_compilations",
                                CCompilations);
  testing::Test::RecordProperty("descriptor_address_neverc_compilations",
                                NeverCCompilations);
}
} // namespace

TEST(X86SystemStateSource,
     NativeDescriptorAddressControlsCompileAcrossFormats) {
  checkDescriptorAddressControls(false);
}

TEST(X86DescriptorStateSource, NativeAddressControlsCompileAcrossFormats) {
  checkDescriptorAddressControls(true);
}

TEST(X86SystemStateSource, EveryOwnedInstructionSurvivesBothSourceRoutes) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const auto &E : systemEncodings(A)) {
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Image = image(A, Format, function(A, E.Bytes, Offset));
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic);
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86SystemStateSource, GeneratedAssemblyCompilesForBothWordSizesAtO0AndO2) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : systemEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
      compileRoutes(Image, false, Entry + Offset, E.Mnemonic);
      if (::testing::Test::HasFailure())
        return;
    }
}

namespace {
std::vector<uint8_t> implicitMsrFunction(const SystemEncoding &E, bool Dead,
                                         bool EmptyMask, size_t &Offset) {
  std::vector<uint8_t> Bytes;
  auto Move = [&](uint8_t Opcode, uint64_t Value) {
    Bytes.insert(Bytes.end(), {0x48, Opcode});
    for (unsigned I = 0; I < 8; ++I)
      Bytes.push_back(Value >> (8 * I));
  };
  if (E.Id == Intrinsic::X86WriteMsrNonSerializing) {
    Move(0xb8, UINT64_C(0x1122334455667788));
    Move(0xb9, UINT64_C(0x88776655ffffffff));
    Move(0xba, UINT64_C(0xfedcba98cafebabe));
  } else {
    // Invalid full-width addresses expose accidental preloads and truncation.
    Move(0xbe, UINT64_C(0x1122334455667788));
    Move(0xbf, UINT64_C(0x8877665544332211));
    Move(0xb9, EmptyMask ? 0 : UINT64_C(0x8000000000000003));
  }
  Offset = Bytes.size();
  Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
  if (Dead)
    Bytes.insert(Bytes.end(), {0x31, 0xc0});
  else if (E.Id == Intrinsic::X86WriteMsrNonSerializing)
    Bytes.insert(Bytes.end(), {0x48, 0x31, 0xd0, 0x48, 0x31, 0xc8});
  else
    Bytes.insert(Bytes.end(), {0x48, 0x89, 0xc8});
  Bytes.push_back(0xc3);
  return Bytes;
}

std::string implicitMsrDriver(const BinaryImage &Image, const SystemEncoding &E,
                              bool Dead, bool EmptyMask) {
  std::string Driver = faultDriver(Image);
  for (const auto Name : {"virtualization_test", "neverd_original"}) {
    const auto Declaration = "extern void " + std::string(Name);
    Driver.replace(Driver.find(Declaration), Declaration.size(),
                   "extern unsigned long long " + std::string(Name));
  }
  const std::string FunctionPointer = "void (*fn)(void)";
  Driver.replace(Driver.find(FunctionPointer), FunctionPointer.size(),
                 "unsigned long long (*fn)(void)");
  const std::string Main = "int main(void)";
  Driver.replace(Driver.find(Main), Main.size(),
                 "static int compare_faults(void)");
  Driver = "#define _GNU_SOURCE\n#include <stdint.h>\n#include <ucontext.h>\n" +
           Driver;
  llvm::raw_string_ostream OS(Driver);
  OS << "\nstatic const unsigned char instruction[] = {";
  for (auto Byte : E.Bytes)
    OS << unsigned(Byte) << ',';
  OS << "};\nstatic const int list = "
     << (E.Id != Intrinsic::X86WriteMsrNonSerializing)
     << ";\nstatic const int dead = " << Dead
     << ";\nstatic const uint64_t initial_mask = "
     << (EmptyMask ? "0" : "UINT64_C(0x8000000000000003)") << ";\n";
  // The real fault comparison is independent of this completion oracle. The
  // oracle checks register transport and sigreturn at the same instruction,
  // not successful privileged accesses or a software model of the MSR list.
  OS << R"C(
static volatile sig_atomic_t events, restart;
static uintptr_t restart_ip;
static void complete(int signal, siginfo_t *info, void *context) {
  ucontext_t *uc = context;
  greg_t *g = uc->uc_mcontext.gregs;
  const unsigned char *ip = (const unsigned char *)(uintptr_t)g[REG_RIP];
  (void)signal; (void)info;
  for (unsigned i = 0; i < sizeof(instruction); ++i)
    if (ip[i] != instruction[i]) _exit(21);
  if (list) {
    uint64_t mask = events ? initial_mask & ~UINT64_C(1) : initial_mask;
    if ((uint64_t)g[REG_RSI] != UINT64_C(0x1122334455667788) ||
        (uint64_t)g[REG_RDI] != UINT64_C(0x8877665544332211) ||
        (uint64_t)g[REG_RCX] != mask) _exit(22);
    if (restart && !events) {
      restart_ip = (uintptr_t)ip;
      g[REG_RCX] = (greg_t)(mask & ~UINT64_C(1));
      ++events;
      return;
    }
    if (restart && restart_ip != (uintptr_t)ip) _exit(23);
    // A distinctive completion detects replacing the returned RCX with zero.
    g[REG_RCX] = restart ? 0 : (greg_t)UINT64_C(0xfedcba9876543210);
  } else if ((uint32_t)g[REG_RAX] != UINT32_C(0x55667788) ||
             (uint32_t)g[REG_RDX] != UINT32_C(0xcafebabe) ||
             (uint32_t)g[REG_RCX] != UINT32_MAX) {
    _exit(24);
  }
  g[REG_RIP] += sizeof(instruction);
  ++events;
}
int main(void) {
  if (compare_faults()) return 10;
  struct sigaction action = {0};
  action.sa_sigaction = complete;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, 0) || sigaction(SIGSEGV, &action, 0)) return 11;
  for (restart = 0; restart <= (list && initial_mask != 0); ++restart) {
    uint64_t expected = dead ? 0 : list ? (restart ? 0 : UINT64_C(0xfedcba9876543210)) :
      UINT64_C(0x1122334455667788) ^ UINT64_C(0x88776655ffffffff) ^ UINT64_C(0xfedcba98cafebabe);
    unsigned count = restart ? 2 : 1;
    events = 0;
    if (neverd_original() != expected || events != count) return 12;
    events = 0;
    if (virtualization_test() != expected || events != count) return 13;
  }
  return 0;
}
)C";
  return Driver;
}
} // namespace

TEST(X86ImplicitMsrSource, FaultsRegisterTransportAndRestartMatchEveryRoute) {
  for (const auto &E : systemEncodings(Arch::X64)) {
    const auto Kind = x86SystemStateContract(E.Id).Kind;
    if (Kind != X86SystemStateKind::MsrList &&
        Kind != X86SystemStateKind::WriteMsr)
      continue;
    for (bool NoOpt : {false, true})
      for (bool Dead : {false, true})
        for (bool EmptyMask : {false, true}) {
          if (EmptyMask && (Kind == X86SystemStateKind::WriteMsr || Dead))
            continue;
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(NoOpt);
          SCOPED_TRACE(Dead);
          SCOPED_TRACE(EmptyMask);
          size_t Offset;
          auto Image = image(Arch::X64, BinaryFormat::ELF,
                             implicitMsrFunction(E, Dead, EmptyMask, Offset));
          compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                        false, false,
                        implicitMsrDriver(Image, E, Dead, EmptyMask), E.Bytes,
                        Dead ? 0 : 8);
          if (::testing::Test::HasFailure())
            return;
        }
  }
}

TEST(X86ImplicitMsrSource, NeverCRetainsNativeCompletionAndRestart) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  for (const auto &E : systemEncodings(Arch::X64)) {
    const auto Kind = x86SystemStateContract(E.Id).Kind;
    if (Kind != X86SystemStateKind::MsrList &&
        Kind != X86SystemStateKind::WriteMsr)
      continue;
    for (const bool NoOpt : {false, true})
      for (const bool Dead : {false, true})
        for (const bool EmptyMask : {false, true}) {
          if (EmptyMask && (Kind == X86SystemStateKind::WriteMsr || Dead))
            continue;
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(NoOpt);
          SCOPED_TRACE(Dead);
          SCOPED_TRACE(EmptyMask);
          size_t Offset;
          auto Image = image(Arch::X64, BinaryFormat::ELF,
                             implicitMsrFunction(E, Dead, EmptyMask, Offset));
          compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                        false, false,
                        implicitMsrDriver(Image, E, Dead, EmptyMask), E.Bytes,
                        Dead ? 0 : 8, false, Compiler);
          if (::testing::Test::HasFailure())
            return;
        }
  }
}

TEST(X86ImplicitMsrSource, CompletionSurvivesEveryNativeObjectFormat) {
  for (const auto &E : systemEncodings(Arch::X64)) {
    const auto Kind = x86SystemStateContract(E.Id).Kind;
    if (Kind != X86SystemStateKind::MsrList &&
        Kind != X86SystemStateKind::WriteMsr)
      continue;
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (bool Dead : {false, true}) {
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          SCOPED_TRACE(Dead);
          size_t Offset;
          auto Image = image(Arch::X64, Format,
                             implicitMsrFunction(E, Dead, false, Offset));
          std::vector<uint8_t> Object;
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, nullptr,
                       nullptr, {}, {}, nullptr, &Object, Dead ? 0 : 8);
          EXPECT_NE(std::search(Object.begin(), Object.end(), E.Bytes.begin(),
                                E.Bytes.end()),
                    Object.end());
          if (::testing::Test::HasFailure())
            return;
        }
  }
}

TEST(X86ImplicitMsrSource, MalformedNativeContractsAreRejected) {
  for (auto Id : {Intrinsic::X86WriteMsrNonSerializing,
                  Intrinsic::X86ReadMsrList, Intrinsic::X86WriteMsrList})
    for (auto A : {Arch::X86, Arch::X64})
      for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
        const bool List = Id != Intrinsic::X86WriteMsrNonSerializing;
        SCOPED_TRACE(static_cast<unsigned>(Id));
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(Mutation);
        const auto P = x86SystemStateAsm(Id, Arch::X64, List ? 8 : 0,
                                         NdMemoryAddressSpace::Default);
        ASSERT_TRUE(P);
        llvm::LLVMContext Context;
        llvm::Module Module("implicit-msr-contract", Context);
        Module.setDataLayout(A == Arch::X64 ? "e-p:64:64" : "e-p:32:32");
        llvm::IRBuilder<> B(Context);
        std::vector<llvm::Type *> Types;
        for (unsigned I = 0; I < P->InputBytes.size(); ++I)
          Types.push_back(List && I < 2
                              ? static_cast<llvm::Type *>(B.getPtrTy())
                              : B.getIntNTy(8 * P->InputBytes[I]));
        if (Mutation == 4)
          Types[0] = B.getInt16Ty();
        if (Mutation == 11 && List)
          Types[1] = B.getPtrTy(256);
        llvm::Type *Return = List ? B.getInt64Ty() : B.getVoidTy();
        if (Mutation == 9)
          Return = List ? B.getInt32Ty() : B.getInt64Ty();
        auto *Signature = llvm::FunctionType::get(Return, Types, false);
        auto *Function = llvm::Function::Create(
            Signature, llvm::GlobalValue::ExternalLinkage, "effect", Module);
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
        std::vector<llvm::Value *> Values;
        for (auto &Arg : Function->args())
          Values.push_back(&Arg);
        std::string Constraints = P->Constraints;
        // Keep the malformed owned contract valid LLVM assembly so the
        // emitter, rather than InlineAsm's constructor, rejects the result.
        if (Mutation == 9 && !List)
          Constraints = "=r," + Constraints;
        if (Mutation == 1)
          Constraints.erase(Constraints.find(",~{memory}"));
        if (Mutation == 10)
          Constraints.replace(Constraints.find("{cx}"), 4, "{ax}");
        auto *Asm = llvm::InlineAsm::get(
            Signature, P->Text + (Mutation == 3 ? "\n\tnop" : ""), Constraints,
            Mutation != 2, Mutation == 12,
            Mutation == 7 ? llvm::InlineAsm::AD_Intel
                          : llvm::InlineAsm::AD_ATT);
        auto *Call = B.CreateCall(Asm, Values);
        if (Mutation == 5)
          Call->setOnlyReadsMemory();
        if (Mutation == 6)
          Call->setOnlyWritesMemory();
        if (Mutation == 8)
          Call->setDoesNotReturn();
        if (Return->isVoidTy())
          B.CreateRetVoid();
        else
          B.CreateRet(Call);
        CEmitterOptions Options;
        Options.TheArch = A;
        Options.EmitIncludes = false;
        std::string Text;
        llvm::raw_string_ostream OS(Text);
        if ((Mutation == 0 || (Mutation == 11 && !List)) &&
            !(A == Arch::X86 && List))
          EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Text;
        else
          EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                       std::invalid_argument);
      }
}

TEST(X86SystemMemorySource, BothSourceRoutesRetainEffectsAcrossAllFormats) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const auto &E : systemMemoryEncodings(A)) {
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Image = image(A, Format, function(A, E.Bytes, Offset));
          std::string High, LLVM;
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, &High, &LLVM);
          const bool Segmented =
              E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65;
          for (const auto *Source : {&High, &LLVM}) {
            if (Format == BinaryFormat::COFF && Source == &High && !Segmented &&
                (E.Id == Intrinsic::CetWrss || E.Id == Intrinsic::CetWruss)) {
              EXPECT_NE(Source->find("_" + std::string(E.Mnemonic) + "("),
                        std::string::npos)
                  << *Source;
              EXPECT_NE(Source->find("#include <intrin.h>"), std::string::npos);
              continue;
            }
            EXPECT_NE(Source->find("__asm__ volatile"), std::string::npos);
            EXPECT_NE(Source->find("\"memory\""), std::string::npos);
            if (Segmented)
              EXPECT_NE(
                  Source->find(E.Bytes.front() == 0x64 ? "%%fs:" : "%%gs:"),
                  std::string::npos)
                  << *Source;
          }
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86SystemMemorySource, GeneratedInstructionsCompileAtO0AndO2) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : systemMemoryEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
      compileRoutes(Image, false, Entry + Offset, E.Mnemonic);
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86SystemMemorySource, WindowsShadowWritesCompileWithMsvcIntrinsics) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-cet-coff", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  llvm::SmallString<128> Input(Directory), Object(Directory),
      ErrorFile(Directory);
  llvm::sys::path::append(Input, "shadow.c");
  llvm::sys::path::append(Object, "shadow.obj");
  llvm::sys::path::append(ErrorFile, "clang.err");
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : systemMemoryEncodings(A)) {
      if ((E.Id != Intrinsic::CetWrss && E.Id != Intrinsic::CetWruss) ||
          E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65)
        continue;
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      size_t Offset;
      auto Image = image(A, BinaryFormat::COFF, function(A, E.Bytes, Offset));
      std::string High;
      sourceRoutes(Image, false, Entry + Offset, E.Mnemonic, &High);
      std::error_code Error;
      llvm::raw_fd_ostream OS(Input, Error);
      ASSERT_FALSE(Error);
      OS << freestandingC(High);
      OS.close();
      ASSERT_FALSE(OS.has_error());
      for (const char *Optimization : {"-O0", "-O2"}) {
        const std::vector<llvm::StringRef> Arguments = {
            Compiler,
            A == Arch::X64 ? "--target=x86_64-pc-windows-msvc"
                           : "--target=i686-pc-windows-msvc",
            "-ffreestanding",
            "-fno-strict-aliasing",
            "-mshstk",
            "-Werror=implicit-function-declaration",
            Optimization,
            "-c",
            Input,
            "-o",
            Object};
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, std::nullopt, ErrorFile.str()};
        const int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                     std::nullopt, Redirects);
        const auto Diagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
        ASSERT_EQ(Status, 0)
            << (Diagnostics ? (*Diagnostics)->getBuffer().str() : "") << High;
      }
    }
}

TEST(X86SystemMemorySource, FaultsMatchOriginalInstructionsAcrossEveryRoute) {
  // Feature/privilege/enablement checks must precede ordinary operand faults.
  // 0x8000000000000000 is noncanonical for both 48- and 57-bit addressing.
  for (const auto &E : std::vector<SystemEncoding>{
           {{0x0f, 0x38, 0xf6, 0x08}, Intrinsic::CetWrss, "wrssd"},
           {{0x48, 0x0f, 0x38, 0xf6, 0x08}, Intrinsic::CetWrss, "wrssq"},
           {{0x66, 0x0f, 0x38, 0xf5, 0x08}, Intrinsic::CetWruss, "wrussd"},
           {{0x66, 0x48, 0x0f, 0x38, 0xf5, 0x08},
            Intrinsic::CetWruss,
            "wrussq"},
           {{0xf2, 0x0f, 0x38, 0xf8, 0x08}, Intrinsic::Enqcmd, "enqcmd"},
           {{0xf3, 0x0f, 0x38, 0xf8, 0x08}, Intrinsic::Enqcmds, "enqcmds"},
           {{0x0f, 0x38, 0xf9, 0x08}, Intrinsic::X86DirectStore, "movdiri"},
           {{0x48, 0x0f, 0x38, 0xf9, 0x08},
            Intrinsic::X86DirectStore,
            "movdiri"},
           {{0x66, 0x0f, 0x38, 0xf8, 0x08},
            Intrinsic::X86DirectStore64B,
            "movdir64b"}}) {
    SCOPED_TRACE(E.Mnemonic);
    std::vector<uint8_t> Bytes = {0x48, 0xb8, 0,    0, 0, 0, 0, 0,
                                  0,    0x80, 0xb9, 1, 0, 0, 0};
    const auto Offset = Bytes.size();
    Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
    Bytes.push_back(0xc3);
    auto Image = image(Arch::X64, BinaryFormat::ELF, Bytes);
    for (bool NoOpt : {false, true}) {
      compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                    false, true);
      if (::testing::Test::HasFailure())
        return;
    }
  }
}

TEST(X86SystemMemorySource, MalformedLLVMAssemblyCannotBypassTheContract) {
  for (const auto Id :
       {Intrinsic::CetWrss, Intrinsic::CetWruss, Intrinsic::Enqcmd,
        Intrinsic::Enqcmds, Intrinsic::X86DirectStore,
        Intrinsic::X86DirectStore64B})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Id));
      SCOPED_TRACE(Mutation);
      const bool Enqueue = Id == Intrinsic::Enqcmd || Id == Intrinsic::Enqcmds;
      const auto P =
          x86SystemStateAsm(Id, Arch::X64, 8, NdMemoryAddressSpace::Default);
      ASSERT_TRUE(P);
      llvm::LLVMContext Context;
      llvm::Module Module("system-memory-contract", Context);
      Module.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(Context);
      auto *Pointer = llvm::PointerType::getUnqual(Context);
      std::vector<llvm::Type *> Types = {
          Mutation == 4 ? static_cast<llvm::Type *>(B.getInt64Ty()) : Pointer,
          isX86SystemStateAddressArgument(Id, 1)
              ? static_cast<llvm::Type *>(Pointer)
              : B.getInt64Ty()};
      llvm::Type *Return = Enqueue ? B.getInt8Ty() : B.getVoidTy();
      auto *Signature = llvm::FunctionType::get(Return, Types, false);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, "effect", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      std::string Constraints = P->Constraints;
      if (Mutation == 1)
        Constraints.erase(Constraints.find(",~{memory}"));
      auto *Assembly = llvm::InlineAsm::get(
          Signature, P->Text + (Mutation == 3 ? "\n\tnop" : ""), Constraints,
          Mutation != 2, false,
          Mutation == 6 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
      auto *Call =
          B.CreateCall(Assembly, {Function->getArg(0), Function->getArg(1)});
      if (Mutation == 5)
        Call->setOnlyReadsMemory();
      if (Mutation == 7)
        Call->setOnlyWritesMemory();
      if (Enqueue)
        B.CreateRet(Call);
      else
        B.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.EmitIncludes = false;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      if (Mutation == 0)
        EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Text;
      else
        EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                     std::invalid_argument);
    }
}

namespace {

std::vector<uint8_t> directStoreFunction(Arch A, bool Block, bool Wide,
                                         size_t &Offset) {
  std::vector<uint8_t> Bytes;
  if (A == Arch::X86)
    Bytes = {0x8b, 0x54, 0x24, 0x04, 0x8b, 0x4c, 0x24, 0x08};
  // Set CF/ZF before the direct store; both it and SFENCE preserve flags.
  Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xf9});
  Offset = Bytes.size();
  if (Block)
    Bytes.insert(Bytes.end(), {0x66, 0x0f, 0x38, 0xf8, 0x3e}); // rdi, [rsi]
  else {
    if (Wide)
      Bytes.push_back(0x48);
    Bytes.insert(
        Bytes.end(),
        {0x0f, 0x38, 0xf9, static_cast<uint8_t>(A == Arch::X86 ? 0x0a : 0x37)});
  }
  // Read completion only after an architecturally explicit store fence.
  Bytes.insert(Bytes.end(), {0x0f, 0xae, 0xf8, 0x0f, 0x92, 0xc0, 0x0f,
                             0x94, 0xc2, 0x0f, 0xb6, 0xc0, 0x0f, 0xb6,
                             0xd2, 0xc1, 0xe2, 0x08, 0x09, 0xd0, 0xc3});
  return Bytes;
}

std::string directStoreDriver(Arch A, bool Block, unsigned Width) {
  std::string Driver = "#define STORE_BYTES " + std::to_string(Width) + "\n";
  Driver += R"C(
static unsigned char bytes[256] __attribute__((aligned(64)));
static unsigned char expected[256];
)C";
  if (Block)
    Driver += R"C(
extern unsigned virtualization_test(void *, const void *);
static int check(void) {
  const unsigned sources[] = {0, 1, 8, 64, 65, 127};
  const unsigned destinations[] = {0, 64, 128};
  for (unsigned s = 0; s != sizeof(sources) / sizeof(*sources); ++s)
    for (unsigned d = 0; d != sizeof(destinations) / sizeof(*destinations); ++d) {
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        expected[i] = bytes[i] = (unsigned char)(i * 13 + s * 31 + d * 7);
      for (unsigned i = 0; i != 64; ++i)
        expected[destinations[d] + i] = bytes[sources[s] + i];
      if (virtualization_test(bytes + destinations[d], bytes + sources[s]) != 0x101)
        return 1;
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        if (bytes[i] != expected[i]) return 2;
    }
  return 0;
}
)C";
  else
    Driver += R"C(
extern unsigned virtualization_test(void *, __UINTPTR_TYPE__);
static int check(void) {
  const unsigned offsets[] = {0, 1, 3, 7, 15, 60, 63};
  const __UINTPTR_TYPE__ values[] = {0, (__UINTPTR_TYPE__)-1,
      (__UINTPTR_TYPE__)0xfedcba9876543210ULL, (__UINTPTR_TYPE__)0x8000000180000000ULL};
  for (unsigned o = 0; o != sizeof(offsets) / sizeof(*offsets); ++o)
    for (unsigned v = 0; v != sizeof(values) / sizeof(*values); ++v) {
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        expected[i] = bytes[i] = (unsigned char)(i * 13 + o * 7);
      for (unsigned i = 0; i != STORE_BYTES; ++i)
        expected[offsets[o] + i] = (unsigned char)(values[v] >> (8 * i));
      if (virtualization_test(bytes + offsets[o], values[v]) != 0x101) return 1;
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        if (bytes[i] != expected[i]) return 2;
    }
  return 0;
}
)C";
  if (A == Arch::X86)
    Driver += R"C(
void *memcpy(void *dst, const void *src, unsigned size) {
  for (unsigned i = 0; i != size; ++i)
    ((unsigned char *)dst)[i] = ((const unsigned char *)src)[i];
  return dst;
}
void _start(void) {
  unsigned status = check();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
)C";
  else
    Driver += "int main(void) { return check(); }\n";
  return Driver;
}

} // namespace

TEST(X86DirectStoreSource, ScalarStoresMatchOriginalBytesAndPreserveFlags) {
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("movdiri"))
    GTEST_SKIP() << "Original-byte execution requires host MOVDIRI";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const bool Wide : {false, true}) {
      if (A == Arch::X86 && Wide)
        continue;
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF,
                         directStoreFunction(A, false, Wide, Offset));
      for (const bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(Wide);
        SCOPED_TRACE(NoOpt);
        compileRoutes(Image, NoOpt, Entry + Offset, "movdiri", std::nullopt,
                      true, false, directStoreDriver(A, false, Wide ? 8 : 4));
        if (::testing::Test::HasFailure())
          return;
      }
    }
}

TEST(X86DirectStoreSource, BlockStoresMatchOriginalBytesIncludingOverlap) {
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("movdir64b"))
    GTEST_SKIP() << "Original-byte execution requires host MOVDIR64B";
  size_t Offset;
  auto Image = image(Arch::X64, BinaryFormat::ELF,
                     directStoreFunction(Arch::X64, true, false, Offset));
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    compileRoutes(Image, NoOpt, Entry + Offset, "movdir64b", std::nullopt, true,
                  false, directStoreDriver(Arch::X64, true, 64));
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(X86DirectStoreSource, StackWritesInvalidateEarlierValuesAcrossRoutes) {
  if (!llvm::sys::getHostCPUFeatures().lookup("movdiri"))
    GTEST_SKIP() << "Original-byte execution requires host MOVDIRI";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const bool Wide : {false, true}) {
      if (A == Arch::X86 && Wide)
        continue;
      const uint64_t Value = Wide ? UINT64_C(0xfedcba9876543210) : 0x87654321;
      std::vector<uint8_t> Bytes;
      if (A == Arch::X64)
        Bytes.push_back(0x48);
      Bytes.insert(Bytes.end(), {0x83, 0xec, 0x10});
      if (Wide)
        Bytes.push_back(0x48);
      Bytes.insert(Bytes.end(), {0xc7, 0x04, 0x24, 0x55, 0, 0, 0});
      if (Wide)
        Bytes.push_back(0x48);
      Bytes.push_back(0xb9);
      for (unsigned I = 0; I != (Wide ? 8u : 4u); ++I)
        Bytes.push_back(Value >> (8 * I));
      const auto Offset = Bytes.size();
      if (Wide)
        Bytes.push_back(0x48);
      Bytes.insert(Bytes.end(),
                   {0x0f, 0x38, 0xf9, 0x0c, 0x24, 0x0f, 0xae, 0xf8});
      if (Wide)
        Bytes.push_back(0x48);
      Bytes.insert(Bytes.end(), {0x8b, 0x04, 0x24});
      if (A == Arch::X64)
        Bytes.push_back(0x48);
      Bytes.insert(Bytes.end(), {0x83, 0xc4, 0x10, 0xc3});
      auto Image = image(A, BinaryFormat::ELF, Bytes);
      for (const bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(Wide);
        SCOPED_TRACE(NoOpt);
        compileRoutes(Image, NoOpt, Entry + Offset, "movdiri", Value, true);
        if (::testing::Test::HasFailure())
          return;
      }
    }
}

TEST(X86FenceSource, HardwareBarriersSurviveNativeLLVMOptimization) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : std::vector<SystemEncoding>{
             {{0x0f, 0xae, 0xe8}, Intrinsic::Lfence, "lfence"},
             {{0x0f, 0xae, 0xf0}, Intrinsic::Mfence, "mfence"},
             {{0x0f, 0xae, 0xf8}, Intrinsic::Sfence, "sfence"}})
      for (const bool NoOpt : {false, true}) {
        size_t Offset;
        auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
        std::string Native;
        std::vector<uint8_t> Object;
        sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, nullptr, nullptr,
                     {}, {}, &Native, &Object);
        EXPECT_NE(Native.find("asm sideeffect \"" + std::string(E.Mnemonic) +
                              "\", \"~{memory}\""),
                  std::string::npos)
            << Native;
        EXPECT_NE(std::search(Object.begin(), Object.end(), E.Bytes.begin(),
                              E.Bytes.end()),
                  Object.end());
        compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86SHA512Source, BothCAndLLVMExecuteWithoutSHA512Hardware) {
  const Words D = {0x0123456789abcdef, 0x8899aabbccddeeff, 0x1020304050607080,
                   0xfedcba9876543210};
  const Words S = {0xa1b2c3d4e5f60718, 0xf1e2d3c4b5a69788, 0x1122334455667788,
                   0x9988776655443322};
  const Words W = {0xffffffffffffffff, 0x55555555aaaaaaaa};
  for (const unsigned Opcode : {0xcc, 0xcd, 0xcb})
    for (const bool NoOpt : {false, true}) {
      SCOPED_TRACE(Opcode);
      SCOPED_TRACE(NoOpt);
      std::vector<uint8_t> Bytes = {0x48, 0x83, 0xec, 0x50};
      unsigned Offset = 0;
      for (const auto V :
           {D[0], D[1], D[2], D[3], S[0], S[1], S[2], S[3], W[0], W[1]}) {
        Bytes.insert(Bytes.end(), {0x48, 0xb8}); // movabs rax, value
        for (unsigned I = 0; I != 8; ++I)
          Bytes.push_back(V >> (8 * I));
        Bytes.insert(Bytes.end(),
                     {0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(Offset)});
        Offset += 8;
      }
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x6f, 0x14, 0x24,         // ymm2 <- D
                    0xc5, 0xfe, 0x6f, 0x5c, 0x24, 0x20,   // ymm3 <- S
                    0xc5, 0xfa, 0x6f, 0x64, 0x24, 0x40}); // xmm4 <- W
      Bytes.insert(Bytes.end(),
                   {0xc4, 0xe2,
                    static_cast<uint8_t>(Opcode == 0xcb ? 0x67 : 0x7f),
                    static_cast<uint8_t>(Opcode),
                    static_cast<uint8_t>(Opcode == 0xcb ? 0xd4 : 0xd3), 0xc5,
                    0xfe, 0x7f, 0x14, 0x24,   // store ymm2
                    0x48, 0x8b, 0x04, 0x24}); // rax <- lane 0
      for (const uint8_t Disp : {8, 16, 24})
        Bytes.insert(Bytes.end(), {0x48, 0x33, 0x44, 0x24, Disp});
      Bytes.insert(Bytes.end(), {0x48, 0x83, 0xc4, 0x50, 0xc3});
      const auto R = sha512Reference(Opcode, D, S, W);
      auto Image = image(Arch::X64, BinaryFormat::ELF, std::move(Bytes));
      compileRoutes(Image, NoOpt, 0, {}, R[0] ^ R[1] ^ R[2] ^ R[3]);
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86SystemStateSource, FailedLarPreservesUpperDestinationInExecutableCode) {
  for (const bool NoOpt : {false, true}) {
    // A null selector fails LAR at CPL3 without touching privileged state.
    // The 32-bit destination must preserve *all* of RAX on failure.
    auto Image = image(Arch::X64, BinaryFormat::ELF,
                       {0x48, 0xb8, 0x89, 0x67, 0x45, 0x23, 0x01, 0xef, 0xcd,
                        0xab, 0x31, 0xc9, 0x0f, 0x02, 0xc1, 0xc3});
    compileRoutes(Image, NoOpt, Entry + 12, "lar",
                  UINT64_C(0xabcdef0123456789));
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(X86SystemStateSource, FailedLslPreservesUpperDestinationInExecutableCode) {
  for (const bool NoOpt : {false, true}) {
    // The null selector fails without a fault. LSL r32 must preserve all
    // 64 destination bits on failure, just as the original instruction does.
    auto Image = image(Arch::X64, BinaryFormat::ELF,
                       {0x48, 0xb8, 0x89, 0x67, 0x45, 0x23, 0x01, 0xef, 0xcd,
                        0xab, 0x31, 0xc9, 0x0f, 0x03, 0xc1, 0xc3});
    compileRoutes(Image, NoOpt, Entry + 12, {}, UINT64_C(0xabcdef0123456789),
                  true);
    if (::testing::Test::HasFailure())
      return;
  }
}

namespace {
void checkDescriptorQueryCompletion(llvm::StringRef NativeCCompiler = {}) {
  for (const uint8_t Opcode : {0x02, 0x03})
    for (const uint8_t Width : {2, 4, 8})
      for (const bool Memory : {false, true})
        for (const bool Success : {false, true})
          for (const bool NoOpt : {false, true}) {
            SCOPED_TRACE(Opcode);
            SCOPED_TRACE(Width);
            SCOPED_TRACE(Memory);
            SCOPED_TRACE(Success);
            SCOPED_TRACE(NoOpt);
            std::vector<uint8_t> Bytes = {
                0x48, 0x83, 0xec, 0x10, // Own a selector slot.
                0x48, 0xb8, 0x89, 0x67, 0x45, 0x23, 0x01,
                0xef, 0xcd, 0xab, 0x31, 0xc9}; // Full-width prior and a null
                                               // selector.
            if (Success)
              Bytes.insert(Bytes.end(), {0x8c, 0xc9}); // mov cx, cs
            if (Memory)
              Bytes.insert(Bytes.end(), {0x66, 0x89, 0x0c, 0x24});
            // Establish all arithmetic flags immediately before the query.
            Bytes.insert(Bytes.end(), {0x39, 0xc9}); // cmp ecx, ecx
            const auto Offset = Bytes.size();
            if (Width == 2)
              Bytes.push_back(0x66);
            else if (Width == 8)
              Bytes.push_back(0x48);
            Bytes.insert(Bytes.end(), {0x0f, Opcode});
            if (Memory)
              Bytes.insert(Bytes.end(), {0x04, 0x24});
            else
              Bytes.push_back(0xc1);
            // Mix the result with the actual arithmetic flags, keeping high
            // destination bits visible on failed and narrow writes.
            Bytes.insert(Bytes.end(), {0x9c, 0x5a, 0x81, 0xe2, 0xd5, 0x08, 0,
                                       0,    0x48, 0xc1, 0xe2, 0x20, 0x48, 0x31,
                                       0xd0, 0x48, 0x83, 0xc4, 0x10, 0xc3});
            auto Image = image(Arch::X64, BinaryFormat::ELF, std::move(Bytes));
            // Successful wide LAR leaves result bits 19:16 undefined. Keep
            // every failed/narrow destination bit in the comparison.
            std::string Driver = Opcode == 0x02 && Width != 2 && Success
                                     ? "#define RESULT_MASK (~0xf0000ULL)\n"
                                     : "#define RESULT_MASK (~0ULL)\n";
            Driver += R"c(
extern unsigned long long virtualization_test(void);
extern unsigned long long neverd_original(void);
int main(void) {
  return ((virtualization_test() ^ neverd_original()) & RESULT_MASK) != 0;
}
__asm__(".text\n.globl neverd_original\nneverd_original:\n"
)c";
            llvm::raw_string_ostream OS(Driver);
            for (const auto Byte : Image.Segments.front().Data)
              OS << "\".byte " << unsigned(Byte) << "\\n\"\n";
            OS << ");\n";
            compileRoutes(Image, NoOpt, Entry + Offset,
                          Opcode == 0x02 ? "lar" : "lsl", std::nullopt, false,
                          false, Driver, {}, 8, false, NativeCCompiler);
            if (::testing::Test::HasFailure())
              return;
          }
}
} // namespace

TEST(X86SystemStateSource, DescriptorResultsAndFlagsMatchOriginalInstructions) {
  checkDescriptorQueryCompletion();
}

TEST(X86SystemStateSource,
     NeverCDescriptorResultsAndFlagsMatchOriginalInstructions) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  checkDescriptorQueryCompletion(Compiler);
}

TEST(X86MaskedStoreSource, AllFourStoreFamiliesCompileThroughEveryRoute) {
  for (const bool Integer : {false, true})
    for (const bool Qword : {false, true})
      for (const bool Wide : {false, true}) {
        SCOPED_TRACE(Integer);
        SCOPED_TRACE(Qword);
        SCOPED_TRACE(Wide);
        std::vector<uint8_t> Bytes = {0x48, 0x83, 0xec, 0x60};
        for (unsigned I = 0; I != 12; ++I) {
          const uint64_t Value = I < 4 ? UINT64_C(0xa5a5a5a5a5a5a5a5)
                                 : I == 4
                                     ? (Qword ? UINT64_C(0x8000000000000000)
                                              : UINT64_C(0x80000000))
                                 : I < 8 ? 0
                                         : UINT64_C(0x5555555555555555);
          Bytes.insert(Bytes.end(), {0x48, 0xb8});
          for (unsigned J = 0; J != 8; ++J)
            Bytes.push_back(Value >> (8 * J));
          Bytes.insert(Bytes.end(),
                       {0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(8 * I)});
        }
        for (const std::vector<uint8_t> &Load :
             {std::vector<uint8_t>{0xc5,
                                   static_cast<uint8_t>(Wide ? 0xfe : 0xfa),
                                   0x6f, 0x14, 0x24},
              std::vector<uint8_t>{0xc5,
                                   static_cast<uint8_t>(Wide ? 0xfe : 0xfa),
                                   0x6f, 0x6c, 0x24, 0x20}})
          Bytes.insert(Bytes.end(), Load.begin(), Load.end());
        Bytes.insert(Bytes.end(),
                     {0xc4, 0xe2,
                      static_cast<uint8_t>(0x51 | (Wide ? 4 : 0) |
                                           (Integer && Qword ? 0x80 : 0)),
                      static_cast<uint8_t>(Integer ? 0x8e
                                           : Qword ? 0x2f
                                                   : 0x2e),
                      0x54, 0x24, 0x40, 0x48, 0x8b, 0x44, 0x24, 0x40, 0x48,
                      0x83, 0xc4, 0x60, 0xc3});
        auto Image = image(Arch::X64, BinaryFormat::ELF, std::move(Bytes));
        compileRoutes(Image, false, 0, {});
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86MaskedStoreSource,
     DynamicInactiveLanesNeitherAccessMemoryNorReadPoison) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "Native execution fixture requires Linux x86-64";
#endif
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::LLVMContext Context;
  llvm::Module Module("masked-memory", Context);
  Module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
  llvm::IRBuilder<> B(Context);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getInt1Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "masked_memory_test", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
  auto *Data = llvm::ConstantVector::get(
      {B.getInt64(0x123456789abcdef0), llvm::PoisonValue::get(B.getInt64Ty())});
  auto *Mask =
      B.CreateInsertElement(llvm::Constant::getNullValue(
                                llvm::FixedVectorType::get(B.getInt1Ty(), 2)),
                            F->getArg(1), B.getInt32(0));
  B.CreateMaskedStore(Data, F->getArg(0), llvm::Align(1), Mask);
  B.CreateRetVoid();
  auto *Load = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt1Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "masked_memory_load", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Load));
  auto *LoadMask =
      B.CreateInsertElement(llvm::Constant::getNullValue(
                                llvm::FixedVectorType::get(B.getInt1Ty(), 2)),
                            Load->getArg(1), B.getInt32(0));
  auto *Passthrough = llvm::ConstantVector::get(
      {B.getInt64(0x77), llvm::PoisonValue::get(B.getInt64Ty())});
  auto *Value = B.CreateMaskedLoad(Passthrough->getType(), Load->getArg(0),
                                   llvm::Align(1), LoadMask, Passthrough);
  B.CreateRet(B.CreateExtractElement(Value, B.getInt32(0)));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Original;
  llvm::raw_string_ostream OriginalOS(Original);
  Module.print(OriginalOS, nullptr);
  CEmitterOptions O;
  O.TheArch = Arch::X64;
  O.Format = BinaryFormat::ELF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, O));
  // Scalarization is private to the C projection.
  std::string After;
  llvm::raw_string_ostream AfterOS(After);
  Module.print(AfterOS, nullptr);
  EXPECT_EQ(Original, After);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-masked-memory", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  llvm::SmallString<128> Input(Directory), Driver(Directory),
      Executable(Directory), Errors(Directory);
  llvm::sys::path::append(Input, "source.c");
  llvm::sys::path::append(Driver, "driver.c");
  llvm::sys::path::append(Executable, "check");
  llvm::sys::path::append(Errors, "clang.err");
  std::error_code Error;
  llvm::raw_fd_ostream SourceOS(Input, Error);
  ASSERT_FALSE(Error);
  SourceOS << Source;
  SourceOS.close();
  llvm::raw_fd_ostream DriverOS(Driver, Error);
  ASSERT_FALSE(Error);
  DriverOS << R"C(
#include <stdint.h>
extern void masked_memory_test(void *, _Bool);
extern uint64_t masked_memory_load(void *, _Bool);
int main(void) {
  masked_memory_test((void *)(uintptr_t)1, 0);
  if (masked_memory_load((void *)(uintptr_t)1, 0) != UINT64_C(0x77)) return 1;
  uint64_t data[2] = {0, UINT64_C(0xabcdef1234567890)};
  masked_memory_test(data, 1);
  return data[0] != UINT64_C(0x123456789abcdef0) ||
         data[1] != UINT64_C(0xabcdef1234567890) ||
         masked_memory_load(data, 1) != data[0];
}
)C";
  DriverOS.close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    const int Status = llvm::sys::ExecuteAndWait(
        Compiler,
        {Compiler, "-std=gnu11", Optimization, Input, Driver, "-o", Executable},
        std::nullopt, Redirects);
    auto Diagnostics = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Status, 0) << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                         : "")
                         << Source;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0) << Source;
  }
}

namespace {
std::vector<uint8_t> systemExtensionFunction(const SystemEncoding &E, bool Dead,
                                             size_t &Offset) {
  std::vector<uint8_t> Bytes;
  auto Move = [&](uint8_t Opcode, uint64_t Value) {
    Bytes.insert(Bytes.end(), {0x48, Opcode});
    for (unsigned I = 0; I < 8; ++I)
      Bytes.push_back(Value >> (8 * I));
  };
  Move(0xb8, UINT64_C(0x1122334455667788));
  Move(0xb9, UINT64_C(0x88776655ffffffff));
  Move(0xba, UINT64_C(0xfedcba98cafebabe));
  Move(0xbe, UINT64_C(0x1122334455667788));
  Offset = Bytes.size();
  Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
  if (Dead)
    Bytes.insert(Bytes.end(), {0x48, 0x31, 0xc0});
  else if (E.Id == Intrinsic::X86TestUserInterrupt)
    // Read all six arithmetic flags, not just the reported CF output.
    Bytes.insert(Bytes.end(), {0x9c, 0x58, 0x48, 0x25, 0xd5, 0x08, 0x00, 0x00});
  else
    Bytes.insert(Bytes.end(), {0x48, 0x31, 0xd0, 0x48, 0x31, 0xc8});
  Bytes.push_back(0xc3);
  return Bytes;
}

std::string systemExtensionDriver(const BinaryImage &Image,
                                  const SystemEncoding &E, bool Dead) {
  std::string Driver = faultDriver(Image);
  for (const auto Name : {"virtualization_test", "neverd_original"}) {
    const auto Declaration = "extern void " + std::string(Name);
    Driver.replace(Driver.find(Declaration), Declaration.size(),
                   "extern unsigned long long " + std::string(Name));
  }
  const std::string FunctionPointer = "void (*fn)(void)";
  Driver.replace(Driver.find(FunctionPointer), FunctionPointer.size(),
                 "unsigned long long (*fn)(void)");
  const std::string Main = "int main(void)";
  Driver.replace(Driver.find(Main), Main.size(),
                 "static int compare_faults(void)");
  Driver = "#define _GNU_SOURCE\n#include <stdint.h>\n#include <ucontext.h>\n" +
           Driver;
  const Intrinsic Kinds[] = {Intrinsic::X86ClearUserInterrupt,
                             Intrinsic::X86SetUserInterrupt,
                             Intrinsic::X86TestUserInterrupt,
                             Intrinsic::X86SendUserInterrupt,
                             Intrinsic::X86LoadKernelGs,
                             Intrinsic::X86LoadKernelGsMemory,
                             Intrinsic::X86HistoryReset,
                             Intrinsic::X86InvalidateBroadcast,
                             Intrinsic::X86TlbSync};
  const auto Kind =
      std::find(std::begin(Kinds), std::end(Kinds), E.Id) - std::begin(Kinds);
  const bool Address32 =
      std::find(E.Bytes.begin(), E.Bytes.end(), 0x67) != E.Bytes.end();
  const unsigned Segment =
      E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65 ? E.Bytes.front() : 0;
  llvm::raw_string_ostream OS(Driver);
  OS << "\nstatic const int kind = " << Kind << ", dead = " << Dead
     << ", address32 = " << Address32 << ", segment = " << Segment << ";\n";
  // Real faults are compared first. This second oracle validates transport
  // and result use through sigreturn, without claiming successful privileged
  // execution, UITT updates, descriptor loads or remote TLB invalidation.
  OS << R"C(
static volatile sig_atomic_t events, carry;
static uint64_t reg_value(greg_t *g, unsigned r) {
  static const unsigned registers[] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX,
    REG_RSP, REG_RBP, REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11,
    REG_R12, REG_R13, REG_R14, REG_R15 };
  return (uint64_t)g[registers[r]];
}
static void complete_system(int signal, siginfo_t *info, void *context) {
  (void)signal; (void)info;
  greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;
  const unsigned char *ip = (const unsigned char *)(uintptr_t)g[REG_RIP];
  unsigned n = 0, rex = 0, seg = 0, a32 = 0, mandatory = 0;
  while (n < 12 && ip[n] != 0x0f) {
    unsigned b = ip[n++];
    if (b >= 0x40 && b <= 0x4f) { rex = b; continue; }
    rex = 0;
    if (b == 0xf2 || b == 0xf3) mandatory = b;
    else if (b == 0x64 || b == 0x65) seg = b;
    else if (b == 0x67) a32 = 1;
    else if (b != 0x66) _exit(21);
  }
  if (n == 12 || ip[n++] != 0x0f) _exit(22);
  unsigned map = ip[n++], modrm = ip[n++];
  if (kind <= 2) {
    unsigned expected = kind == 0 ? 0xee : kind == 1 ? 0xef : 0xed;
    if (mandatory != 0xf3 || map != 1 || modrm != expected) _exit(23);
    if (kind == 2) g[REG_EFL] = (g[REG_EFL] & ~(greg_t)0x8d5) | carry;
  } else if (kind <= 5) {
    if (mandatory != (kind == 3 ? 0xf3 : 0xf2) ||
        map != (kind == 3 ? 0xc7 : 0) || (modrm & 0x38) != 0x30) _exit(24);
    unsigned rm = (modrm & 7) | ((rex & 1) ? 8 : 0);
    uint64_t value;
    if (kind == 5) {
      if ((modrm & 0xc0) == 0xc0 || seg != segment) _exit(25);
      if ((modrm & 7) == 4) {
        unsigned sib = ip[n++];
        if ((sib & 0x38) != 0x20 || (rex & 2)) _exit(26);
        rm = (sib & 7) | ((rex & 1) ? 8 : 0);
      }
      value = reg_value(g, rm);
      if ((modrm & 0xc0) == 0x40) value += (int8_t)ip[n++];
      else if ((modrm & 0xc0) == 0x80) {
        uint32_t disp = ip[n] | ((uint32_t)ip[n+1] << 8) |
          ((uint32_t)ip[n+2] << 16) | ((uint32_t)ip[n+3] << 24);
        value += (int32_t)disp; n += 4;
      }
      if (a32) value = (uint32_t)value;
      if (value != (address32 ? UINT64_C(0x55667788) : UINT64_C(0x1122334455667788))) _exit(27);
    } else {
      if ((modrm & 0xc0) != 0xc0) _exit(28);
      value = reg_value(g, rm);
      if (kind == 4) value = (uint16_t)value;
      if (value != (kind == 4 ? UINT64_C(0x7788) : UINT64_C(0x1122334455667788))) _exit(29);
    }
  } else if (kind == 6) {
    if (mandatory != 0xf3 || map != 0x3a || modrm != 0xf0 || ip[n++] != 0xc0 ||
        (uint32_t)g[REG_RAX] != UINT32_C(0x55667788)) _exit(30);
    ++n; // imm8 is architecturally ignored, including the original 0xab.
  } else {
    if (mandatory || map != 1 || modrm != (kind == 7 ? 0xfe : 0xff)) _exit(31);
    if (kind == 7) {
      uint64_t value = a32 ? (uint32_t)g[REG_RAX] : (uint64_t)g[REG_RAX];
      if (a32 != address32 ||
          value != (address32 ? UINT64_C(0x55667788) : UINT64_C(0x1122334455667788)) ||
          (uint32_t)g[REG_RDX] != UINT32_C(0xcafebabe) ||
          (uint32_t)g[REG_RCX] != UINT32_MAX) _exit(32);
    }
  }
  g[REG_RIP] += n;
  ++events;
}
int main(void) {
  if (compare_faults()) return 10;
  struct sigaction action = {0};
  action.sa_sigaction = complete_system;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, 0) || sigaction(SIGSEGV, &action, 0)) return 11;
  for (carry = 0; carry <= (kind == 2); ++carry) {
    uint64_t expected = dead ? 0 : kind == 2 ? (uint64_t)carry :
      UINT64_C(0x1122334455667788) ^ UINT64_C(0x88776655ffffffff) ^ UINT64_C(0xfedcba98cafebabe);
    events = 0;
    uint64_t original = neverd_original();
    unsigned count = events;
    // OS-enabled UINTR may execute without a fault. Compare its real result;
    // controlled CF injection is qualified only on the faulting path.
    if (count == 0 && kind <= 2) expected = original;
    else if (count != 1) return 12;
    if (original != expected) return 13;
    events = 0;
    if (virtualization_test() != expected || events != count) return 14;
  }
  return 0;
}
)C";
  return Driver;
}
} // namespace

TEST(X86SystemExtensionsSource, FaultsInputsAndFlagResultsMatchEveryRoute) {
  for (const auto &E : systemExtensionEncodings(Arch::X64))
    for (bool NoOpt : {false, true})
      for (bool Dead : {false, true}) {
        if (Dead && E.Id != Intrinsic::X86TestUserInterrupt)
          continue;
        SCOPED_TRACE(E.Mnemonic);
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Dead);
        size_t Offset;
        auto Image = image(Arch::X64, BinaryFormat::ELF,
                           systemExtensionFunction(E, Dead, Offset));
        compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                      false, false, systemExtensionDriver(Image, E, Dead), {},
                      8);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86SystemExtensionsSource, RealCarrySurvivesEveryNativeObjectFormat) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true}) {
      const SystemEncoding E{
          {0xf3, 0x0f, 0x01, 0xed}, Intrinsic::X86TestUserInterrupt, "testui"};
      size_t Offset;
      auto Image =
          image(Arch::X64, Format, systemExtensionFunction(E, false, Offset));
      std::vector<uint8_t> Object;
      sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, nullptr, nullptr,
                   {}, {}, nullptr, &Object, 8);
      EXPECT_NE(std::search(Object.begin(), Object.end(), E.Bytes.begin(),
                            E.Bytes.end()),
                Object.end());
    }
}

TEST(X86SystemExtensionsSource, MalformedOwnedAssemblyCannotLoseItsEffects) {
  const Intrinsic Ids[] = {Intrinsic::X86ClearUserInterrupt,
                           Intrinsic::X86SetUserInterrupt,
                           Intrinsic::X86TestUserInterrupt,
                           Intrinsic::X86SendUserInterrupt,
                           Intrinsic::X86LoadKernelGs,
                           Intrinsic::X86LoadKernelGsMemory,
                           Intrinsic::X86HistoryReset,
                           Intrinsic::X86InvalidateBroadcast,
                           Intrinsic::X86TlbSync};
  for (auto Id : Ids)
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Id));
      SCOPED_TRACE(Mutation);
      const auto P =
          x86SystemStateAsm(Id, Arch::X64, 4, NdMemoryAddressSpace::Default);
      ASSERT_TRUE(P);
      llvm::LLVMContext Context;
      llvm::Module Module("bounded-system-contract", Context);
      Module.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(Context);
      std::vector<llvm::Type *> Types;
      for (unsigned I = 0; I < P->InputBytes.size(); ++I)
        Types.push_back(isX86SystemStateAddressArgument(Id, I)
                            ? static_cast<llvm::Type *>(B.getPtrTy())
                            : B.getIntNTy(8 * P->InputBytes[I]));
      llvm::Type *Return =
          P->OutputBytes.empty() ? B.getVoidTy() : B.getInt8Ty();
      if (Mutation == 4)
        Return = B.getInt16Ty();
      auto *Signature = llvm::FunctionType::get(Return, Types, false);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, "effect", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      std::vector<llvm::Value *> Values;
      for (auto &Arg : Function->args())
        Values.push_back(&Arg);
      auto Constraints = P->Constraints;
      if (Mutation == 4 && P->OutputBytes.empty())
        Constraints = "=r," + Constraints;
      if (Mutation == 1) {
        auto Pos = Constraints.find("~{memory}");
        if (Pos)
          --Pos;
        Constraints.erase(Pos);
      }
      auto *Asm = llvm::InlineAsm::get(
          Signature, P->Text + (Mutation == 6 ? "\n\tnop" : ""), Constraints,
          Mutation != 2);
      auto *Call = B.CreateCall(Asm, Values);
      if (Mutation == 3)
        Call->setDoesNotReturn();
      if (Mutation == 5)
        Call->setOnlyReadsMemory();
      if (Return->isVoidTy())
        B.CreateRetVoid();
      else
        B.CreateRet(Call);
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.EmitIncludes = false;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      if (Mutation == 0)
        EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Text;
      else
        EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                     std::invalid_argument);
    }
}

namespace {
std::vector<uint8_t> sgxFunction(Arch A, bool Supervisor, bool Dead,
                                 size_t &Offset) {
  const bool Wide = A == Arch::X64;
  std::vector<uint8_t> B = {0x53}; // preserve the ABI's rBX
  auto Move = [&](uint8_t Opcode, uint64_t Value) {
    if (Wide)
      B.push_back(0x48);
    B.push_back(Opcode);
    for (unsigned I = 0; I < (Wide ? 8u : 4u); ++I)
      B.push_back(Value >> (8 * I));
  };
  if (Wide)
    B.insert(B.end(), {0x48, 0x89, 0xf8}); // dynamic leaf from rDI
  else
    B.insert(B.end(), {0x8b, 0x44, 0x24, 0x08}); // cdecl leaf
  Move(0xbb, UINT64_C(0x1122334455667788));
  Move(0xb9, UINT64_C(0x2233445566778899));
  Move(0xba, UINT64_C(0x33445566778899aa));
  // The original flags are OF/SF/AF/PF/CF, with ZF clear.
  B.insert(B.end(),
           {0x50, 0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 0x01, 0xf9, 0x58});
  Offset = B.size();
  B.insert(B.end(),
           {0x0f, 0x01, static_cast<uint8_t>(Supervisor ? 0xcf : 0xc0)});
  if (Dead) {
    if (Wide)
      B.push_back(0x48);
    B.insert(B.end(), {0x31, 0xc0});
  } else {
    // Capture before arithmetic consumes the instruction's flags. rSI is
    // saved below in the i386 fixture, where it is callee-saved.
    if (Wide)
      B.insert(B.end(), {0x9c, 0x41, 0x58, 0x49, 0x81, 0xe0, 0xd5, 0x08,
                         0,    0,    0x48, 0x31, 0xd8, 0x48, 0x31, 0xc8,
                         0x48, 0x31, 0xd0, 0x4c, 0x31, 0xc0});
    else
      B.insert(B.end(), {0x56, 0x9c, 0x5e, 0x81, 0xe6, 0xd5, 0x08, 0, 0, 0x31,
                         0xd8, 0x31, 0xc8, 0x31, 0xd0, 0x31, 0xf0, 0x5e});
  }
  B.insert(B.end(), {0x5b, 0xc3});
  return B;
}

std::string sgxDriver(const BinaryImage &Image, bool Supervisor, bool Dead) {
  std::string D = faultDriver(Image);
  for (const char *Name : {"virtualization_test", "neverd_original"}) {
    const std::string Old = std::string("extern void ") + Name + "(void);";
    D.replace(D.find(Old), Old.size(),
              std::string("extern unsigned long long ") + Name +
                  "(unsigned long long);");
  }
  D.replace(D.find("int main(void)"), 14, "int compare_faults(void)");
  for (const char *Name : {"virtualization_test", "neverd_original"}) {
    const std::string Old = std::string("observe(") + Name;
    D.replace(D.find(Old), Old.size(),
              std::string("observe((void (*)(void))") + Name);
  }
  D = "#define _GNU_SOURCE\n#include <stdint.h>\n#include <ucontext.h>\n" + D;
  llvm::raw_string_ostream OS(D);
  OS << "\nstatic const int supervisor = " << Supervisor << ", dead = " << Dead
     << ";\n";
  OS << R"C(
static volatile sig_atomic_t events, completion;
static uint64_t leaf_input;
static int preserves(unsigned leaf) {
  return supervisor && (leaf == 0 || leaf == 1 || leaf == 6 || leaf == 10 || leaf == 13);
}
static void complete_sgx(int signal, siginfo_t *info, void *context) {
  (void)signal; (void)info;
  greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;
  const unsigned char *ip = (const unsigned char *)(uintptr_t)g[REG_RIP];
  if (ip[0] != 0x0f || ip[1] != 1 || ip[2] != (supervisor ? 0xcf : 0xc0)) _exit(21);
  unsigned leaf = (uint32_t)leaf_input;
  int b_used = supervisor ? (leaf <= 19 && leaf != 3 && leaf != 9 &&
                              leaf != 12 && leaf != 17) : leaf < 2;
  int c_used = !supervisor || leaf <= 19;
  int d_used = supervisor ? (leaf == 2 || leaf == 7 || leaf == 8 ||
                              leaf == 11 || leaf == 18 || leaf == 19) : leaf == 2;
  if ((uint64_t)g[REG_RAX] != leaf_input ||
      (b_used && (uint64_t)g[REG_RBX] != UINT64_C(0x1122334455667788)) ||
      (c_used && (uint64_t)g[REG_RCX] != UINT64_C(0x2233445566778899)) ||
      (d_used && (uint64_t)g[REG_RDX] != UINT64_C(0x33445566778899aa))) _exit(22);
  g[REG_RAX] = UINT64_C(0xfedcba9876543210);
  if (supervisor && (uint32_t)leaf_input == 4 && !(completion & 2))
    g[REG_RBX] = UINT64_C(0xabcddcba11223344);
  if (!preserves((uint32_t)leaf_input))
    g[REG_EFL] = (g[REG_EFL] & ~(greg_t)0x8d5) |
                (completion & 1) | ((completion >> 1) << 6);
  g[REG_RIP] += 3;
  ++events;
}
int main(void) {
  if (compare_faults()) return 10;
  struct sigaction action = {0};
  action.sa_sigaction = complete_sgx;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, 0) || sigaction(SIGSEGV, &action, 0)) return 11;
  for (unsigned leaf = 0; leaf <= (supervisor ? 24u : 2u); ++leaf) {
    if (supervisor && leaf > 19 && leaf != 24) continue;
    leaf_input = UINT64_C(0x9876543200000000) | leaf;
    for (completion = 0; completion < 4; ++completion) {
      uint64_t flags = preserves(leaf) ? 0x895 :
          (unsigned)(completion & 1) | ((unsigned)(completion >> 1) << 6);
      uint64_t bx = supervisor && leaf == 4 && !(completion & 2) ? UINT64_C(0xabcddcba11223344) :
                                               UINT64_C(0x1122334455667788);
      uint64_t expected = dead ? 0 : UINT64_C(0xfedcba9876543210) ^ bx ^
          UINT64_C(0x2233445566778899) ^ UINT64_C(0x33445566778899aa) ^ flags;
      events = 0;
      if (neverd_original(leaf_input) != expected || events != 1) return 12;
      events = 0;
      if (virtualization_test(leaf_input) != expected || events != 1) return 13;
    }
  }
  return 0;
}
)C";
  return D;
}
} // namespace

TEST(X86SgxSource, DynamicLeavesRetainRegistersFlagsFaultsAndDeadEffects) {
  for (bool Supervisor : {false, true})
    for (bool NoOpt : {false, true})
      for (bool Dead : {false, true}) {
        SCOPED_TRACE(Supervisor);
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Dead);
        size_t Offset;
        auto I = image(Arch::X64, BinaryFormat::ELF,
                       sgxFunction(Arch::X64, Supervisor, Dead, Offset));
        const std::vector<uint8_t> Encoding = {
            0x0f, 0x01, static_cast<uint8_t>(Supervisor ? 0xcf : 0xc0)};
        compileRoutes(I, NoOpt, Entry + Offset, Supervisor ? "encls" : "enclv",
                      std::nullopt, false, false,
                      sgxDriver(I, Supervisor, Dead), Encoding, 8);
      }
}

TEST(X86SgxSource, BothWordSizesRetainCompletionAcrossNativeObjectFormats) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Supervisor : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(Supervisor);
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto I = image(A, Format, sgxFunction(A, Supervisor, false, Offset));
          // The x64 COFF ABI's first parameter is RCX rather than RDI.
          if (A == Arch::X64 && Format == BinaryFormat::COFF)
            I.Segments.front().Data[3] = 0xc8;
          compileRoutes(I, NoOpt, Entry + Offset,
                        Supervisor ? "encls" : "enclv", std::nullopt, false,
                        false, {}, {}, A == Arch::X64 ? 8 : 4, true);
        }
}

TEST(X86SgxSource, MalformedAssemblyCannotDropRegistersOrEffects) {
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Id : {Intrinsic::X86Encls, Intrinsic::X86Enclv})
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Id));
        SCOPED_TRACE(Mutation);
        const auto P = x86SgxAsm(Id, A);
        ASSERT_TRUE(P);
        llvm::LLVMContext Context;
        llvm::Module M("sgx-contract", Context);
        M.setDataLayout(A == Arch::X64 ? "e-p:64:64" : "e-p:32:32");
        llvm::IRBuilder<> B(Context);
        auto *Word = B.getIntNTy(P->WordBytes * 8);
        auto *Return = llvm::StructType::get(
            Context, {Word, Word, B.getIntNTy(Mutation == 4 ? 32 : 16)});
        std::vector<llvm::Type *> Types(4, Word);
        if (Mutation == 7)
          Types[3] = B.getInt16Ty();
        auto *Signature = llvm::FunctionType::get(Return, Types, false);
        auto *F = llvm::Function::Create(
            Signature, llvm::GlobalValue::ExternalLinkage, "effect", M);
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
        std::vector<llvm::Value *> Values;
        for (auto &V : F->args())
          Values.push_back(&V);
        auto Constraints = P->Constraints;
        if (Mutation == 1)
          Constraints.erase(Constraints.find(",~{memory}"));
        auto *Asm = llvm::InlineAsm::get(
            Signature, P->Text + (Mutation == 6 ? "\n\tnop" : ""), Constraints,
            Mutation != 2);
        auto *Call = B.CreateCall(Asm, Values);
        if (Mutation == 3)
          Call->setDoesNotReturn();
        if (Mutation == 5)
          Call->setOnlyReadsMemory();
        B.CreateRet(Call);
        CEmitterOptions O;
        O.TheArch = A;
        O.EmitIncludes = false;
        std::string Text;
        llvm::raw_string_ostream OS(Text);
        if (!Mutation)
          EXPECT_TRUE(LLVMCEmitter().emit(M, OS, O)) << Text;
        else
          EXPECT_THROW(LLVMCEmitter().emit(M, OS, O), std::invalid_argument);
      }
}

TEST(X86SgxSource, NeverCCompletionMatchesOriginalRegistersAndFlags) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  for (bool Supervisor : {false, true})
    for (bool NoOpt : {false, true})
      for (bool Dead : {false, true}) {
        SCOPED_TRACE(Supervisor);
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Dead);
        size_t Offset;
        auto Image = image(Arch::X64, BinaryFormat::ELF,
                           sgxFunction(Arch::X64, Supervisor, Dead, Offset));
        const std::vector<uint8_t> Encoding = {
            0x0f, 0x01, static_cast<uint8_t>(Supervisor ? 0xcf : 0xc0)};
        compileRoutes(Image, NoOpt, Entry + Offset,
                      Supervisor ? "encls" : "enclv", std::nullopt, false,
                      false, sgxDriver(Image, Supervisor, Dead), Encoding, 8,
                      false, Compiler);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86SgxSource, NativeContractsRejectLostEffectsAndMode) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.encls"))
    GTEST_SKIP() << "native SGX completion requires the qualified fork";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Id : {Intrinsic::X86Encls, Intrinsic::X86Enclv})
      for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Id));
        SCOPED_TRACE(Mutation);
        llvm::LLVMContext Context;
        llvm::Module Module("sgx-native-contract", Context);
        llvm::IRBuilder<> B(Context);
        const auto ID =
            llvm::Intrinsic::lookupIntrinsicID(x86SgxNativeName(Id, A));
        auto *F = llvm::Intrinsic::getOrInsertDeclaration(&Module, ID);
        auto *Owner = llvm::Function::Create(
            llvm::FunctionType::get(B.getVoidTy(), false),
            llvm::GlobalValue::ExternalLinkage, "owner", Module);
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Owner));
        auto *Word = A == Arch::X64 ? B.getInt64Ty() : B.getInt32Ty();
        auto *Call = B.CreateCall(
            F, std::vector<llvm::Value *>(4, llvm::ConstantInt::get(Word, 0)));
        if (Mutation == 1)
          Call->setOnlyReadsMemory();
        if (Mutation == 2)
          Call->setOnlyWritesMemory();
        if (Mutation == 3)
          Call->setDoesNotReturn();
        if (Mutation == 4)
          Call->setArgOperand(0, B.getInt16(0));
        if (Mutation == 6)
          llvm::BasicBlock::Create(Context, "definition", F);
        const auto Mode =
            Mutation == 5 ? (A == Arch::X64 ? Arch::X86 : Arch::X64) : A;
        const std::vector<std::string> Arguments(4, "input");
        if (Mutation)
          EXPECT_THROW(renderX86SgxCall(Mode, *Call, {}, false, Arguments),
                       std::invalid_argument);
        else
          EXPECT_TRUE(renderX86SgxCall(Mode, *Call, {}, false, Arguments));
      }
}

TEST(X86SgxSource, NeverCCompilesNativeCompletionAndCombinedAmxFeatures) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-sgx-native-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (const auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool Supervisor : {false, true})
      for (bool Dead : {false, true})
        for (bool NoOpt : {false, true})
          for (bool Amx : {false, true}) {
            SCOPED_TRACE(static_cast<unsigned>(Format));
            SCOPED_TRACE(Supervisor);
            SCOPED_TRACE(Dead);
            SCOPED_TRACE(NoOpt);
            SCOPED_TRACE(Amx);
            size_t Offset;
            auto Bytes = sgxFunction(Arch::X64, Supervisor, Dead, Offset);
            if (Format == BinaryFormat::COFF)
              Bytes[3] = 0xc8;
            if (Amx) {
              const auto &Zero = amxEncodings()[3].Bytes;
              Bytes.insert(Bytes.begin() + Offset + 3, Zero.begin(),
                           Zero.end());
            }
            auto Image = image(Arch::X64, Format, Bytes);
            std::string High, LLVM, Native;
            sourceRoutes(Image, NoOpt, Entry + Offset, {}, &High, &LLVM, {}, {},
                         &Native);
            if (::testing::Test::HasFailure())
              return;
            const auto Name = x86SgxNativeName(Supervisor ? Intrinsic::X86Encls
                                                          : Intrinsic::X86Enclv,
                                               Arch::X64);
            if (llvm::Intrinsic::lookupIntrinsicID(Name)) {
              EXPECT_NE(Native.find("@" + Name + "("), std::string::npos)
                  << Native;
              // The fixture also owns a separate PUSHF snapshot. Check the
              // migrated SGX boundary without discarding that flag effect.
              EXPECT_EQ(Native.find("asm sideeffect \"encls"),
                        std::string::npos)
                  << Native;
              EXPECT_EQ(Native.find("asm sideeffect \"enclv"),
                        std::string::npos)
                  << Native;
              if (Amx)
                EXPECT_EQ(Native.find("asm sideeffect \"# neverd AMX"),
                          std::string::npos)
                    << Native;
            }
            for (const auto &Source : {High, LLVM}) {
              compileNativeC(Compiler, Directory, Format, Source, Name, Cases,
                             std::nullopt, true);
              if (::testing::Test::HasFailure())
                return;
            }
          }
  EXPECT_EQ(Cases, 192u);
  RecordProperty("sgx_native_c_ir_and_object_compilations", Cases);
}

TEST(X86SgxSource, UnusedLeafRegistersDoNotBecomeParametersOrTraps) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(NoOpt);
      auto I = image(A, BinaryFormat::ELF,
                     {0xb8, 0x18, 0, 0, 0, 0x0f, 0x01, 0xcf, 0xc3});
      std::string High, LLVM;
      sourceRoutes(I, NoOpt, Entry + 5, "encls", &High, &LLVM);
      EXPECT_EQ(High.find("unknown register"), std::string::npos) << High;
      EXPECT_EQ(High.find("arg0"), std::string::npos) << High;
      EXPECT_EQ(LLVM.find("__builtin_trap"), std::string::npos) << LLVM;
      compileRoutes(I, NoOpt, Entry + 5, "encls", std::nullopt, false,
                    A == Arch::X64);
    }
}

namespace {
std::vector<uint8_t> segmentedBlockFunction(unsigned Kind, unsigned Segment,
                                            bool Address16, size_t &Offset) {
  // Two cdecl scalar offsets. Preserve callee-saved ESI/EDI and keep the flag
  // result observable without a synthetic pointer dereference in the fixture.
  std::vector<uint8_t> Bytes = {0x56, 0x57, 0x8b, 0x74, 0x24, 0x0c, 0x8b,
                                0x7c, 0x24, 0x10, 0xb8, 0,    0,    0,
                                0,    0x83, 0xc0, 0,    0xf9};
  Offset = Bytes.size();
  const uint8_t Segments[] = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65};
  Bytes.push_back(Segments[Segment]);
  if (Address16)
    Bytes.push_back(0x67);
  Bytes.insert(Bytes.end(), {uint8_t(Kind == 0   ? 0x66
                                     : Kind == 1 ? 0xf2
                                                 : 0xf3),
                             0x0f, 0x38, 0xf8, uint8_t(Address16 ? 0x3c : 0x3e),
                             0x9f, 0x0f, 0xb6, 0xc4, 0x5f, 0x5e, 0xc3});
  return Bytes;
}

std::string segmentedBlockDriver(const BinaryImage &Image, unsigned Kind,
                                 unsigned Segment, bool Address16) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "#define KIND " << Kind << "\n#define SEGMENT " << Segment
     << "\n#define ADDRESS16 " << unsigned(Address16) << "\n";
  OS << R"C(
extern unsigned virtualization_test(unsigned, unsigned);
extern unsigned neverd_original(unsigned, unsigned);
static unsigned char source[256] __attribute__((aligned(64)));
static unsigned char destination[256] __attribute__((aligned(64)));
static unsigned char expected[256];
static int sys(int n, unsigned a, unsigned b, unsigned c) {
  int r;
  __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory", "cc");
  return r;
}
static void done(int code) {
  sys(1, code, 0, 0);
  __builtin_unreachable();
}
struct descriptor { unsigned entry, base, limit, flags; };
static unsigned invoke(unsigned (*fn)(unsigned, unsigned), int mode) {
  struct descriptor es = {0, (unsigned)destination, mode == 1 ? 63u : 255u,
                           mode == 2 ? 0x49u : 0x41u};
  struct descriptor fs = {1, (unsigned)source, mode == 4 ? 127u : 255u, 0x41};
  if (sys(123, 1, (unsigned)&es, sizeof(es)) ||
      sys(123, 1, (unsigned)&fs, sizeof(fs))) done(90);
  unsigned short oldes, oldfs;
  __asm__ volatile("movw %%es,%0; movw %%fs,%1" : "=r"(oldes), "=r"(oldfs));
  __asm__ volatile("movw %0,%%es; movw %1,%%fs" : : "r"((unsigned short)7), "r"((unsigned short)15) : "memory");
  unsigned src = SEGMENT == 3 ? (unsigned)(source + 128) : 128;
  unsigned dst = mode == 3 ? 65 : 64;
  if (ADDRESS16) { src |= 0xa5a50000u; dst |= 0x5a5a0000u; }
  unsigned result = fn(src, dst);
  __asm__ volatile("sfence; movw %0,%%es; movw %1,%%fs" : : "r"(oldes), "r"(oldfs) : "memory");
  return result;
}
static void init(void) {
  for (unsigned i = 0; i != 256; ++i) {
    source[i] = (unsigned char)(i * 13 + 7);
    destination[i] = (unsigned char)(i * 17 + 5);
  }
}
static int status(unsigned (*fn)(unsigned, unsigned), int mode) {
  int pid = sys(2, 0, 0, 0);
  if (pid < 0) done(91);
  if (!pid) {
    sys(172, 4, 0, 0); // PR_SET_DUMPABLE=0: no host core collector.
    invoke(fn, mode);
    done(0);
  }
  int result = -1;
  if (sys(7, pid, (unsigned)&result, 0) != pid) done(92);
  return result;
}
void _start(void) {
  init();
  if (KIND == 0) {
    unsigned a = invoke(neverd_original, 0);
    for (unsigned i = 0; i != 256; ++i) expected[i] = destination[i];
    // Independent successful transaction check, beyond agreement of routes.
    for (unsigned i = 0; i != 64; ++i) {
      unsigned char value = SEGMENT == 0 ? (unsigned char)((128+i)*17+5) : source[128+i];
      if (destination[64+i] != value) done(11);
    }
    init();
    unsigned b = invoke(virtualization_test, 0);
    if (a != 0x47 || b != a) done(12);
    for (unsigned i = 0; i != 256; ++i)
      if (destination[i] != expected[i]) done(13);
  }
  for (int mode = KIND ? 0 : 1; mode != 5; ++mode) {
    if (mode == 4 && SEGMENT != 4) continue;
    int a = status(neverd_original, mode);
    int b = status(virtualization_test, mode);
    if ((a & 127) == 0 || a != b) done(20 + mode);
  }
  done(0);
}
__asm__(".text\n.globl neverd_original\nneverd_original:\n"
)C";
  for (uint8_t Byte : Image.Segments.front().Data)
    OS << "\".byte " << unsigned(Byte) << "\\n\"\n";
  OS << ");\n";
  return Text;
}
} // namespace

TEST(X86SegmentedBlockSource, AllSegmentsAndWidthsCompileAcrossNativeFormats) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (unsigned Kind = 0; Kind != 3; ++Kind)
      for (unsigned Segment = 0; Segment != 6; ++Segment)
        for (bool Address16 : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(Kind);
          SCOPED_TRACE(Segment);
          SCOPED_TRACE(Address16);
          size_t Offset;
          auto Image =
              image(Arch::X86, Format,
                    segmentedBlockFunction(Kind, Segment, Address16, Offset));
          compileRoutes(Image, false, Entry + Offset,
                        Kind == 0   ? "movdir64b"
                        : Kind == 1 ? "enqcmd"
                                    : "enqcmds",
                        {}, false, false, {}, {}, 4, true);
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86SegmentedBlockSource,
     NonflatSegmentsAndNativeFaultsMatchOriginalBytes) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "Requires Linux i386 compatibility execution";
#else
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("movdir64b"))
    GTEST_SKIP() << "MOVDIR64B is unavailable";
  for (unsigned Kind = 0; Kind != 3; ++Kind)
    for (unsigned Segment : {0, 3, 4})
      for (bool Address16 : {false, true})
        for (bool NoOpt : {false, true}) {
          if (Segment == 3 && Address16)
            continue; // DS is the process flat segment.
          SCOPED_TRACE(Kind);
          SCOPED_TRACE(Segment);
          SCOPED_TRACE(Address16);
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Image =
              image(Arch::X86, BinaryFormat::ELF,
                    segmentedBlockFunction(Kind, Segment, Address16, Offset));
          const auto Driver =
              segmentedBlockDriver(Image, Kind, Segment, Address16);
          compileRoutes(Image, NoOpt, Entry + Offset,
                        Kind == 0   ? "movdir64b"
                        : Kind == 1 ? "enqcmd"
                                    : "enqcmds",
                        {}, false, false, Driver, {}, 4);
          if (::testing::Test::HasFailure())
            return;
        }
#endif
}

TEST(X86SegmentedBlockSource, MalformedAssemblyCannotEraseSegmentOrEffects) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    const auto Selector = x86SegmentedBlockSelector(
        static_cast<X86SegmentedBlockKind>(Kind), X86BlockSegment::FS, true);
    const auto P =
        x86SystemStateAsm(Intrinsic::X86SegmentedBlockMemory, Arch::X86, 0,
                          NdMemoryAddressSpace::Default, Selector);
    ASSERT_TRUE(P);
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(Kind);
      SCOPED_TRACE(Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("segmented", Context);
      llvm::IRBuilder<> B(Context);
      auto *Result = Kind ? B.getInt8Ty() : B.getVoidTy();
      auto *Type = llvm::FunctionType::get(
          Result, {B.getInt32Ty(), B.getInt32Ty()}, false);
      auto *Function = llvm::Function::Create(
          Type, llvm::GlobalValue::ExternalLinkage, "test", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      auto Text = P->Text;
      auto Constraints = P->Constraints;
      if (Mutation == 1)
        Constraints.erase(Constraints.find(",~{memory}"));
      if (Mutation == 2)
        Text.replace(Text.find("%fs:"), 4, "%ds:");
      // DS is also a valid segment. Breaking the declared width/constraints
      // instead of merely choosing another valid transaction must fail.
      if (Mutation == 2)
        Constraints.replace(Constraints.find("{si}"), 4, "r");
      if (Mutation == 3)
        Text.erase(Text.find("%fs:"), 4);
      auto *Asm = llvm::InlineAsm::get(
          Type, Text, Constraints, Mutation != 4, Mutation == 5,
          Mutation == 6 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
      auto *Call =
          B.CreateCall(Asm, {Function->getArg(0), Function->getArg(1)});
      if (Mutation == 7)
        Call->setOnlyReadsMemory();
      if (Mutation == 8)
        Call->setDoesNotReturn();
      if (Kind)
        B.CreateRet(Call);
      else
        B.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Arch::X86;
      Options.EmitIncludes = false;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      if (!Mutation)
        EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Source;
      else
        EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                     std::invalid_argument);
    }
  }
}

TEST(X86SegmentedBlockSource, ImageShapedOffsetsRemainNumeric) {
  for (bool NoOpt : {false, true}) {
    // The offset equals this image's named entry. It belongs to DS/ES, so
    // neither operand may become the address of the replacement C function.
    std::vector<uint8_t> Bytes = {
        0x57, 0xbf, 0x00, 0x10, 0,    0,    0xb8, 0,    0,    0,    0,
        0x83, 0xc0, 0,    0xf9, 0x3e, 0x66, 0x0f, 0x38, 0xf8, 0x3d, 0,
        0x10, 0,    0,    0x9f, 0x0f, 0xb6, 0xc4, 0x5f, 0xc3};
    auto Image = image(Arch::X86, BinaryFormat::ELF, Bytes);
    std::string High, LLVM, Native;
    sourceRoutes(Image, NoOpt, Entry + 15, "movdir64b", &High, &LLVM, {}, {},
                 &Native);
    if (::testing::Test::HasFailure())
      return;
    for (const auto *S : {&High, &LLVM}) {
      const auto Start = S->find("__asm__ volatile");
      ASSERT_NE(Start, std::string::npos);
      const auto End = S->find(';', Start);
      EXPECT_EQ(S->substr(Start, End - Start).find("virtualization_test"),
                std::string::npos)
          << *S;
    }
    EXPECT_EQ(Native.find("ptrtoint (ptr @virtualization_test"),
              std::string::npos)
        << Native;
    compileRoutes(Image, NoOpt, Entry + 15, "movdir64b");
    if (::testing::Test::HasFailure())
      return;
  }
}

namespace {
std::vector<uint8_t> amxFunction(const AmxEncoding &E, size_t &Offset) {
  // A zero palette is a valid LDTILECFG input. Stack storage also makes
  // STTILECFG independent of the test process's ambient address layout.
  std::vector<uint8_t> Bytes = {0x48, 0x83, 0xec, 0x40};
  for (unsigned I = 0; I != 8; ++I)
    Bytes.insert(Bytes.end(), {0x48, 0xc7, 0x44, 0x24,
                               static_cast<uint8_t>(8 * I), 0, 0, 0, 0});
  const bool Row = E.Kind >= 22;
  Bytes.insert(Bytes.end(), {0xb8, 0, 0, 0, 0, 0xb9, 0x40, 0, 0, 0});
  if (!Row)
    Bytes.insert(Bytes.end(), {0x48, 0x89, 0xe0}); // mov rax,rsp.
  if (llvm::StringRef(E.Assembly).contains("%r16d"))
    Bytes.insert(Bytes.end(), {0xd5, 0x10, 0xb8, 0, 0, 0, 0});
  Offset = Bytes.size();
  Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
  if (Row && llvm::StringRef(E.Assembly).contains("%zmm17"))
    Bytes.insert(Bytes.end(), {0x62, 0xe1, 0xfd, 0x08, 0x7e, 0xc8});
  else if (Row && llvm::StringRef(E.Assembly).contains("%zmm31"))
    Bytes.insert(Bytes.end(), {0x62, 0x61, 0xfd, 0x08, 0x7e, 0xf8});
  else if (Row)
    Bytes.insert(Bytes.end(),
                 {0xc4, 0xe1, 0xf9, 0x7e, 0xc0}); // vmovq rax,xmm0.
  else
    Bytes.insert(Bytes.end(), {0xb8, 0, 0, 0, 0});
  Bytes.insert(Bytes.end(), {0x48, 0x83, 0xc4, 0x40, 0xc3});
  return Bytes;
}

std::string amxOutcomeDriver(const BinaryImage &Image) {
  // Compare completion as well as native exceptions: TILERELEASE and a zero
  // configuration can complete on AMX hardware even without tiled data use.
  std::string Source = R"C(
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
extern uint64_t virtualization_test(void), neverd_original(void);
static int output;
static void fault(int signal, siginfo_t *info, void *context) {
  (void)context;
  uint64_t result[3] = {(unsigned)signal, (unsigned)info->si_code, 0};
  _exit(write(output, result, sizeof(result)) != sizeof(result));
}
static int observe(uint64_t (*fn)(void), uint64_t result[3]) {
  int pipefd[2];
  if (pipe(pipefd)) return 1;
  pid_t child = fork();
  if (child < 0) return 2;
  if (!child) {
    close(pipefd[0]); output = pipefd[1];
    struct sigaction sa = {0};
    sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, 0) || sigaction(SIGSEGV, &sa, 0) ||
        sigaction(SIGBUS, &sa, 0)) _exit(3);
    uint64_t completed[3] = {0, 0, fn()};
    _exit(write(output, completed, sizeof(completed)) != sizeof(completed));
  }
  close(pipefd[1]);
  long bytes = read(pipefd[0], result, 3 * sizeof(uint64_t));
  close(pipefd[0]);
  int status;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
      WEXITSTATUS(status) || bytes != 3 * sizeof(uint64_t)) return 4;
  return 0;
}
int main(void) {
  uint64_t expected[3], actual[3];
  if (observe(neverd_original, expected) ||
      observe(virtualization_test, actual)) return 1;
  return expected[0] != actual[0] || expected[1] != actual[1] ||
         expected[2] != actual[2];
}
__asm__(".text\n.globl neverd_original\nneverd_original:\n"
)C";
  for (const auto Byte : Image.Segments.front().Data)
    Source += "\".byte " + std::to_string(Byte) + "\\n\"\n";
  return Source + ");\n";
}
} // namespace

TEST(X86AmxSource, EveryBankOperationAndEncodingCompilesAcrossNativeFormats) {
  for (const auto &E : amxEncodings())
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(E.Assembly);
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        size_t Offset;
        auto Image = image(Arch::X64, Format, amxFunction(E, Offset));
        const bool FixedBank =
            E.Kind == 2 || E.Kind == 3 || (E.Kind >= 9 && E.Kind < 22);
        compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                      false, false, {},
                      FixedBank ? E.Bytes : std::vector<uint8_t>{}, 0, true);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86AmxSource, NativeContractsRejectLostEffectsAndControls) {
  if (!llvm::Intrinsic::lookupIntrinsicID("llvm.x86.ldtilecfg.address"))
    GTEST_SKIP() << "linked SDK predates explicit AMX controls";
  for (unsigned K = 0; K != unsigned(X86AmxOp::Count); ++K)
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(K);
      SCOPED_TRACE(Mutation);
      const auto Kind = X86AmxOp(K);
      uint32_t D = X86AmxStateVersion | K;
      if (x86AmxIsCompute(Kind))
        D |= (1u << 8) | (2u << 11);
      llvm::LLVMContext Context;
      llvm::Module Module("amx-native", Context);
      llvm::IRBuilder<> B(Context);
      const auto ID = llvm::Intrinsic::lookupIntrinsicID("llvm.x86." +
                                                         x86AmxNativeSuffix(D));
      ASSERT_NE(ID, 0u);
      auto *F = llvm::Intrinsic::getOrInsertDeclaration(&Module, ID);
      auto *Owner = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), {B.getInt32Ty()}, false),
          llvm::GlobalValue::ExternalLinkage, "test", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Owner));
      std::vector<llvm::Value *> Values;
      for (auto *T : F->getFunctionType()->params())
        Values.push_back(llvm::Constant::getNullValue(T));
      if (x86AmxIsCompute(Kind)) {
        Values[1] = B.getInt8(1);
        Values[2] = B.getInt8(2);
      }
      const unsigned Width = x86AmxIsTileMemory(Kind) ? 3 : 1;
      if (x86AmxIsMemory(Kind))
        Values[Width] = B.getInt32(64);
      auto *Call = B.CreateCall(F, Values);
      bool Invalid = Mutation >= 1 && Mutation <= 3;
      if (Mutation == 1)
        Call->setOnlyReadsMemory();
      if (Mutation == 2)
        Call->setOnlyWritesMemory();
      if (Mutation == 3)
        Call->setDoesNotReturn();
      if (Mutation >= 4 && x86AmxIsMemory(Kind)) {
        Invalid = true;
        if (Mutation == 4)
          Call->setArgOperand(Width, B.getInt32(16));
        if (Mutation == 5)
          Call->setArgOperand(Width + 1, B.getInt32(1));
        if (Mutation == 6)
          Call->setArgOperand(Width + 2, B.getInt32(2));
        if (Mutation == 7)
          Call->setArgOperand(Width, Owner->getArg(0));
      } else if (Mutation >= 4 && x86AmxIsRow(Kind)) {
        Invalid = true;
        if (Mutation == 4)
          Call->setArgOperand(0, B.getInt8(8));
        else if (Mutation == 5)
          Call->setArgOperand(2, B.getInt32(2));
        else
          Call->setArgOperand(2, Owner->getArg(0));
      } else if (Mutation >= 4 && x86AmxIsCompute(Kind)) {
        Invalid = true;
        Call->setArgOperand(1, B.getInt8(Mutation == 4 ? 8 : 0));
      }
      const auto Args = std::vector<std::string>(Values.size(), "input");
      if (Invalid)
        EXPECT_THROW(renderX86AmxCall(Arch::X64, *Call, Args),
                     std::invalid_argument);
      else
        EXPECT_TRUE(renderX86AmxCall(Arch::X64, *Call, Args));
    }
}

TEST(X86AmxSource, NeverCCompilesBothRoutesWithoutAssembly) {
  const char *Compiler = std::getenv("NEVERD_TEST_NEVERC");
  if (!Compiler || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC to the qualified NeverC compiler";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-amx-native-c", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  unsigned Cases = 0;
  for (const auto &E : amxEncodings())
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(E.Assembly);
        SCOPED_TRACE(static_cast<unsigned>(Format));
        size_t Offset;
        auto Image = image(Arch::X64, Format, amxFunction(E, Offset));
        std::string High, LLVM, Native;
        sourceRoutes(Image, NoOpt, Entry + Offset, {}, &High, &LLVM, {}, {},
                     &Native);
        if (::testing::Test::HasFailure())
          return;
        uint32_t D = X86AmxStateVersion | E.Kind;
        if (x86AmxIsCompute(X86AmxOp(E.Kind)))
          D |= (1u << 8) | (2u << 11);
        if (x86AmxIsRow(X86AmxOp(E.Kind)) &&
            llvm::StringRef(E.Assembly).contains('$'))
          D |= X86AmxImmediateRow;
        const auto Name = "llvm.x86." + x86AmxNativeSuffix(D);
        if (llvm::Intrinsic::lookupIntrinsicID("llvm.x86.ldtilecfg.address")) {
          EXPECT_NE(Native.find("@" + Name + "("), std::string::npos) << Native;
          EXPECT_EQ(Native.find(" asm "), std::string::npos) << Native;
        }
        for (const auto &Source : {High, LLVM}) {
          compileNativeC(Compiler, Directory, Format, Source, Name, Cases,
                         std::nullopt, true);
          if (::testing::Test::HasFailure())
            return;
        }
      }
  EXPECT_EQ(Cases, amxEncodings().size() * 24);
  RecordProperty("amx_native_c_compilations", Cases);
  RecordProperty("amx_native_c_object_compilations", Cases);
}

TEST(X86AmxSource, NativeCompletionAndFaultsMatchOriginalBytes) {
  for (const auto &E : amxEncodings())
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(E.Assembly);
      SCOPED_TRACE(NoOpt);
      size_t Offset;
      auto Image = image(Arch::X64, BinaryFormat::ELF, amxFunction(E, Offset));
      compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                    false, false, amxOutcomeDriver(Image));
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86AmxSource, MalformedAssemblyCannotDropBankMemoryOrFeatureEffects) {
  for (const auto Descriptor :
       {X86AmxStateVersion | unsigned(X86AmxOp::Release),
        X86AmxStateVersion | unsigned(X86AmxOp::Load) | X86AmxEvexMemory,
        X86AmxStateVersion | unsigned(X86AmxOp::MoveRow) | X86AmxImmediateRow,
        X86AmxStateVersion | unsigned(X86AmxOp::MoveRow) | X86AmxExtendedRow}) {
    const auto P = x86AmxAsm(Descriptor, NdMemoryAddressSpace::Default);
    ASSERT_TRUE(P);
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Descriptor);
      SCOPED_TRACE(Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("amx", Context);
      llvm::IRBuilder<> B(Context);
      std::vector<llvm::Type *> Types;
      std::vector<llvm::Value *> Values;
      for (unsigned I = 0; I < P->InputBytes.size(); ++I) {
        llvm::Type *T = B.getIntNTy(8 * P->InputBytes[I]);
        if (P->Row && I == 0)
          T = B.getPtrTy();
        Types.push_back(T);
        Values.push_back(llvm::Constant::getNullValue(T));
      }
      auto *Type = llvm::FunctionType::get(B.getVoidTy(), Types, false);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), false),
          llvm::GlobalValue::ExternalLinkage, "test", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      auto Text = P->Text;
      auto Constraints = P->Constraints;
      if (Mutation == 1)
        Constraints.erase(Constraints.find(",~{memory}"));
      if (Mutation == 2)
        Constraints.erase(Constraints.find("~{tmm7}"), 8);
      if (Mutation == 3)
        Text += "\n\tnop";
      auto *Asm = llvm::InlineAsm::get(
          Type, Text, Constraints, Mutation != 4, Mutation == 5,
          Mutation == 6 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
      auto *Call = B.CreateCall(Asm, Values);
      if (Mutation == 7)
        Call->setOnlyReadsMemory();
      if (Mutation == 8)
        Call->setOnlyWritesMemory();
      if (Mutation == 9)
        Call->setDoesNotReturn();
      B.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.EmitIncludes = false;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      if (!Mutation)
        EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Source;
      else
        EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                     std::invalid_argument);
    }
  }
}

namespace {
std::vector<uint8_t> amxTransferFunction() {
  // SysV: config, source, byte stride, destination, completed config.
  // No explicit tile values cross the ordinary function ABI.
  return {0xc4, 0xe2, 0x78, 0x49, 0x07,       // ldtilecfg [rdi].
          0xc4, 0xe2, 0x7b, 0x4b, 0x04, 0x16, // tileloadd tmm0,[rsi+rdx].
          0xb8, 0x40, 0,    0,    0,          // stride for packed output.
          0xc4, 0xe2, 0x7a, 0x4b, 0x04, 0x01, // tilestored [rcx+rax],tmm0.
          0xc4, 0xc2, 0x79, 0x49, 0x00,       // sttilecfg [r8].
          0xc4, 0xe2, 0x78, 0x49, 0xc0, 0xc3};
}

std::string amxRestartDriver(const BinaryImage &Image, bool Execute) {
  std::string Source = R"C(
#define _GNU_SOURCE
#include <cpuid.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
typedef void (*transfer)(void *, void *, uint64_t, void *, void *);
extern void virtualization_test(void *, void *, uint64_t, void *, void *);
extern void neverd_original(void *, void *, uint64_t, void *, void *);
static unsigned char *source;
static unsigned page, config_offset, tile_offset;
static volatile sig_atomic_t faults;
static void resume(int signal, siginfo_t *info, void *context) {
  ucontext_t *uc = context;
  const unsigned char *state = (const unsigned char *)uc->uc_mcontext.fpregs;
  if (signal != SIGSEGV || info->si_addr != source + page || faults++)
    _exit(10);
  // Linux signal frames use standard XSAVE offsets, not compacted offsets.
  // Check the published length before reading either extension component.
  uint32_t magic, length;
  uint64_t present, compacted;
  memcpy(&magic, state + 464, 4); memcpy(&length, state + 480, 4);
  memcpy(&present, state + 512, 8); memcpy(&compacted, state + 520, 8);
  if (magic != 0x46505853 || length < tile_offset + 1024 ||
      (compacted >> 63) || (present & (3ULL << 17)) != (3ULL << 17) ||
      state[config_offset] != 1 || state[config_offset + 1] != 1)
    _exit(11);
  for (unsigned i = 0; i != 64; ++i)
    if (state[tile_offset + i] != source[i]) _exit(12);
  if (syscall(SYS_mprotect, source + page, page, PROT_READ | PROT_WRITE))
    _exit(13);
  // sigreturn restores TILECFG.start_row and TMM0; retry the same instruction.
}
static int check(transfer fn, int restart) {
  _Alignas(64) unsigned char config[64] = {0}, completed[64] = {0};
  unsigned char destination[128];
  memset(destination, 0xa5, sizeof(destination));
  config[0] = 1; config[16] = 64; config[48] = 2;
  faults = 0;
  if (mprotect(source + page, page, restart ? PROT_NONE : PROT_READ | PROT_WRITE))
    return 20;
  fn(config, source, page, destination, completed);
  if (faults != restart || memcmp(config, completed, 64)) return 21;
  if (memcmp(destination, source, 64) ||
      memcmp(destination + 64, source + page, 64)) return 22;
  return 0;
}
static int run(void) {
  if (syscall(SYS_arch_prctl, 0x1023, 18)) return 30;
  unsigned a, b, c, d;
  __cpuid_count(13, 17, a, b, c, d); config_offset = b;
  __cpuid_count(13, 18, a, b, c, d); tile_offset = b;
  page = (unsigned)sysconf(_SC_PAGESIZE);
  source = mmap(0, 2 * page, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (source == MAP_FAILED) return 31;
  for (unsigned i = 0; i != 64; ++i) {
    source[i] = (unsigned char)(3 * i + 7);
    source[page + i] = (unsigned char)(5 * i + 11);
  }
  struct sigaction sa = {0};
  sa.sa_sigaction = resume; sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, 0)) return 32;
  for (int restart = 0; restart != 2; ++restart) {
    int status = check(neverd_original, restart);
    if (status) return status;
    status = check(virtualization_test, restart);
    if (status) return status;
  }
  return munmap(source, 2 * page) != 0;
}
)C";
  Source += Execute ? "int main(void) { return run(); }\n"
                    : "int main(void) { return 0; }\n";
  Source +=
      "__asm__(\".text\\n.globl neverd_original\\nneverd_original:\\n\"\n";
  for (const auto Byte : Image.Segments.front().Data)
    Source += "\".byte " + std::to_string(Byte) + "\\n\"\n";
  return Source + ");\n";
}
} // namespace

TEST(X86AmxSource, CompleteTransferSequenceAndRestartDriverCompile) {
  for (bool NoOpt : {false, true}) {
    auto Image = image(Arch::X64, BinaryFormat::ELF, amxTransferFunction());
    // Also compile/link the signal-context oracle on hosts without AMX.
    compileRoutes(Image, NoOpt, Entry + 5, "tileloadd", std::nullopt, false,
                  false, amxRestartDriver(Image, false));
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(X86AmxSource, NativeTileRowsAndFaultContinuationMatchOriginalBytes) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "AMX signal continuation requires Linux x86-64";
#else
  const auto Features = llvm::sys::getHostCPUFeatures();
  const auto Tile = Features.find("amx-tile");
  if (Tile == Features.end() || !Tile->second)
    GTEST_SKIP()
        << "Host does not provide AMX-TILE; native restart unqualified";
  for (bool NoOpt : {false, true}) {
    auto Image = image(Arch::X64, BinaryFormat::ELF, amxTransferFunction());
    compileRoutes(Image, NoOpt, Entry + 5, "tileloadd", std::nullopt, false,
                  false, amxRestartDriver(Image, true));
    if (::testing::Test::HasFailure())
      return;
  }
#endif
}

TEST(X86AmxSource, HighCRejectsMalformedBankOwnership) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Call = HighExpr::makeCall(
        "x86_amx_state", 0,
        {HighExpr::makeConst(0, 8),
         HighExpr::makeConst(X86AmxStateVersion | unsigned(X86AmxOp::Release),
                             4)});
    Call->IntrinsicId = Intrinsic::X86AMXState;
    Call->Type = NdType::makeVoid();
    switch (Mutation) {
    case 1:
      Call->Operands[1]->ConstVal &= ~X86AmxStateVersion;
      break;
    case 2:
      Call->Operands[1]->ConstVal |= X86AmxAddress32;
      break;
    case 3:
      Call->Operands[0]->ConstVal = 1;
      break;
    case 4:
      Call->Operands[1]->Type = NdType::makeInt(8, false);
      break;
    case 5:
      Call->Operands.pop_back();
      break;
    case 6:
      Call->Type = NdType::makeInt(64, false);
      break;
    case 7:
      Call->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 8:
      Call->DoesNotReturn = true;
      break;
    case 9:
      Call->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    default:
      break;
    }
    HighStmt Statement;
    Statement.Kind = StmtKind::Call;
    Statement.CallExpr = Call;
    HighFunc Function;
    Function.Entry = Entry;
    Function.Name = "test";
    Function.ReturnType = NdType::makeVoid();
    Function.ReturnsNoValue = true;
    Function.Body.push_back(std::move(Statement));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::ELF;
    Options.EmitIncludes = false;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    if (!Mutation)
      EXPECT_TRUE(HighCEmitter().emit({Function}, OS, Options)) << Source;
    else
      EXPECT_THROW(HighCEmitter().emit({Function}, OS, Options),
                   std::invalid_argument);
  }
}
