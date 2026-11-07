//===- X86AMXAddressTests.cpp - AMX address provenance -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish the encoded segment prefix from native stack-fault provenance.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86AmxState.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86AmxAsm.h"
#include "neverd/backend/llvm/X86/X86AmxNative.h"
#include "neverd/backend/llvm/X86/X86AmxStackLeaf.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t kEntry = 0x1000;

int decode(Decoder &D, unsigned Route, const std::vector<uint8_t> &Bytes,
           DecodedInsn &Insn) {
  if (Route == 0)
    return D.decodeOne(Bytes.data(), Bytes.size(), kEntry, Insn);
  if (Route == 1)
    return D.decodeOneLight(Bytes.data(), Bytes.size(), kEntry, Insn);
  return D.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn);
}

TEST(X86AMXAddress, ConfigPrefixesKeepIndependentDefaultStackProvenance) {
  // Independently encoded RAX, RSP, RBP, R12 and R13 operands. R12/R13 use
  // DS-default references despite sharing low ModRM bits with SP/BP.
  const std::array<std::vector<uint8_t>, 5> Forms = {{
      {0xc4, 0xe2, 0x78, 0x49, 0x00},
      {0xc4, 0xe2, 0x78, 0x49, 0x04, 0x24},
      {0xc4, 0xe2, 0x78, 0x49, 0x45, 0x00},
      {0xc4, 0xc2, 0x78, 0x49, 0x04, 0x24},
      {0xc4, 0xc2, 0x78, 0x49, 0x45, 0x00},
  }};
  for (bool Store : {false, true})
    for (unsigned Base = 0; Base != Forms.size(); ++Base)
      for (bool Address32 : {false, true})
        for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
          for (unsigned Route : {0u, 1u, 2u}) {
            auto Bytes = Forms[Base];
            Bytes[2] += Store;
            if (Address32)
              Bytes.insert(Bytes.begin(), 0x67);
            if (Prefix)
              Bytes.insert(Bytes.begin(), Prefix);
            SCOPED_TRACE(testing::PrintToString(Bytes));
            SCOPED_TRACE(Route);
            Decoder D;
            ASSERT_TRUE(D.init(Arch::X64));
            D.setStrict(true);
            DecodedInsn Insn{};
            ASSERT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
            std::vector<LowOp> Ops;
            ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
            unsigned Effects = 0;
            for (const auto &Op : Ops) {
              if (Op.Opcode != NdOp::INTRINSIC)
                continue;
              ++Effects;
              const auto S = x86AmxLowShape(Op, Arch::X64);
              ASSERT_TRUE(x86AmxShapeIsValid(S));
              const uint32_t Descriptor = *S.Descriptor;
              EXPECT_EQ(Descriptor & X86AmxVersionMask, X86AmxStateVersion);
              EXPECT_EQ(bool(Descriptor & X86AmxStackSegment), Prefix == 0x36);
              EXPECT_EQ(bool(Descriptor & X86AmxStackReference),
                        !Address32 && (Base == 1 || Base == 2) &&
                            Prefix != 0x64 && Prefix != 0x65);
            }
            EXPECT_EQ(Effects, 1u);
          }
}

TEST(X86AMXAddress, StackStridesKeepIndependentBaseAndStride) {
  for (const auto Control : {std::array<uint8_t, 2>{0x7b, 0x4b},
                             {0x79, 0x4b},
                             {0x7b, 0x4a},
                             {0x79, 0x4a},
                             {0x7a, 0x4b}})
    for (bool FrameBase : {false, true})
      for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e})
        for (bool Strict : {false, true})
          for (unsigned Route : {0u, 1u, 2u}) {
            std::vector<uint8_t> Bytes = {0xc4,
                                          0xe2,
                                          Control[0],
                                          Control[1],
                                          uint8_t(FrameBase ? 0x44 : 0x04),
                                          uint8_t(FrameBase ? 0x0d : 0x0c)};
            if (FrameBase)
              Bytes.push_back(0);
            if (Prefix)
              Bytes.insert(Bytes.begin(), Prefix);
            SCOPED_TRACE(testing::PrintToString(Bytes));
            Decoder D;
            ASSERT_TRUE(D.init(Arch::X64));
            D.setStrict(Strict);
            DecodedInsn Insn{};
            ASSERT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
            std::vector<LowOp> Ops;
            ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
            unsigned Effects = 0;
            for (const auto &Op : Ops) {
              if (Op.Opcode != NdOp::INTRINSIC)
                continue;
              ++Effects;
              ASSERT_EQ(Op.NumInputs, 4u);
              EXPECT_EQ(Op.Inputs[0].Offset, unsigned(Intrinsic::X86AMXState));
              const auto Descriptor = uint32_t(Op.Inputs[2].Offset);
              EXPECT_TRUE(x86AmxNeedsStackLeaf(Descriptor));
              EXPECT_EQ(bool(Descriptor & X86AmxStackSegment), Prefix == 0x36);
              EXPECT_EQ(Op.Inputs[3].Size, 8);
              EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default);
            }
            EXPECT_EQ(Effects, 1u);
          }
}

TEST(X86AMXAddress, LegacyAmbiguityIsNotAssignedNewFaultSemantics) {
  for (const auto K : {X86AmxOp::LoadConfig, X86AmxOp::StoreConfig,
                       X86AmxOp::Load, X86AmxOp::Store})
    for (uint32_t Flags : {0u, X86AmxStackSegment}) {
      const uint32_t Old = X86AmxLegacyStateVersion | unsigned(K) | Flags;
      EXPECT_TRUE(x86AmxDescriptorIsValid(Old));
      EXPECT_FALSE(x86AmxAsm(Old, NdMemoryAddressSpace::Default));
      EXPECT_TRUE(x86AmxAsm(Old | X86AmxAddress32,
                            NdMemoryAddressSpace::Default, true));
      if (!Flags)
        for (auto Space :
             {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS})
          EXPECT_TRUE(x86AmxAsm(Old, Space, true));
    }
  EXPECT_TRUE(x86AmxAsm(X86AmxLegacyStateVersion | unsigned(X86AmxOp::Release),
                        NdMemoryAddressSpace::Default, true));
}

TEST(X86AMXAddress, LegacyAssemblyReaderKeepsOnlyUnambiguousAddresses) {
  for (auto Space : {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
                     NdMemoryAddressSpace::X86GS})
    for (bool Address32 : {false, true}) {
      const uint32_t D = X86AmxLegacyStateVersion | unsigned(X86AmxOp::Load) |
                         (Address32 ? X86AmxAddress32 : 0);
      const bool Valid = Address32 || Space != NdMemoryAddressSpace::Default;
      // Construct the old wire form independently, including the unconstrained
      // inputs. A recognized old marker must not escape to generic asm output.
      const auto Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                           : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                                                                  : "%ds:";
      const auto Text =
          std::string(X86AmxLegacyAsmMarker) + std::to_string(D) + " " +
          std::to_string(unsigned(Space)) + "\n\ttileloadd " + Segment +
          (Address32 ? "(${0:k},${1:k})" : "(${0:q},${1:q})") + ", %tmm0";
      std::string Constraints = "r,r";
      for (unsigned I = 0; I != 8; ++I)
        Constraints += ",~{tmm" + std::to_string(I) + "}";
      Constraints += ",~{memory}";
      llvm::LLVMContext C;
      llvm::Module M("legacy-amx-address", C);
      llvm::IRBuilder<> B(C);
      auto *Word = B.getIntNTy(Address32 ? 32 : 64);
      auto *Type = llvm::FunctionType::get(B.getVoidTy(), {Word, Word}, false);
      auto *Asm = llvm::InlineAsm::get(Type, Text, Constraints, true);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), false),
          llvm::GlobalValue::ExternalLinkage, "probe", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto *Zero = llvm::ConstantInt::get(Word, 0);
      auto *Call = B.CreateCall(Asm, {Zero, Zero});
      B.CreateRetVoid();
      if (!Valid) {
        EXPECT_THROW(renderX86AmxCall(Arch::X64, *Call, {"address", "stride"}),
                     std::invalid_argument);
        continue;
      }
      const auto Rendered =
          renderX86AmxCall(Arch::X64, *Call, {"address", "stride"});
      ASSERT_TRUE(Rendered);
      EXPECT_NE(Rendered->find("\"a\""), std::string::npos);
      EXPECT_NE(Rendered->find("\"c\""), std::string::npos);
      EXPECT_NE(Rendered->find("AMX v2"), std::string::npos);
    }
}

TEST(X86AMXAddress, ConfigNativeAndSourceRoutesKeepTheStackControl) {
  for (auto K : {X86AmxOp::LoadConfig, X86AmxOp::StoreConfig})
    for (bool ExplicitSS : {false, true}) {
      const uint32_t D = X86AmxStateVersion | unsigned(K) |
                         X86AmxStackReference |
                         (ExplicitSS ? X86AmxStackSegment : 0);
      const auto Plan = x86AmxAsm(D, NdMemoryAddressSpace::Default);
      ASSERT_TRUE(Plan);
      EXPECT_NE(Plan->Text.find("subq %rbp, %rdx"), std::string::npos);
      EXPECT_NE(Plan->Text.find("(%rbp,%rdx)"), std::string::npos);
      EXPECT_NE(Plan->Constraints.find("{ax},~{rdx},~{flags}"),
                std::string::npos);
      EXPECT_EQ(x86AmxNativePrefix(D, NdMemoryAddressSpace::Default),
                0x100u | (ExplicitSS ? 0x36u : 0x3eu));

      auto High = HighExpr::makeCall(
          "x86_amx_state", 0,
          {HighExpr::makeConst(0, 8), HighExpr::makeConst(D, 4)});
      High->IntrinsicId = Intrinsic::X86AMXState;
      High->Type = NdType::makeVoid();
      const auto Source = renderX86AmxExpression(
          Arch::X64, *High, {"address", "control"}, "void");
      EXPECT_NE(Source.find("NEVERD_X86_AMX_STACK("), std::string::npos);
      EXPECT_NE(Source.find("%%rbp,%%rdx"), std::string::npos);
      EXPECT_NE(Source.find("\"a\""), std::string::npos);

      MedFunc Function;
      Function.Entry = kEntry;
      Function.Name = "amx_stack";
      Function.CC = CallingConv::SysV_AMD64;
      Function.ReturnsNoValue = true;
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.addInput(MedVar::makeConst(unsigned(Intrinsic::X86AMXState), 2));
      Op.addInput(MedVar::makeConst(0xface000000001234ULL, 8));
      Op.addInput(MedVar::makeConst(D, 4));
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      MedBlock Block;
      Block.Id = 0;
      Block.Ops = {Op, Return};
      Function.Blocks.push_back(Block);
      ASSERT_TRUE(verifyMedFunc(Function, "AMX stack provenance"));
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, "amx-stack", Arch::X64);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      unsigned Effects = 0;
      for (const auto &F : *Module)
        for (const auto &B : F)
          for (const auto &I : B)
            if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
              ++Effects;
              std::vector<std::string> Args(Call->arg_size(), "input");
              const auto Rendered = renderX86AmxCall(Arch::X64, *Call, Args);
              ASSERT_TRUE(Rendered);
              EXPECT_NE(Rendered->find("NEVERD_X86_AMX_STACK("),
                        std::string::npos);
              if (const auto *Asm =
                      llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand()))
                EXPECT_EQ(Asm->getAsmString(), Plan->Text);
              else {
                ASSERT_EQ(Call->arg_size(), 4u);
                EXPECT_EQ(llvm::cast<llvm::ConstantInt>(Call->getArgOperand(2))
                              ->getZExtValue(),
                          x86AmxNativePrefix(D, NdMemoryAddressSpace::Default));
              }
            }
      EXPECT_EQ(Effects, 1u);
    }
}

TEST(X86AMXAddress, OwnedMemorySourcesUseQualifiedCarriers) {
  const char *Export = std::getenv("NEVERD_TEST_AMX_ADDRESS_EXPORT");
  std::ofstream Manifest;
  if (Export) {
    std::filesystem::create_directories(Export);
    Manifest.open(std::filesystem::path(Export) / "manifest.tsv");
    ASSERT_TRUE(Manifest);
  }
  for (auto K : {X86AmxOp::LoadConfig, X86AmxOp::StoreConfig, X86AmxOp::Load,
                 X86AmxOp::LoadT1, X86AmxOp::LoadRS, X86AmxOp::LoadRST1,
                 X86AmxOp::Store})
    for (bool Address32 : {false, true})
      for (bool Stack : {false, true})
        for (bool ExplicitSS : {false, true})
          for (bool Evex : {false, true}) {
            if (Stack && Address32)
              continue;
            const uint32_t D = X86AmxStateVersion | unsigned(K) |
                               (Address32 ? X86AmxAddress32 : 0) |
                               (Stack ? X86AmxStackReference : 0) |
                               (ExplicitSS ? X86AmxStackSegment : 0) |
                               (Evex ? X86AmxEvexMemory : 0);
            const auto Plan = x86AmxAsm(D, NdMemoryAddressSpace::Default);
            ASSERT_TRUE(Plan);
            EXPECT_EQ(Plan->InputConstraints[0],
                      Plan->StackLeaf ? "{bp}" : "{ax}");
            if (x86AmxIsTileMemory(K))
              EXPECT_EQ(Plan->InputConstraints[1], "{cx}");
            std::vector<ExprPtr> Operands{HighExpr::makeConst(0, 8),
                                          HighExpr::makeConst(D, 4)};
            std::vector<std::string> HighArgs{"address", "control"};
            std::vector<std::string> AsmArgs{"address"};
            if (x86AmxIsTileMemory(K)) {
              Operands.push_back(HighExpr::makeConst(0, 8));
              HighArgs.push_back("stride");
              AsmArgs.push_back("stride");
            }
            auto High = HighExpr::makeCall("x86_amx_state", 0, Operands);
            High->IntrinsicId = Intrinsic::X86AMXState;
            High->Type = NdType::makeVoid();
            const auto HighSource =
                renderX86AmxExpression(Arch::X64, *High, HighArgs, "void");

            llvm::LLVMContext Context;
            llvm::Module Module("amx-address-source", Context);
            llvm::IRBuilder<> B(Context);
            std::vector<llvm::Type *> Types;
            std::vector<llvm::Value *> Values;
            for (unsigned Bytes : Plan->InputBytes) {
              Types.push_back(B.getIntNTy(Bytes * 8));
              Values.push_back(llvm::Constant::getNullValue(Types.back()));
            }
            auto *Asm = llvm::InlineAsm::get(
                llvm::FunctionType::get(B.getVoidTy(), Types, false),
                Plan->Text, Plan->Constraints, true);
            auto *F = llvm::Function::Create(
                llvm::FunctionType::get(
                    B.getVoidTy(),
                    Plan->StackLeaf ? Types : std::vector<llvm::Type *>{},
                    false),
                Plan->StackLeaf ? llvm::GlobalValue::InternalLinkage
                                : llvm::GlobalValue::ExternalLinkage,
                "amx", Module);
            if (Plan->StackLeaf) {
              F->setCallingConv(llvm::CallingConv::PreserveAll);
              F->addFnAttr(X86AmxStackLeafAttribute);
              F->addFnAttr(llvm::Attribute::NoInline);
              F->addFnAttr("frame-pointer", "none");
              Values = {F->getArg(0), F->getArg(1)};
            }
            B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
            auto *Call = B.CreateCall(Asm, Values);
            B.CreateRetVoid();
            const auto LLVMSource = renderX86AmxCall(Arch::X64, *Call, AsmArgs);
            ASSERT_TRUE(LLVMSource);
            for (const auto &Source : {HighSource, *LLVMSource}) {
              if (Plan->StackLeaf) {
                EXPECT_NE(Source.find("NEVERD_X86_AMX_STACK_MEMORY("),
                          std::string::npos);
                EXPECT_EQ(Source.find("__asm__"), std::string::npos);
                continue;
              }
              EXPECT_NE(Source.find("\"a\""), std::string::npos);
              if (x86AmxIsTileMemory(K))
                EXPECT_NE(Source.find("\"c\""), std::string::npos);
            }
            if (!Export)
              continue;
            for (unsigned Route = 0; Route != 2; ++Route) {
              const auto Name = std::to_string(D) + "-" + std::to_string(Route);
              std::ofstream File(std::filesystem::path(Export) / (Name + ".c"));
              ASSERT_TRUE(File);
              File << R"(#include <stdint.h>
#if defined(NEVERD_AMX_USE_NATIVE)
#define NEVERD_X86_AMX(native, ...) native
#define NEVERD_X86_AMX_STACK(native, ...) native
#define NEVERD_X86_AMX_STACK_MEMORY(native) native
#else
#define NEVERD_X86_AMX(native, ...) __VA_ARGS__
#define NEVERD_X86_AMX_STACK(native, ...) __VA_ARGS__
#define NEVERD_X86_AMX_STACK_MEMORY(native) ({ _Static_assert(0, "native AMX stack-memory compiler required"); })
#endif
)";
              File << "__attribute__((target(\"" << x86AmxNativeFeatures(D)
                   << "\"), noinline))\n";
              File
                  << R"(uint64_t probe(uint64_t address, uint64_t stride, unsigned n) {
  _Alignas(64) volatile uint64_t fixed[8];
  volatile unsigned char *dynamic = __builtin_alloca(n);
  fixed[0] = UINT64_C(0x123456789abcdef0);
  dynamic[0] = 0xa5;
)";
              File << (Route ? *LLVMSource : HighSource) << ";\n"
                   << "  return fixed[0] ^ dynamic[0];\n}\n";
              Manifest << Name << '\t' << unsigned(K) << '\t'
                       << (Address32 ? 32 : 64) << '\t' << Stack << '\t'
                       << ExplicitSS << '\t' << Evex << '\n';
            }
          }
}

TEST(X86AMXAddress, StackRowsOwnAnIsolatedLeafAtTheLLVMAndCBoundaries) {
  for (auto K : {X86AmxOp::Load, X86AmxOp::LoadT1, X86AmxOp::LoadRS,
                 X86AmxOp::LoadRST1, X86AmxOp::Store}) {
    const uint32_t D = X86AmxStateVersion | unsigned(K) | X86AmxStackReference;
    MedFunc Function;
    Function.Entry = kEntry;
    Function.Name = "probe";
    Function.CC = CallingConv::SysV_AMD64;
    Function.ReturnsNoValue = true;
    MedOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.addInput(MedVar::makeConst(unsigned(Intrinsic::X86AMXState), 2));
    Op.addInput(MedVar::makeConst(0xface000000001234ULL, 8));
    Op.addInput(MedVar::makeConst(D, 4));
    Op.addInput(MedVar::makeConst(0x8765432100000040ULL, 8));
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    MedBlock Block;
    Block.Id = 0;
    Block.Ops = {Op, Return};
    Function.Blocks.push_back(Block);
    ASSERT_TRUE(verifyMedFunc(Function, "AMX stack leaf"));
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, "amx-stack", Arch::X64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    unsigned Effects = 0;
    for (auto &F : *Module)
      for (auto &BB : F)
        for (auto &I : BB) {
          auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
          if (!Call || (Call->getCalledFunction() &&
                        !Call->getCalledFunction()->isDeclaration()))
            continue;
          ++Effects;
          const std::vector<std::string> Args(Call->arg_size(), "input");
          auto Source = renderX86AmxCall(Arch::X64, *Call, Args);
          ASSERT_TRUE(Source);
          EXPECT_NE(Source->find("NEVERD_X86_AMX_STACK_MEMORY("),
                    std::string::npos);
          EXPECT_EQ(Source->find("__asm__"), std::string::npos);
          if (!llvm::isa<llvm::InlineAsm>(Call->getCalledOperand()))
            continue;
          EXPECT_TRUE(x86AmxIsStackLeaf(*Call));
          F.addFnAttr("frame-pointer", "all");
          EXPECT_THROW(renderX86AmxCall(Arch::X64, *Call, Args),
                       std::invalid_argument);
          F.addFnAttr("frame-pointer", "none");
          F.removeFnAttr(llvm::Attribute::NoInline);
          EXPECT_THROW(renderX86AmxCall(Arch::X64, *Call, Args),
                       std::invalid_argument);
          F.addFnAttr(llvm::Attribute::NoInline);
        }
    EXPECT_EQ(Effects, 1u);
    // IPO specializes a private leaf's constant inputs and removes its formal
    // parameters. Ownership belongs to its frame/body, not an unstable ABI.
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder PB;
    PB.registerLoopAnalyses(LAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerModuleAnalyses(MAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    auto Pipeline =
        PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    Pipeline.run(*Module, MAM);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Source;
    llvm::raw_string_ostream SourceStream(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, SourceStream, Options));
    EXPECT_NE(Source.find("NEVERD_X86_AMX_STACK_MEMORY("), std::string::npos);
    if (const char *Export = std::getenv("NEVERD_TEST_AMX_ADDRESS_EXPORT")) {
      std::filesystem::create_directories(Export);
      std::ofstream File(std::filesystem::path(Export) /
                         ("stack-leaf-" + std::to_string(unsigned(K)) + ".ll"));
      std::string IR;
      llvm::raw_string_ostream Stream(IR);
      Module->print(Stream, nullptr);
      File << IR;
      ASSERT_TRUE(File);
      std::ofstream CFile(std::filesystem::path(Export) /
                          ("stack-leaf-" + std::to_string(unsigned(K)) + ".c"));
      CFile << Source;
      ASSERT_TRUE(CFile);
    }
  }
}
} // namespace
