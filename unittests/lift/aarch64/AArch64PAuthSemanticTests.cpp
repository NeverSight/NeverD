//===- AArch64PAuthSemanticTests.cpp - PAC operand and failure contracts -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently assembled Armv8.3 words exercise both decoder routes and
/// distinguish signed pointers, raw pointers and authenticated control flow.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/AArch64/AArch64PAuthLoadC.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/AArch64/AArch64FastClassify.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.h"
#include "neverd/ir/intrinsics/AArch64/AArch64ExceptionReturn.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/MachO/MachOMetadataPointer.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <array>
#include <climits>
#include <functional>
#include <string>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/sysctl.h>
#endif

using namespace neverd;

namespace {
void optimize(llvm::Module &Module) {
  llvm::LoopAnalysisManager Loops;
  llvm::FunctionAnalysisManager Functions;
  llvm::CGSCCAnalysisManager CallGraph;
  llvm::ModuleAnalysisManager Modules;
  llvm::PassBuilder Passes;
  Passes.registerLoopAnalyses(Loops);
  Passes.registerFunctionAnalyses(Functions);
  Passes.registerCGSCCAnalyses(CallGraph);
  Passes.registerModuleAnalyses(Modules);
  Passes.crossRegisterProxies(Loops, Functions, CallGraph, Modules);
  auto Optimize =
      Passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  Optimize.run(Module, Modules);
}

std::vector<LowOp> lift(uint32_t Word, bool Fast) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::AArch64));
  Dec.setStrict(true);
  std::array<uint8_t, 4> Bytes;
  writeLE<uint32_t>(Bytes.data(), Word);
  DecodedInsn Insn;
  EXPECT_EQ(Fast ? Dec.decodeOneForLift(Bytes.data(), 4, 0x1000, Insn)
                 : Dec.decodeOne(Bytes.data(), 4, 0x1000, Insn),
            4);
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

void expectRegister(const NdVar &V, uint64_t Offset) {
  EXPECT_TRUE(V.isReg());
  EXPECT_EQ(V.Offset, Offset);
  EXPECT_EQ(V.Size, 8);
}

void expectIntrinsic(const LowOp &Op, Intrinsic Id, unsigned Inputs) {
  EXPECT_EQ(Op.Opcode, NdOp::INTRINSIC);
  ASSERT_EQ(Op.NumInputs, Inputs + 1);
  ASSERT_TRUE(Op.Inputs[0].isConst());
  EXPECT_EQ(Op.Inputs[0].Offset, static_cast<uint64_t>(Id));
}

void compileArm64eC(const std::string &Source,
                    const std::function<void(llvm::StringRef)> &Inspect = {},
                    bool EmitLLVM = false, llvm::StringRef ExecuteTarget = {},
                    bool *ExecutionAvailable = nullptr,
                    llvm::StringRef Language = "c") {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName("clang");
  if (!Found)
    GTEST_SKIP() << "clang is required for the arm64e source compilation check";
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<128> SourcePath, ObjectPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm64e", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm64e", "o", ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm64e", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code FileError;
  {
    llvm::raw_fd_ostream Out(SourcePath, FileError);
    ASSERT_FALSE(FileError);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  // Exclude host libc headers explicitly. Only Clang's resource headers and
  // the fixture's C declarations are needed, including on Linux and Windows.
  const bool ExecuteNative = !ExecuteTarget.empty();
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef> Args = {
        Compiler,
        "-target",
        ExecuteNative ? ExecuteTarget : "arm64e-apple-macos",
        "-ffreestanding",
        "-nostdlibinc",
        "-I",
        NEVERD_ARM64E_TEST_HEADERS,
        "-std=gnu11",
        "-Werror=unused-value",
        "-x",
        Language,
        Optimization,
        SourcePath,
        "-o",
        ObjectPath};
    if (EmitLLVM)
      Args.append({"-S", "-emit-llvm"});
    else if (ExecuteNative)
      Args.push_back("-march=armv8.3-a+pauth");
    else
      Args.push_back("-c");
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                                 Redirects, 30, 0, &Error);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "")
                         << Source;
    if (Inspect)
      Inspect(ObjectPath);
    if (ExecuteNative) {
      const llvm::StringRef RunArgs[] = {ObjectPath};
      const int Result = llvm::sys::ExecuteAndWait(
          ObjectPath, RunArgs, std::nullopt, Redirects, 30, 0, &Error);
      if (ExecutionAvailable) {
        *ExecutionAvailable = Result == 0;
        return;
      }
      EXPECT_EQ(Result, 0) << Error << Source;
    }
  }
}

MedFunc pointerFunction(Intrinsic Id, llvm::StringRef Name) {
  MedFunc F;
  F.Name = Name.str();
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8, false);
  const auto Checked = aarch64CPAOperation(Id);
  const auto TwoModifiers = aarch64PAuthExtendedOperation(Id);
  const unsigned Params = TwoModifiers ? TwoModifiers->operandCount()
                          : Checked    ? Checked->Sources
                                       : 2;
  for (unsigned Index = 0; Index < Params; ++Index) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.TheArch = Arch::AArch64;
    V.Id = Index;
    V.RegOff = Index * 8;
    V.Size = 8;
    F.Params.push_back(V);
  }
  MedOp Auth;
  Auth.Opcode = NdOp::INTRINSIC;
  Auth.Output.Kind = MedVar::Reg;
  Auth.Output.TheArch = Arch::AArch64;
  Auth.Output.Id = 2;
  Auth.Output.SSAVer = 1;
  Auth.Output.Size = 8;
  Auth.Output.RegOff = a64reg::X0;
  Auth.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), 4));
  Auth.addInput(F.Params[0]);
  if (Id == Intrinsic::A64_AuthBranch)
    Auth.addInput(MedVar::makeConst(1, 4));
  if (TwoModifiers || Checked || Id == Intrinsic::A64_AuthBranch ||
      Id == Intrinsic::A64_Pacga || !aarch64PAuthOperation(Id)->Strip)
    Auth.addInput(F.Params[1]);
  if (TwoModifiers || (Checked && Checked->Sources == 3))
    Auth.addInput(F.Params[2]);
  if (TwoModifiers && TwoModifiers->ConditionalPrefix)
    Auth.addInput(F.Params[3]);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Auth.Output);
  F.Blocks.emplace_back();
  F.Blocks[0].Id = 0;
  F.Blocks[0].StartAddr = F.Entry;
  F.Blocks[0].Ops = {Auth, Return};
  return F;
}
} // namespace

TEST(AArch64PAuthSemantics, CompilerObjCRecipesRetainLocalMetadataTargets) {
  const char Source[] = R"(
    __attribute__((objc_root_class))
    @interface ArmMetadata
    - (int)increment:(int)value;
    @end
    @implementation ArmMetadata
    - (int)increment:(int)value { return value + 1; }
    @end
  )";
  compileArm64eC(
      Source,
      [](llvm::StringRef Path) {
        auto Buffer = llvm::MemoryBuffer::getFile(Path);
        ASSERT_TRUE(Buffer);
        auto Image = MachOLoader().loadBuffer((*Buffer)->getMemBufferRef());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        EXPECT_TRUE(Image->isArm64e());
        EXPECT_FALSE(Image->MachOAuthenticatedPointers.empty());
        bool ClassRO = false, MetaclassRO = false, Implementation = false;
        for (const auto &[Slot, Recipe] : Image->MachOAuthenticatedPointers) {
          ASSERT_TRUE(Recipe.EncodedValue);
          EXPECT_EQ(readLE<uint64_t>(Image->readVA(Slot, 8)),
                    *Recipe.EncodedValue);
          EXPECT_FALSE(Image->CodePtrRelocSlots.count(Slot));
          EXPECT_EQ(macho_loader::readMetadataPointerTarget(*Image, Slot),
                    Recipe.Target);
          ClassRO |= Recipe.Symbol == "__OBJC_CLASS_RO_$_ArmMetadata";
          MetaclassRO |= Recipe.Symbol == "__OBJC_METACLASS_RO_$_ArmMetadata";
          if (Recipe.Symbol == "-[ArmMetadata increment:]") {
            Implementation = true;
            EXPECT_TRUE(Image->isCodeAddress(Recipe.Target));
          }
        }
        EXPECT_TRUE(ClassRO && MetaclassRO && Implementation);
        // Source type recovery still requires a linked image. Loading an
        // object's recipes does not prove its runtime class/dispatch layout.
        EXPECT_TRUE(Image->ObjCClasses.empty());
        EXPECT_TRUE(Image->ObjCMethods.empty());
#if defined(__APPLE__) && defined(__aarch64__)
        // Apple's linker supplies a real linked metadata graph. Strip local
        // symbols so class/method recovery must follow the signing recipes.
        const auto Driver = llvm::sys::findProgramByName("clang");
        ASSERT_TRUE(Driver);
        llvm::SmallString<128> BundlePath;
        ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-arm64e-objc",
                                                        "bundle", BundlePath));
        llvm::FileRemover RemoveBundle(BundlePath);
        const llvm::StringRef Args[] = {*Driver,
                                        "-target",
                                        "arm64e-apple-macos",
                                        "-bundle",
                                        "-Wl,-undefined,dynamic_lookup",
                                        "-Wl,-x",
                                        Path,
                                        "-o",
                                        BundlePath};
        std::string Error;
        ASSERT_EQ(llvm::sys::ExecuteAndWait(*Driver, Args, std::nullopt, {}, 30,
                                            0, &Error),
                  0)
            << Error;
        auto BundleBytes = llvm::MemoryBuffer::getFile(BundlePath);
        ASSERT_TRUE(BundleBytes);
        auto Bundle =
            MachOLoader().loadBuffer((*BundleBytes)->getMemBufferRef());
        ASSERT_TRUE(bool(Bundle)) << llvm::toString(Bundle.takeError());
        EXPECT_FALSE(Bundle->IsRelocatable);
        ASSERT_EQ(Bundle->ObjCClasses.size(), 1);
        ASSERT_EQ(Bundle->ObjCMethods.size(), 1);
        const auto &Method = Bundle->ObjCMethods[0];
        EXPECT_EQ(Method.ClassName, "ArmMetadata");
        EXPECT_EQ(Method.Selector, "increment:");
        EXPECT_EQ(Method.Status, "supported");
        EXPECT_TRUE(Bundle->isCodeAddress(Method.Implementation));
        EXPECT_EQ(Bundle->getFunctionNameAt(Method.Implementation),
                  "-[ArmMetadata increment:]");
        EXPECT_EQ(Bundle->statedFunctionNameAt(Method.Implementation),
                  "-[ArmMetadata increment:]");
        const Symbol *Name = Bundle->findSymbolAt(Method.Implementation);
        ASSERT_TRUE(Name);
        EXPECT_EQ(Name->Name, "-[ArmMetadata increment:]");
        EXPECT_EQ(Name->Origin, NameOrigin::Stated);
        ASSERT_TRUE(Method.TypeHint);
#endif
      },
      false, {}, nullptr, "objective-c");
}

TEST(AArch64CPASemantics, ScalarOperandsRetainChecksThroughBothDecoders) {
  // Independently assembled with llvm-mc -mattr=+cpa. The multiply-add's
  // final input is its pointer base, not another multiplier.
  const struct {
    uint32_t Word;
    Intrinsic Id;
  } Cases[] = {{0x9a022020, Intrinsic::A64_Addpt},
               {0xda022020, Intrinsic::A64_Subpt},
               {0x9b620c20, Intrinsic::A64_Maddpt},
               {0x9b628c20, Intrinsic::A64_Msubpt}};
  for (bool Fast : {false, true}) {
    for (const auto &Case : Cases) {
      SCOPED_TRACE(testing::Message() << Case.Word << " " << Fast);
      const auto Ops = lift(Case.Word, Fast);
      ASSERT_EQ(Ops.size(), 1);
      const unsigned Inputs = aarch64CPAOperation(Case.Id)->Sources;
      expectIntrinsic(Ops[0], Case.Id, Inputs);
      expectRegister(Ops[0].Output, a64reg::X0);
      expectRegister(Ops[0].Inputs[1], a64reg::X1);
      expectRegister(Ops[0].Inputs[2], a64reg::X2);
      if (Inputs == 3)
        expectRegister(Ops[0].Inputs[3], a64reg::X3);
      EXPECT_TRUE(lowAArch64CPAShapeIsValid(Ops[0]));
      EXPECT_TRUE(isSideeffectIntrinsic(Case.Id));
      for (unsigned Index = 0; Index <= Inputs; ++Index) {
        auto Bad = Ops[0];
        (Index ? Bad.Inputs[Index] : Bad.Output).Size = 4;
        EXPECT_FALSE(lowAArch64CPAShapeIsValid(Bad));
      }
      auto Bad = Ops[0];
      --Bad.NumInputs;
      EXPECT_FALSE(lowAArch64CPAShapeIsValid(Bad));
    }
    const auto Shifted = lift(0x9a023fff, Fast); // addpt sp, sp, x2, lsl #7
    ASSERT_EQ(Shifted.size(), 2);
    EXPECT_EQ(Shifted[0].Opcode, NdOp::INT_LEFT);
    expectRegister(Shifted[0].Inputs[0], a64reg::X2);
    EXPECT_EQ(Shifted[0].Inputs[1].Offset, 7);
    expectIntrinsic(Shifted[1], Intrinsic::A64_Addpt, 2);
    expectRegister(Shifted[1].Output, a64reg::SP);
    expectRegister(Shifted[1].Inputs[1], a64reg::SP);
    EXPECT_EQ(Shifted[1].Inputs[2], Shifted[0].Output);

    const auto Alias = lift(0xda012c21, Fast); // subpt x1, x1, x1, lsl #3
    ASSERT_EQ(Alias.size(), 2);
    expectRegister(Alias[0].Inputs[0], a64reg::X1);
    expectRegister(Alias[1].Output, a64reg::X1);
    expectRegister(Alias[1].Inputs[1], a64reg::X1);
    EXPECT_EQ(Alias[1].Inputs[2], Alias[0].Output);

    const auto Zero = lift(0x9b7f7fff, Fast); // maddpt xzr, xzr, xzr, xzr
    ASSERT_EQ(Zero.size(), 1);
    expectIntrinsic(Zero[0], Intrinsic::A64_Maddpt, 3);
    for (unsigned I = 1; I < 4; ++I) {
      EXPECT_TRUE(Zero[0].Inputs[I].isConst());
      EXPECT_EQ(Zero[0].Inputs[I].Offset, 0);
    }
  }
}

TEST(AArch64CPASemantics, MalformedShapesCannotPreserveCallArguments) {
  for (const auto &Operation : kAArch64CPAOperations) {
    const auto Function = pointerFunction(Operation.Id, "checked_pointer");
    const auto &Op = Function.Blocks[0].Ops[0];
    EXPECT_TRUE(aarch64CPAMedShapeIsValid(Op));
    EXPECT_FALSE(endsCallArgumentSetup(Op));
    for (unsigned Index = 0; Index <= Operation.Sources; ++Index) {
      auto Bad = Op;
      (Index ? Bad.Inputs[Index] : Bad.Output).Size = 4;
      EXPECT_FALSE(aarch64CPAMedShapeIsValid(Bad));
      EXPECT_TRUE(endsCallArgumentSetup(Bad));
    }
    for (unsigned Invalid = 0; Invalid < 4; ++Invalid) {
      auto Bad = Op;
      if (Invalid == 0)
        --Bad.NumInputs;
      else if (Invalid == 1)
        Bad.IntrinsicOutputs.push_back(Bad.Output);
      else if (Invalid == 2)
        Bad.Output = MedVar::makeConst(0, 8);
      else
        Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
      EXPECT_FALSE(aarch64CPAMedShapeIsValid(Bad));
      EXPECT_TRUE(endsCallArgumentSetup(Bad));
    }
    bool HasIntrinsics = false;
    const std::vector<std::string> Args(Operation.Sources, "value");
    EXPECT_THROW(renderARMIntrinsicCall(Operation.Id, Args, 4, HasIntrinsics),
                 std::invalid_argument);
    EXPECT_THROW(renderARMIntrinsicCall(Operation.Id, {}, 8, HasIntrinsics),
                 std::invalid_argument);
    EXPECT_THROW(renderARMInlineAsm(Arch::AArch64, Operation.MachineAsm, "",
                                    "result", true, {}),
                 std::invalid_argument);
  }
}

TEST(AArch64PAuthSemantics, MalformedPointerShapesFailIRVerification) {
  const struct {
    uint32_t Word;
    Intrinsic Id;
  } Cases[] = {
      {0xdac10020, Intrinsic::Pacia},      {0xdac11020, Intrinsic::Autia},
      {0xdac143e0, Intrinsic::Xpaci},      {0x9ac23020, Intrinsic::A64_Pacga},
      {0x9a022020, Intrinsic::A64_Addpt},  {0x9b620c20, Intrinsic::A64_Maddpt},
      {0xdac18bfe, Intrinsic::A64_Pacia2}, {0xdac1bffe, Intrinsic::A64_Autib2}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Word);
    auto Ops = lift(Case.Word, false);
    ASSERT_EQ(Ops.size(), 1);
    LowBlock Block;
    Block.StartAddr = 0x1000;
    Block.EndAddr = 0x1004;
    Block.Ops = Ops;
    LowInstructionBoundary Boundary;
    Boundary.Address = Block.StartAddr;
    Boundary.Size = 4;
    Boundary.OpCount = 1;
    Block.InstructionBoundaries.push_back(Boundary);
    auto Valid = validateLowInstructionBoundaries(Block);
    EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    for (bool BadOutput : {false, true}) {
      Block.Ops = Ops;
      (BadOutput ? Block.Ops[0].Output : Block.Ops[0].Inputs[1]).Size = 4;
      auto Error = validateLowInstructionBoundaries(Block);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
  }
  for (Intrinsic Id :
       {Intrinsic::Pacia, Intrinsic::Autia, Intrinsic::Xpaci,
        Intrinsic::A64_Pacga, Intrinsic::A64_AuthBranch, Intrinsic::A64_Addpt,
        Intrinsic::A64_Maddpt, Intrinsic::A64_Pacia2, Intrinsic::A64_Autib2,
        Intrinsic::A64_AuthReturnIA2}) {
    const auto Function = pointerFunction(Id, "pointer_shape");
    EXPECT_TRUE(verifyMedFunc(Function, "pointer-shape"));
    for (bool BadOutput : {false, true}) {
      auto Bad = Function;
      auto &Op = Bad.Blocks[0].Ops[0];
      (BadOutput ? Op.Output : Op.Inputs[1]).Size = 4;
      EXPECT_FALSE(verifyMedFunc(Bad, "bad-pointer-shape"));
    }
  }
}

TEST(AArch64CPASemantics, LLVMAndBothCRoutesKeepExactInstructions) {
  const auto CheckObject = [](llvm::MemoryBufferRef Buffer, Intrinsic Id) {
    // Independently assembled fixed-register forms, not read from the
    // production instruction table. This also catches a misplaced operand.
    uint32_t Expected = 0;
    switch (Id) {
    case Intrinsic::A64_Addpt:
      Expected = 0x9a112210;
      break;
    case Intrinsic::A64_Subpt:
      Expected = 0xda112210;
      break;
    case Intrinsic::A64_Maddpt:
      Expected = 0x9b713e10;
      break;
    case Intrinsic::A64_Msubpt:
      Expected = 0x9b71be10;
      break;
    default:
      FAIL() << "unexpected checked instruction";
    }
    auto Image = MachOLoader().loadBuffer(Buffer);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    unsigned Count = 0;
    for (const auto &Segment : Image->Segments)
      if (Segment.isExecutable())
        for (size_t Offset = 0; Offset + 4 <= Segment.Data.size(); Offset += 4)
          Count += readLE<uint32_t>(Segment.Data.data() + Offset) == Expected;
    EXPECT_EQ(Count, 1);
  };
  for (const auto &Operation : kAArch64CPAOperations)
    for (bool Optimize : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << intrinsicName(Operation.Id) << " " << Optimize);
      const auto Function = pointerFunction(Operation.Id, "checked_pointer");
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, "cpa", Arch::AArch64);
      ASSERT_TRUE(Module);
      if (Optimize)
        optimize(*Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      unsigned Count = 0;
      for (const auto &Block : *Module->getFunction(Function.Name))
        for (const auto &Insn : Block)
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn))
            if (const auto *Asm =
                    llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())) {
              EXPECT_EQ(Asm->getAsmString(), Operation.MachineAsm);
              EXPECT_EQ(Asm->getConstraintString(),
                        Operation.llvmConstraints());
              EXPECT_TRUE(Asm->hasSideEffects());
              EXPECT_EQ(Call->arg_size(), Operation.Sources);
              if (Optimize)
                for (unsigned I = 0; I < Operation.Sources; ++I)
                  EXPECT_EQ(Call->getArgOperand(I),
                            Call->getFunction()->getArg(I));
              ++Count;
            }
      EXPECT_EQ(Count, 1);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      for (bool LLVMRoute : {false, true}) {
        if (!LLVMRoute && Optimize)
          continue;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVMRoute)
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
        else {
          auto High = MedToHighConverter().convert(Function, Arch::AArch64);
          ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
        }
        EXPECT_NE(Source.find(Operation.MachineAsm), std::string::npos)
            << Source;
        EXPECT_EQ(Source.find("unknown_intrinsic"), std::string::npos)
            << Source;
        compileArm64eC(Source, [&](llvm::StringRef Path) {
          auto Buffer = llvm::MemoryBuffer::getFile(Path);
          ASSERT_TRUE(Buffer);
          CheckObject((*Buffer)->getMemBufferRef(), Operation.Id);
        });
      }
      const auto Object =
          Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
      EXPECT_TRUE(Object.Success);
      CheckObject(
          llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char *>(
                                                    Object.ObjectData.data()),
                                                Object.ObjectData.size()),
                                "cpa.o"),
          Operation.Id);
    }
}

TEST(AArch64CPASemantics,
     DeadResultsKeepChecksAndTemporariesDoNotCaptureInputs) {
  for (const auto &Operation : kAArch64CPAOperations) {
    auto Function = pointerFunction(Operation.Id, "checked_pointer");
    Function.Blocks[0].Ops.back().Inputs[0] = MedVar::makeConst(11, 8);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, "dead_cpa", Arch::AArch64);
    ASSERT_TRUE(Module);
    optimize(*Module);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    for (bool LLVMRoute : {false, true}) {
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute)
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      else {
        auto High = MedToHighConverter().convert(Function, Arch::AArch64);
        ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
      }
      EXPECT_NE(Source.find(Operation.MachineAsm), std::string::npos) << Source;
      compileArm64eC(Source);
      std::vector<std::string> Args = {"__checked_pointer",
                                       "__checked_pointer_1"};
      if (Operation.Sources == 3)
        Args.push_back("__checked_pointer_2");
      bool HasIntrinsics = false;
      const std::string Body =
          LLVMRoute ? renderARMInlineAsm(Arch::AArch64, Operation.MachineAsm,
                                         "", Args[0], true, Args)
                              .Code +
                          "return " + Args[0] + ";"
                    : "return " +
                          renderARMIntrinsicCall(Operation.Id, Args, 8,
                                                 HasIntrinsics) +
                          ";";
      std::string Capture = "#include <stdint.h>\nuint64_t capture(";
      for (unsigned I = 0; I < Args.size(); ++I) {
        if (I)
          Capture += ", ";
        Capture += "uint64_t " + Args[I];
      }
      Capture += ") { " + Body + " }\n";
      compileArm64eC(
          Capture,
          [&](llvm::StringRef Path) {
            llvm::LLVMContext CContext;
            llvm::SMDiagnostic Diagnostic;
            auto CModule = llvm::parseIRFile(Path, Diagnostic, CContext);
            ASSERT_TRUE(CModule) << Diagnostic.getMessage().str();
            auto *CFn = CModule->getFunction("capture");
            ASSERT_TRUE(CFn);
            CFn->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*CModule);
            ASSERT_FALSE(llvm::verifyModule(*CModule));
            unsigned Count = 0;
            for (auto &Block : *CFn)
              for (auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                const auto *Asm = Call ? llvm::dyn_cast<llvm::InlineAsm>(
                                             Call->getCalledOperand())
                                       : nullptr;
                if (!Asm || Asm->getAsmString().find(Operation.MachineAsm) ==
                                std::string::npos)
                  continue;
                ++Count;
                ASSERT_EQ(Call->arg_size(), Operation.Sources);
                for (unsigned I = 0; I < Operation.Sources; ++I)
                  EXPECT_EQ(Call->getArgOperand(I), CFn->getArg(I));
                const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                ASSERT_TRUE(Return);
                EXPECT_EQ(Return->getReturnValue(), Call);
              }
            EXPECT_EQ(Count, 1);
          },
          /*EmitLLVM=*/true);
    }
  }
}

TEST(AArch64PAuthSemantics, RegisterAndZeroFormsRetainPointersAndModifiers) {
  const std::array<Intrinsic, 8> Ids = {
      Intrinsic::Pacia, Intrinsic::Pacib, Intrinsic::Pacda, Intrinsic::Pacdb,
      Intrinsic::Autia, Intrinsic::Autib, Intrinsic::Autda, Intrinsic::Autdb};
  for (bool Fast : {false, true})
    for (unsigned I = 0; I < Ids.size(); ++I)
      for (bool Zero : {false, true}) {
        SCOPED_TRACE(testing::Message() << I << " " << Fast << " " << Zero);
        const auto Ops =
            lift((Zero ? 0xdac123e0u : 0xdac10020u) + I * 0x400, Fast);
        ASSERT_EQ(Ops.size(), 1);
        expectIntrinsic(Ops[0], Ids[I], 2);
        expectRegister(Ops[0].Output, a64reg::X0);
        expectRegister(Ops[0].Inputs[1], a64reg::X0);
        if (Zero) {
          EXPECT_TRUE(Ops[0].Inputs[2].isConst());
          EXPECT_EQ(Ops[0].Inputs[2].Offset, 0);
        } else
          expectRegister(Ops[0].Inputs[2], a64reg::X1);
      }
}

TEST(AArch64PAuthSemantics, GenericSignaturePreservesIndependentSources) {
  for (bool Fast : {false, true}) {
    for (uint32_t Word : {0x9ac23020u, 0x9adf33e3u, 0x9ac13021u}) {
      SCOPED_TRACE(testing::Message() << Word << " " << Fast);
      const auto Ops = lift(Word, Fast);
      ASSERT_EQ(Ops.size(), 1);
      expectIntrinsic(Ops[0], Intrinsic::A64_Pacga, 2);
      EXPECT_TRUE(lowAArch64PAuthShapeIsValid(Ops[0]));
      if (Word == 0x9adf33e3u) {
        expectRegister(Ops[0].Output, a64reg::X3);
        EXPECT_TRUE(Ops[0].Inputs[1].isConst());
        EXPECT_EQ(Ops[0].Inputs[1].Offset, 0);
        expectRegister(Ops[0].Inputs[2], a64reg::SP);
      } else {
        expectRegister(Ops[0].Output,
                       Word == 0x9ac23020u ? a64reg::X0 : a64reg::X1);
        expectRegister(Ops[0].Inputs[1], a64reg::X1);
        expectRegister(Ops[0].Inputs[2],
                       Word == 0x9ac23020u ? a64reg::X2 : a64reg::X1);
      }
      for (unsigned Invalid = 0; Invalid < 4; ++Invalid) {
        auto Bad = Ops[0];
        if (Invalid == 0)
          Bad.Output.Size = 4;
        else if (Invalid == 1)
          Bad.Inputs[1].Size = 4;
        else if (Invalid == 2)
          Bad.Inputs[2].Size = 4;
        else
          --Bad.NumInputs;
        EXPECT_FALSE(lowAArch64PAuthShapeIsValid(Bad));
      }
    }
  }
  bool HasIntrinsics = false;
  EXPECT_THROW(renderARMIntrinsicCall(Intrinsic::A64_Pacga, {"a", "b"}, 4,
                                      HasIntrinsics),
               std::invalid_argument);
  EXPECT_THROW(renderARMIntrinsicCall(Intrinsic::A64_Pacga, {"a", "b", "c"}, 8,
                                      HasIntrinsics),
               std::invalid_argument);
}

TEST(AArch64PAuthSemantics, HintAliasesUseLRSPAnd1716InsteadOfClobberingX0) {
  const struct {
    unsigned Hint;
    Intrinsic Id;
  } Cases[] = {
      {8, Intrinsic::Pacia},  {10, Intrinsic::Pacib}, {12, Intrinsic::Autia},
      {14, Intrinsic::Autib}, {24, Intrinsic::Pacia}, {25, Intrinsic::Pacia},
      {26, Intrinsic::Pacib}, {27, Intrinsic::Pacib}, {28, Intrinsic::Autia},
      {29, Intrinsic::Autia}, {30, Intrinsic::Autib}, {31, Intrinsic::Autib}};
  for (bool Fast : {false, true})
    for (const auto &C : Cases) {
      SCOPED_TRACE(C.Hint);
      const auto Ops = lift(0xd503201f | (C.Hint << 5), Fast);
      ASSERT_EQ(Ops.size(), 1);
      expectIntrinsic(Ops[0], C.Id, 2);
      expectRegister(Ops[0].Output, C.Hint < 16 ? a64reg::X17 : a64reg::X30);
      expectRegister(Ops[0].Inputs[1], C.Hint < 16 ? a64reg::X17 : a64reg::X30);
      if (C.Hint < 16 || (C.Hint & 1))
        expectRegister(Ops[0].Inputs[2],
                       C.Hint < 16 ? a64reg::X16 : a64reg::SP);
      else {
        EXPECT_TRUE(Ops[0].Inputs[2].isConst());
        EXPECT_EQ(Ops[0].Inputs[2].Offset, 0);
      }
    }
}

TEST(AArch64PAuthSemantics,
     StripInstructionsRetainTheirReadModifyWriteOperand) {
  for (bool Fast : {false, true})
    for (uint32_t Word : {0xdac143e0u, 0xdac147e0u, 0xd50320ffu}) {
      const auto Ops = lift(Word, Fast);
      ASSERT_EQ(Ops.size(), 1);
      expectIntrinsic(
          Ops[0], Word == 0xdac147e0 ? Intrinsic::Xpacd : Intrinsic::Xpaci, 1);
      expectRegister(Ops[0].Inputs[1],
                     Word == 0xd50320ff ? a64reg::X30 : a64reg::X0);
      EXPECT_EQ(Ops[0].Output.Offset, Ops[0].Inputs[1].Offset);
    }
}

TEST(AArch64PAuthSemantics, CallsAuthenticateOldLRBeforeWritingTheLink) {
  for (bool Fast : {false, true})
    for (bool KeyB : {false, true})
      for (bool Zero : {false, true}) {
        const auto Ops =
            lift((Zero ? 0xd63f0bdfu : 0xd73f0bdeu) + (KeyB ? 0x400 : 0), Fast);
        ASSERT_EQ(Ops.size(), 3);
        expectIntrinsic(Ops[0], Intrinsic::A64_AuthBranch, 3);
        expectRegister(Ops[0].Inputs[1], a64reg::X30);
        EXPECT_EQ(Ops[0].Inputs[2].Offset, unsigned(KeyB));
        if (!Zero)
          expectRegister(Ops[0].Inputs[3], a64reg::X30);
        else
          EXPECT_EQ(Ops[0].Inputs[3].Offset, 0);
        EXPECT_EQ(Ops[1].Opcode, NdOp::COPY);
        expectRegister(Ops[1].Output, a64reg::X30);
        EXPECT_EQ(Ops[1].Inputs[0].Offset, 0x1004);
        EXPECT_EQ(Ops[2].Opcode, NdOp::INDIR_CALL);
        EXPECT_EQ(Ops[2].Inputs[0], Ops[0].Output);
      }
}

TEST(AArch64PAuthSemantics, BranchesAndReturnsAuthenticateAndTerminate) {
  for (bool Fast : {false, true})
    for (uint32_t Word : {0xd71f0801u, 0xd71f0c01u, 0xd61f081fu, 0xd61f0c1fu,
                          0xd65f0bffu, 0xd65f0fffu}) {
      SCOPED_TRACE(Word);
      const bool Return = (Word & 0xfffff000) == 0xd65f0000;
      const auto Ops = lift(Word, Fast);
      ASSERT_EQ(Ops.size(), 2);
      expectIntrinsic(Ops[0], Intrinsic::A64_AuthBranch, 3);
      EXPECT_EQ(Ops[0].Inputs[2].Offset, (Word >> 10) & 1);
      if (Return) {
        expectRegister(Ops[0].Inputs[1], a64reg::X30);
        expectRegister(Ops[0].Inputs[3], a64reg::SP);
      }
      EXPECT_EQ(Ops[1].Opcode, Return ? NdOp::RETURN : NdOp::INDIR_BR);
      EXPECT_EQ(Ops[1].Inputs[0], Ops[0].Output);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::AArch64));
      uint8_t Bytes[4];
      writeLE<uint32_t>(Bytes, Word);
      DecodedInsn Insn;
      ASSERT_EQ(Dec.decodeOne(Bytes, 4, 0x1000, Insn), 4);
      EXPECT_TRUE(Dec.isFunctionTerminator(Insn));
    }
}

namespace {
namespace pauth_load_fixture {
#define NEVERD_PAUTH_LOAD_VALUE(Type, Name, Value)                             \
  inline constexpr Type Name = Value;
#define NEVERD_PAUTH_LOAD_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64PAuthLoadFixtures.def"

struct Case {
  uint32_t Word;
  Intrinsic Id;
  unsigned Destination;
  unsigned Base;
  int64_t Displacement;
  bool Writeback;
  uint32_t Scratch;
  bool Compile;
};
constexpr Case Cases[] = {
#define NEVERD_PAUTH_LOAD_CASE(Word, Id, Dst, Base, Offset, WB, Scratch,       \
                               Compile)                                        \
  {Word, Intrinsic::Id, Dst, Base, Offset, WB, Scratch, Compile},
#include "AArch64PAuthLoadFixtures.def"
};

MedFunc function(uint32_t Word, bool Discard = false) {
  MedFunc Function;
  Function.Name = kName;
  Function.Entry = kEntry;
  Function.ReturnType = NdType::makeInt(kPairBytes, false);
  MedVar Base;
  Base.Kind = MedVar::Param;
  Base.TheArch = Arch::AArch64;
  Base.Id = 0;
  Base.RegOff = a64reg::X0;
  Base.Size = kScalarBytes;
  Function.Params.push_back(Base);
  MedOp Load;
  Load.Opcode = NdOp::INTRINSIC;
  Load.Output.Kind = MedVar::Temp;
  Load.Output.TheArch = Arch::AArch64;
  Load.Output.Id = 1;
  Load.Output.SSAVer = 1;
  Load.Output.Size = kPairBytes;
  Load.addInput(MedVar::makeConst(
      static_cast<uint64_t>(aarch64PAuthLoadInstruction(Word)->Id),
      kIdentifierBytes));
  Load.addInput(Base);
  Load.addInput(MedVar::makeConst(Word, kWordBytes));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Discard ? MedVar::makeConst(0, kPairBytes) : Load.Output);
  Function.Blocks.emplace_back();
  Function.Blocks.front().Id = 0;
  Function.Blocks.front().StartAddr = kEntry;
  Function.Blocks.front().Ops = {Load, Return};
  return Function;
}

void checkObject(llvm::MemoryBufferRef Buffer, uint32_t Expected) {
  auto Image = MachOLoader().loadBuffer(Buffer);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  unsigned Count = 0;
  for (const auto &Segment : Image->Segments)
    if (Segment.isExecutable())
      for (size_t Offset = 0; Offset + kWordBytes <= Segment.Data.size();
           Offset += kWordBytes)
        Count += readLE<uint32_t>(Segment.Data.data() + Offset) == Expected;
  EXPECT_EQ(Count, 1);
}

void checkProjections(uint32_t Word, uint32_t Scratch, bool Discard) {
  const auto Function = function(Word, Discard);
  ASSERT_TRUE(verifyMedFunc(Function, kName));
  for (bool Optimize : {false, true}) {
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    if (Optimize)
      optimize(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    unsigned Count = 0;
    for (const auto &Block : *Module->getFunction(kName))
      for (const auto &Insn : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Insn)) {
          const auto *Asm =
              llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
          ASSERT_TRUE(Asm);
          EXPECT_EQ(Asm->getAsmString(), kPAuthLoadAsm);
          EXPECT_EQ(Asm->getConstraintString(), kPAuthLoadConstraints);
          EXPECT_NE(Asm->getConstraintString().find(kMemoryClobber),
                    std::string::npos);
          EXPECT_TRUE(Asm->hasSideEffects());
          ASSERT_EQ(Call->arg_size(), kPAuthLoadOperandCount);
          const auto *Immediate =
              llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(1));
          ASSERT_TRUE(Immediate);
          EXPECT_EQ(Immediate->getZExtValue(), Scratch);
          ++Count;
        }
    EXPECT_EQ(Count, 1);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    for (bool LLVMRoute : {false, true}) {
      if (!LLVMRoute && Optimize)
        continue;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute)
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      else {
        const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
        ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
      }
      EXPECT_NE(Source.find(kAsmPrefix), std::string::npos) << Source;
      EXPECT_EQ(Source.find(kUnknown), std::string::npos) << Source;
      compileArm64eC(Source, [&](llvm::StringRef Path) {
        const auto Buffer = llvm::MemoryBuffer::getFile(Path);
        ASSERT_TRUE(bool(Buffer));
        checkObject((*Buffer)->getMemBufferRef(), Scratch);
      });
    }
    const auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
    ASSERT_TRUE(Object.Success);
    checkObject(
        llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char *>(
                                                  Object.ObjectData.data()),
                                              Object.ObjectData.size()),
                              kObject),
        Scratch);
  }
}
} // namespace pauth_load_fixture
} // namespace

TEST(AArch64PAuthLoadSemantics,
     RawWordsRetainCombinedLoadAndCompletedWriteback) {
  using namespace pauth_load_fixture;
  for (const auto &Case : Cases)
    for (bool Fast : {false, true}) {
      SCOPED_TRACE(Case.Word);
      const auto Instruction = aarch64PAuthLoadInstruction(Case.Word);
      ASSERT_TRUE(Instruction);
      EXPECT_EQ(Instruction->Id, Case.Id);
      EXPECT_EQ(Instruction->destination(), Case.Destination);
      EXPECT_EQ(Instruction->base(), Case.Base);
      EXPECT_EQ(Instruction->displacement(), Case.Displacement);
      EXPECT_EQ(Instruction->writeback(), Case.Writeback);
      EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
      const auto Ops = lift(Case.Word, Fast);
      ASSERT_EQ(Ops.size(),
                1U + (Case.Destination != kZeroRegister) + Case.Writeback);
      expectIntrinsic(Ops.front(), Case.Id, kPAuthLoadOperandCount);
      EXPECT_TRUE(lowAArch64PAuthLoadShapeIsValid(Ops.front()));
      EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Ops.front()));
      EXPECT_FALSE(lowIntrinsicMemoryOperands(Ops.front()).Complete);
      EXPECT_EQ(Ops.front().Output.Size, kPairBytes);
      EXPECT_EQ(Ops.front().Inputs[2].Offset, Case.Word);
      expectRegister(Ops.front().Inputs[1],
                     Case.Base == kZeroRegister
                         ? a64reg::SP
                         : a64reg::X0 + Case.Base * kScalarBytes);
      if (Case.Destination != kZeroRegister) {
        EXPECT_EQ(Ops[1].Opcode, NdOp::SUBBYTES);
        EXPECT_EQ(Ops[1].Inputs[0], Ops.front().Output);
        EXPECT_EQ(Ops[1].Inputs[1].Offset, 0);
        expectRegister(Ops[1].Output,
                       a64reg::X0 + Case.Destination * kScalarBytes);
      }
      if (Case.Writeback) {
        EXPECT_EQ(Ops.back().Opcode, NdOp::SUBBYTES);
        EXPECT_EQ(Ops.back().Inputs[0], Ops.front().Output);
        EXPECT_EQ(Ops.back().Inputs[1].Offset, kScalarBytes);
        EXPECT_EQ(Ops.back().Output, Ops.front().Inputs[1]);
      }
      EXPECT_TRUE(isSideeffectIntrinsic(Case.Id));
    }
  EXPECT_FALSE(aarch64PAuthLoadInstruction(kOverlap));
  EXPECT_FALSE(aarch64PAuthLoadInstruction(kInvalidOpcode));
  EXPECT_FALSE(aarch64PAuthLoadInstruction(kOversizedWord));
}

TEST(AArch64PAuthLoadSemantics, LLVMAndCEmitOneCombinedInstruction) {
  using namespace pauth_load_fixture;
  for (const auto &Case : Cases)
    if (Case.Compile)
      checkProjections(Case.Word, Case.Scratch, false);
}

TEST(AArch64PAuthLoadSemantics, ValidLoadsPreserveUnwrittenCallArguments) {
  using namespace pauth_load_fixture;
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Word);
    const auto Function = function(Case.Word);
    const auto &Load = Function.Blocks.front().Ops.front();
    ASSERT_TRUE(aarch64PAuthLoadMedShapeIsValid(Load));
    EXPECT_FALSE(endsCallArgumentSetup(Load));
  }
}

TEST(AArch64PAuthLoadSemantics, DiscardedResultsRetainMemoryAndPACFailure) {
  using namespace pauth_load_fixture;
  checkProjections(kDeadA, kDeadScratchA, true);
  checkProjections(kDeadB, kDeadScratchB, true);
}

TEST(AArch64PAuthLoadSemantics, SPProvenanceCannotBecomeGPRExecution) {
  using namespace pauth_load_fixture;
  for (const auto &Case : Cases) {
    if (Case.Base != kZeroRegister)
      continue;
    const auto Function = function(Case.Word);
    EXPECT_TRUE(
        aarch64PAuthLoadMedShapeIsValid(Function.Blocks.front().Ops.front()));
    llvm::LLVMContext Context;
    EXPECT_THROW(
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64),
        std::runtime_error);
    bool HasIntrinsics = false;
    EXPECT_THROW(renderARMIntrinsicCall(
                     Case.Id, {kBaseExpression, std::to_string(Case.Word)},
                     kPairBytes, HasIntrinsics, {kScalarBytes, kWordBytes}),
                 std::invalid_argument);
  }
}

TEST(AArch64PAuthLoadSemantics, MalformedContractsFailBeforeEmission) {
  using namespace pauth_load_fixture;
  const auto Function = function(kFirst);
  const auto Ops = lift(kFirst, false);
  enum class Mutation {
    WrappedId,
    Width,
    WordWidth,
    DynamicWord,
    InvalidWord,
    WrongKey,
    ExtraInput,
    ExtraOutput,
    BaseKind,
    Ordering,
    AddressSpace
  };
  for (const auto Change :
       {Mutation::WrappedId, Mutation::Width, Mutation::WordWidth,
        Mutation::DynamicWord, Mutation::InvalidWord, Mutation::WrongKey,
        Mutation::ExtraInput, Mutation::ExtraOutput, Mutation::BaseKind,
        Mutation::Ordering, Mutation::AddressSpace}) {
    auto Bad = Function;
    auto Low = Ops.front();
    auto &Med = Bad.Blocks.front().Ops.front();
    switch (Change) {
    case Mutation::WrappedId:
      Low.Inputs[0].Offset += kWrappedIdIncrement;
      Med.Inputs[0].ConstVal += kWrappedIdIncrement;
      break;
    case Mutation::Width:
      Low.Output.Size = Med.Output.Size = kScalarBytes;
      break;
    case Mutation::WordWidth:
      Low.Inputs[2].Size = Med.Inputs[2].Size = kScalarBytes;
      break;
    case Mutation::DynamicWord:
      Low.Inputs[2].Space = VnodeSpace::TEMP;
      Med.Inputs[2].Kind = MedVar::Temp;
      break;
    case Mutation::InvalidWord:
      Low.Inputs[2].Offset = Med.Inputs[2].ConstVal = kInvalidOpcode;
      break;
    case Mutation::WrongKey:
      Low.Inputs[2].Offset = Med.Inputs[2].ConstVal = kDifferentKey;
      break;
    case Mutation::ExtraInput:
      Low.addInput(Low.Inputs[1]);
      Med.addInput(Med.Inputs[1]);
      break;
    case Mutation::ExtraOutput:
      Med.IntrinsicOutputs.push_back(Med.Output);
      break;
    case Mutation::BaseKind:
      Low.Inputs[1].Space = VnodeSpace::RAM;
      Med.Inputs[1].Kind = MedVar::Stack;
      break;
    case Mutation::Ordering:
      Low.MemoryOrdering = Med.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case Mutation::AddressSpace:
      Low.MemoryAddressSpace = Med.MemoryAddressSpace =
          NdMemoryAddressSpace::X86FS;
      break;
    }
    if (Change != Mutation::ExtraOutput)
      EXPECT_FALSE(lowAArch64PAuthLoadShapeIsValid(Low));
    EXPECT_FALSE(aarch64PAuthLoadMedShapeIsValid(Med));
    EXPECT_TRUE(endsCallArgumentSetup(Med));
    EXPECT_FALSE(verifyMedFunc(Bad, kName));
    llvm::LLVMContext Context;
    EXPECT_THROW(MedLLVMEmitter().emit({Bad}, Context, kName, Arch::AArch64),
                 std::runtime_error);
  }
}

TEST(AArch64PAuthLoadSemantics, SourceAdaptersRejectMalformedOwnedOperations) {
  using namespace pauth_load_fixture;
  const auto Function = function(kFirst);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
  ASSERT_TRUE(Module);
  llvm::CallInst *Call = nullptr;
  for (auto &Block : *Module->getFunction(kName))
    for (auto &Insn : Block)
      if (auto *Candidate = llvm::dyn_cast<llvm::CallInst>(&Insn))
        if (const auto *Asm =
                llvm::dyn_cast<llvm::InlineAsm>(Candidate->getCalledOperand());
            Asm && Asm->getAsmString() == kPAuthLoadAsm)
          Call = Candidate;
  ASSERT_TRUE(Call);
  const std::vector<std::string> Args = {kBaseExpression,
                                         std::to_string(Cases[0].Scratch)};
  ASSERT_TRUE(renderAArch64PAuthLoadCall(Arch::AArch64, *Call, kResultName,
                                         true, Args));
  EXPECT_THROW(
      renderAArch64PAuthLoadCall(Arch::X64, *Call, kResultName, true, Args),
      std::invalid_argument);
  auto *Original = Call->getArgOperand(1);
  Call->setArgOperand(
      1, llvm::ConstantInt::get(Context,
                                llvm::APInt(kWordBytes * CHAR_BIT, kFirst)));
  EXPECT_THROW(
      renderAArch64PAuthLoadCall(Arch::AArch64, *Call, kResultName, true, Args),
      std::invalid_argument);
  Call->setArgOperand(1, Original);
  bool HasIntrinsics = false;
  for (const auto Widths :
       {std::vector<uint16_t>{}, std::vector<uint16_t>{kWordBytes, kWordBytes},
        std::vector<uint16_t>{kScalarBytes, kScalarBytes}})
    EXPECT_THROW(renderARMIntrinsicCall(Intrinsic::A64_Ldraa, Args, kPairBytes,
                                        HasIntrinsics, Widths),
                 std::invalid_argument);

  HighExpr Expression;
  Expression.Kind = ExprKind::Call;
  Expression.IntrinsicId = Intrinsic::A64_Ldraa;
  Expression.Type = NdType::makeInt(kPairBytes, false);
  auto Base = std::make_shared<HighExpr>();
  Base->Kind = ExprKind::Var;
  Base->Type = NdType::makeInt(kScalarBytes, false);
  auto Word = std::make_shared<HighExpr>();
  Word->Kind = ExprKind::Const;
  Word->Type = NdType::makeInt(kWordBytes, false);
  Word->ConstVal = kFirst;
  Expression.Operands = {Base, Word};
  ASSERT_TRUE(aarch64PAuthLoadHighShapeIsValid(Expression));
  Base->Type = NdType::makeInt(kWordBytes, true);
  EXPECT_FALSE(aarch64PAuthLoadHighShapeIsValid(Expression));
  Base->Type = NdType::makeFloat(kScalarBytes);
  EXPECT_FALSE(aarch64PAuthLoadHighShapeIsValid(Expression));
  Base->Type = NdType::makeInt(kScalarBytes, false);
  Word->Kind = ExprKind::Var;
  EXPECT_FALSE(aarch64PAuthLoadHighShapeIsValid(Expression));
}

TEST(AArch64PAuthLoadSemantics, HighCRejectsMalformedLoadsBeforeHiding) {
  using namespace pauth_load_fixture;
  enum class Mutation {
    ExtraOutput,
    IndirectCall,
    IndirectTarget,
    NonCall,
    WrongWidth,
    DynamicWord,
    StackBase,
    CrossArchitecture
  };
  for (bool Hidden : {false, true})
    for (const auto Change :
         {Mutation::ExtraOutput, Mutation::IndirectCall,
          Mutation::IndirectTarget, Mutation::NonCall, Mutation::WrongWidth,
          Mutation::DynamicWord, Mutation::StackBase,
          Mutation::CrossArchitecture}) {
      SCOPED_TRACE(static_cast<int>(Change));
      SCOPED_TRACE(Hidden);
      HighFunc Function;
      Function.Entry = kEntry;
      Function.Name = kName;
      Function.ReturnType = NdType::makeVoid();
      auto Load = std::make_shared<HighExpr>();
      Load->Kind = ExprKind::Call;
      Load->IntrinsicId = Intrinsic::A64_Ldraa;
      Load->Type = NdType::makeInt(kPairBytes, false);
      Load->Operands = {HighExpr::makeConst(kEntry, kScalarBytes),
                        HighExpr::makeConst(kFirst, kWordBytes)};
      ASSERT_TRUE(aarch64PAuthLoadHighShapeIsValid(*Load));
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      switch (Change) {
      case Mutation::ExtraOutput:
        Load->IntrinsicOutputs.push_back(MedVar::makeConst(0, kScalarBytes));
        break;
      case Mutation::IndirectCall:
        Load->IsIndirectCall = true;
        break;
      case Mutation::IndirectTarget:
        Load->IndirectTarget = HighExpr::makeConst(kEntry, kScalarBytes);
        break;
      case Mutation::NonCall:
        Load->Kind = ExprKind::Const;
        break;
      case Mutation::WrongWidth:
        Load->Type = NdType::makeInt(kScalarBytes, false);
        break;
      case Mutation::DynamicWord:
        Load->Operands.back()->Kind = ExprKind::Var;
        break;
      case Mutation::StackBase:
        Load->Operands.back()->ConstVal = kStackBase;
        break;
      case Mutation::CrossArchitecture:
        Options.TheArch = Arch::X64;
        break;
      }
      HighStmt Invoke;
      Invoke.Kind = StmtKind::ExprStmt;
      Invoke.Val = Load;
      if (Hidden) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        Branch.Cond = HighExpr::makeConst(0, kBooleanBytes);
        Branch.Body.push_back(Invoke);
        Function.Body.push_back(Branch);
      } else {
        Function.Body.push_back(Invoke);
      }
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(HighCEmitter().emit({Function}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Source.empty()) << Source;
    }
}

TEST(AArch64PAuthLoadSemantics, CTemporariesDoNotCaptureBaseExpression) {
  using namespace pauth_load_fixture;
  const auto Expression =
      aarch64PAuthLoadC(*aarch64PAuthLoadInstruction(kFirst), kBaseExpression);
  compileArm64eC(llvm::formatv(kCapturedSource, Expression).str());
}

TEST(AArch64PAuthSemantics, AuthenticationCannotBeEliminatedAsDead) {
  for (Intrinsic Id : {Intrinsic::Autia, Intrinsic::Autib, Intrinsic::Autda,
                       Intrinsic::Autdb, Intrinsic::A64_AuthBranch})
    EXPECT_TRUE(isSideeffectIntrinsic(Id));
}

TEST(AArch64PAuthSemantics, CRenderersPreserveTiedPointerOperands) {
  for (Intrinsic Id : {Intrinsic::Pacia, Intrinsic::Autia, Intrinsic::Pacdb,
                       Intrinsic::Autdb, Intrinsic::Xpaci, Intrinsic::Xpacd}) {
    const auto Auth = *aarch64PAuthOperation(Id);
    const std::vector<std::string> Args =
        Auth.Strip ? std::vector<std::string>{"pointer"}
                   : std::vector<std::string>{"pointer", "modifier"};
    bool HasIntrinsics = false;
    const auto High = renderARMIntrinsicCall(Id, Args, 8, HasIntrinsics);
    EXPECT_NE(High.find("\"+r\"(__pointer)"), std::string::npos) << High;
    const auto Low = renderARMInlineAsm(Arch::AArch64,
                                        std::string(Auth.Mnemonic) +
                                            (Auth.Strip ? " $0" : " $0, $2"),
                                        Auth.Mnemonic, "result", true, Args);
    EXPECT_NE(Low.Code.find("\"+r\"(__pointer)"), std::string::npos);
    EXPECT_NE(Low.Code.find("result = "), std::string::npos);
  }
}

TEST(AArch64PAuthSemantics, CTemporariesNeverCaptureOperandsOrResults) {
  for (Intrinsic Id : {Intrinsic::Pacia, Intrinsic::Autdb, Intrinsic::Xpaci,
                       Intrinsic::A64_AuthBranch, Intrinsic::A64_Pacga}) {
    const bool Branch = Id == Intrinsic::A64_AuthBranch;
    const bool Strip = Id == Intrinsic::Xpaci;
    const bool Generic = Id == Intrinsic::A64_Pacga;
    const std::string Pointer = Branch ? "__target" : "__pointer";
    const std::string Modifier = Pointer + "_";
    std::vector<std::string> Args{Pointer};
    if (Branch)
      Args.push_back("1");
    if (!Strip)
      Args.push_back(Modifier);
    for (bool LLVMRoute : {false, true}) {
      if (Branch && LLVMRoute)
        continue; // LLVMC's auth builtin introduces no local C scope.
      if (Generic && LLVMRoute)
        continue; // Tested through the LLVM intrinsic's complete C projection.
      SCOPED_TRACE(testing::Message() << intrinsicName(Id) << " " << LLVMRoute);
      std::string Body;
      if (LLVMRoute) {
        const auto Auth = *aarch64PAuthOperation(Id);
        // The output deliberately has the same name as the original input.
        Body = renderARMInlineAsm(Arch::AArch64, Auth.llvmAsm(), Auth.Mnemonic,
                                  Pointer, true, Args)
                   .Code +
               "return " + Pointer + ";";
      } else {
        bool HasIntrinsics = false;
        Body = "return " + renderARMIntrinsicCall(Id, Args, 8, HasIntrinsics) +
               ";";
      }
      const std::string Source =
          "#include <stdint.h>\nuint64_t capture(uint64_t " + Pointer +
          ", uint64_t " + Modifier + ") { " + Body + " }\n";
      compileArm64eC(
          Source,
          [&](llvm::StringRef Path) {
            llvm::LLVMContext Context;
            llvm::SMDiagnostic Diagnostic;
            auto Module = llvm::parseIRFile(Path, Diagnostic, Context);
            ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
            auto *Function = Module->getFunction("capture");
            ASSERT_TRUE(Function);
            Function->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*Module);
            ASSERT_FALSE(llvm::verifyModule(*Module));
            Function = Module->getFunction("capture");
            ASSERT_TRUE(Function);
            unsigned AuthenticationCount = 0;
            for (auto &Block : *Function)
              for (auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                if (!Call)
                  continue;
                const auto *Asm =
                    llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
                const auto *Callee = Call->getCalledFunction();
                const bool Machine =
                    Asm && !Branch &&
                    llvm::StringRef(Asm->getAsmString())
                        .starts_with(
                            std::string(
                                Generic ? intrinsicAsmMnemonic(Id)
                                        : aarch64PAuthOperation(Id)->Mnemonic) +
                            " ");
                const bool Control =
                    Branch && Callee &&
                    Callee->getIntrinsicID() == llvm::Intrinsic::ptrauth_auth;
                if (!Machine && !Control)
                  continue;
                ++AuthenticationCount;
                // Clang places the explicit modifier before the implicit tied
                // input of a +r output. NeverD's direct LLVM emitter uses its
                // own valid ordering; this oracle checks the C compiler's IR.
                if (Machine)
                  EXPECT_EQ(Asm->getConstraintString(), Generic ? "=r,r,r"
                                                        : Strip ? "=r,0"
                                                                : "=r,r,0");
                EXPECT_EQ(
                    Call->getArgOperand(Branch || Strip || Generic ? 0 : 1),
                    Function->getArg(0));
                if (!Strip)
                  EXPECT_EQ(Call->getArgOperand(Branch    ? 2
                                                : Generic ? 1
                                                          : 0),
                            Function->getArg(1));
                const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                ASSERT_TRUE(Return);
                EXPECT_EQ(Return->getReturnValue(), Call);
              }
            EXPECT_EQ(AuthenticationCount, 1u) << Source;
          },
          /*EmitLLVM=*/true);
    }
  }
}

TEST(AArch64PAuthSemantics, LLVMAndBothCRoutesRetainMachineAuthentication) {
  for (Intrinsic Id :
       {Intrinsic::Pacia, Intrinsic::Pacib, Intrinsic::Pacda, Intrinsic::Pacdb,
        Intrinsic::Autia, Intrinsic::Autib, Intrinsic::Autda, Intrinsic::Autdb,
        Intrinsic::Xpaci, Intrinsic::Xpacd, Intrinsic::A64_AuthBranch,
        Intrinsic::A64_Pacga}) {
    SCOPED_TRACE(intrinsicName(Id));
    MedFunc F = pointerFunction(Id, "authenticate_pointer");
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({F}, Context, "pauth", Arch::AArch64);
    ASSERT_TRUE(Module);
    EXPECT_FALSE(llvm::verifyModule(*Module));
    const auto *Fn = Module->getFunction(F.Name);
    ASSERT_TRUE(Fn);
    unsigned AuthCalls = 0;
    for (const auto &Block : *Fn)
      for (const auto &Insn : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Insn)) {
          if (Id == Intrinsic::A64_AuthBranch || Id == Intrinsic::A64_Pacga) {
            if (const auto *Callee = Call->getCalledFunction();
                Callee && Callee->getIntrinsicID() ==
                              (Id == Intrinsic::A64_Pacga
                                   ? llvm::Intrinsic::ptrauth_sign_generic
                                   : llvm::Intrinsic::ptrauth_auth))
              ++AuthCalls;
          } else if (const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(
                         Call->getCalledOperand())) {
            EXPECT_EQ(Asm->getConstraintString(),
                      aarch64PAuthOperation(Id)->Strip ? "=r,0" : "=r,0,r");
            EXPECT_TRUE(Asm->hasSideEffects());
            ++AuthCalls;
          }
        }
    EXPECT_EQ(AuthCalls, 1);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    std::string LowC, HighC;
    llvm::raw_string_ostream LowOS(LowC), HighOS(HighC);
    EXPECT_TRUE(LLVMCEmitter().emit(*Module, LowOS, Options));
    auto High = MedToHighConverter().convert(F, Arch::AArch64);
    EXPECT_TRUE(HighCEmitter().emit({High}, HighOS, Options));
    for (const std::string *C : {&LowC, &HighC}) {
      EXPECT_NE(C->find(Id == Intrinsic::A64_AuthBranch
                            ? "__builtin_ptrauth_auth"
                        : Id == Intrinsic::A64_Pacga
                            ? "pacga %0, %1, %2"
                            : aarch64PAuthOperation(Id)->Mnemonic),
                std::string::npos)
          << *C;
      EXPECT_EQ(C->find("unknown_intrinsic"), std::string::npos) << *C;
      if (Id != Intrinsic::A64_Pacga) {
        compileArm64eC(*C);
        continue;
      }
      compileArm64eC(
          *C,
          [&](llvm::StringRef Path) {
            llvm::LLVMContext CContext;
            llvm::SMDiagnostic Diagnostic;
            auto CModule = llvm::parseIRFile(Path, Diagnostic, CContext);
            ASSERT_TRUE(CModule) << Diagnostic.getMessage().str();
            auto *Function = CModule->getFunction(F.Name);
            ASSERT_TRUE(Function);
            Function->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*CModule);
            Function = CModule->getFunction(F.Name);
            ASSERT_TRUE(Function);
            unsigned Count = 0;
            for (auto &Block : *Function)
              for (auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                const auto *Asm = Call ? llvm::dyn_cast<llvm::InlineAsm>(
                                             Call->getCalledOperand())
                                       : nullptr;
                if (!Asm || Asm->getAsmString() != "pacga $0, $1, $2")
                  continue;
                ++Count;
                EXPECT_EQ(Asm->getConstraintString(), "=r,r,r");
                EXPECT_EQ(Call->getArgOperand(0), Function->getArg(0));
                EXPECT_EQ(Call->getArgOperand(1), Function->getArg(1));
                const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                ASSERT_TRUE(Return);
                EXPECT_EQ(Return->getReturnValue(), Call);
              }
            EXPECT_EQ(Count, 1);
          },
          /*EmitLLVM=*/true);
    }
    auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
    EXPECT_TRUE(Object.Success);
  }
}

TEST(AArch64PAuthSemantics,
     CompilerReturnSequenceKeepsAuthenticationAndX0Result) {
  // pacibsp; mov x0, #7; retab. RETAB's LR is control flow, never the C result.
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 12;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(12);
  writeLE<uint32_t>(Text.Data.data(), 0xd503237f);
  writeLE<uint32_t>(Text.Data.data() + 4, 0xd28000e0);
  writeLE<uint32_t>(Text.Data.data() + 8, 0xd65f0fff);
  Image.Segments.push_back(Text);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  Dec.setStrict(true);
  auto Low = CFGBuilder().build(Image, Dec, 0x1000, "signed_return");
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  unsigned Auths = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].ConstVal ==
              static_cast<uint64_t>(Intrinsic::A64_AuthBranch))
        ++Auths;
  EXPECT_EQ(Auths, 1);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "signed-return", Arch::AArch64);
  ASSERT_TRUE(Module);
  EXPECT_FALSE(llvm::verifyModule(*Module));
  auto *Fn = Module->getFunction("signed_return");
  ASSERT_TRUE(Fn);
  llvm::SmallVector<llvm::AllocaInst *> Allocas;
  for (auto &Insn : Fn->getEntryBlock())
    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&Insn))
      if (llvm::isAllocaPromotable(Alloca))
        Allocas.push_back(Alloca);
  llvm::DominatorTree DT(*Fn);
  llvm::PromoteMemToReg(Allocas, DT);
  unsigned Returns = 0;
  for (const auto &Block : *Fn)
    if (const auto *Ret =
            llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator())) {
      const auto *Value =
          llvm::dyn_cast_or_null<llvm::ConstantInt>(Ret->getReturnValue());
      ASSERT_TRUE(Value);
      EXPECT_EQ(Value->getZExtValue(), 7);
      ++Returns;
    }
  EXPECT_EQ(Returns, 1);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  std::string LowC, HighC;
  llvm::raw_string_ostream LowOS(LowC), HighOS(HighC);
  EXPECT_TRUE(LLVMCEmitter().emit(*Module, LowOS, Options));
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  EXPECT_TRUE(HighCEmitter().emit({High}, HighOS, Options));
  EXPECT_NE(LowC.find("__builtin_ptrauth_auth"), std::string::npos) << LowC;
  EXPECT_NE(HighC.find("branch-protection=pac-ret+leaf+b-key"),
            std::string::npos)
      << HighC;
  EXPECT_EQ(HighC.find("__builtin_ptrauth_auth"), std::string::npos) << HighC;
  compileArm64eC(LowC);
  compileArm64eC(HighC);
}

TEST(AArch64PAuthSemantics, ExceptionReturnsNeverLoseAuthentication) {
  for (bool Fast : {false, true})
    for (uint32_t Word : {0xd69f0bffu, 0xd69f0fffu})
      EXPECT_THROW(lift(Word, Fast), UnliftedInstruction);
}

namespace {
void executeCProjections(const std::vector<MedFunc> &Functions,
                         llvm::StringRef Oracle,
                         llvm::StringRef ExecuteTarget) {
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  for (bool LLVMRoute : {false, true})
    for (bool Optimize : {false, true}) {
      if (!LLVMRoute && Optimize)
        continue;
      SCOPED_TRACE(testing::Message() << LLVMRoute << " " << Optimize);
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute) {
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit(Functions, Context, "native", Arch::AArch64);
        ASSERT_TRUE(Module);
        if (Optimize)
          optimize(*Module);
        ASSERT_FALSE(llvm::verifyModule(*Module));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      } else {
        std::vector<HighFunc> High;
        for (const auto &F : Functions)
          High.push_back(MedToHighConverter().convert(F, Arch::AArch64));
        ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
      }
      Source.append(Oracle.data(), Oracle.size());
      compileArm64eC(Source, {}, /*EmitLLVM=*/false, ExecuteTarget);
    }
}

bool hasNativePAuth() {
#if defined(__APPLE__) && defined(__aarch64__)
  int Supported = 0;
  size_t Size = sizeof(Supported);
  return sysctlbyname("hw.optional.arm.FEAT_PAuth", &Supported, &Size, nullptr,
                      0) == 0 &&
         Supported;
#else
  return false;
#endif
}
} // namespace

TEST(AArch64PAuthSemantics, NativeGenericSignaturesAgreeWithBothCProjections) {
  if (!hasNativePAuth())
    GTEST_SKIP() << "native key comparison requires a PAuth Apple Silicon host";
  const auto Oracle = llvm::MemoryBuffer::getFile(
      std::string(NEVERD_ARM64E_TEST_HEADERS) + "/pacga_oracle.c");
  ASSERT_TRUE(Oracle);
  executeCProjections({pointerFunction(Intrinsic::A64_Pacga, "test_pacga")},
                      (*Oracle)->getBuffer(), "arm64-apple-macos");
}

TEST(AArch64PAuthSemantics, NativeKeysAgreeWithBothCProjections) {
#define NEVERD_PAUTH_NATIVE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "AArch64PAuthNative.def"
  if (!hasNativePAuth())
    GTEST_SKIP() << "native key comparison requires a PAuth Apple Silicon host";
  // A requested target alone is not evidence: a linker may emit ordinary
  // arm64. Require each signing key to change a pointer in the actual process.
  bool ExecutionAvailable = false;
  compileArm64eC(kSigningKeyProbe, {}, /*EmitLLVM=*/false, kSigningKeyTarget,
                 &ExecutionAvailable);
  if (HasFatalFailure())
    return;
  if (!ExecutionAvailable)
    GTEST_SKIP() << kSigningKeysUnavailable;
  const auto Oracle = llvm::MemoryBuffer::getFile(
      std::string(NEVERD_ARM64E_TEST_HEADERS) + "/pauth_oracle.c");
  ASSERT_TRUE(Oracle);
  std::vector<MedFunc> Functions;
  const std::pair<Intrinsic, const char *> Cases[] = {
      {Intrinsic::Pacia, "test_pacia"},    {Intrinsic::Pacib, "test_pacib"},
      {Intrinsic::Pacda, "test_pacda"},    {Intrinsic::Pacdb, "test_pacdb"},
      {Intrinsic::Autia, "test_autia"},    {Intrinsic::Autib, "test_autib"},
      {Intrinsic::Autda, "test_autda"},    {Intrinsic::Autdb, "test_autdb"},
      {Intrinsic::Xpaci, "test_xpaci"},    {Intrinsic::Xpacd, "test_xpacd"},
      {Intrinsic::A64_Pacga, "test_pacga"}};
  for (const auto &[Id, Name] : Cases) {
    auto F = pointerFunction(Id, Name);
    F.Entry += Functions.size() * 0x10;
    F.Blocks.front().StartAddr = F.Entry;
    Functions.push_back(std::move(F));
  }
  executeCProjections(Functions, (*Oracle)->getBuffer(), kSigningKeyTarget);
}

TEST(AArch64PAuthLRSemantics, NativeTwoModifierKeysAgreeWithBothCProjections) {
  if (!hasNativePAuth())
    GTEST_SKIP() << "native pointer authentication requires Apple Silicon";
  bool ExecutionAvailable = false;
  // The process must both execute PAuth_LR and have active IA/IB keys. A
  // successful ordinary arm64 process cannot establish either condition.
  compileArm64eC(R"(
    #include <stdint.h>
    int main(void) {
      int changed_a = 0, changed_b = 0;
      for (uint64_t i = 0; i != 64; ++i) {
        uint64_t a, b, pointer = 0x12340 + i * 16;
        __asm__ volatile("mov x17, %2\nmov x16, %3\nmov x15, %3\n"
                         ".inst 0xdac18bfe\nmov %0, x17\n"
                         "mov x17, %2\n.inst 0xdac18ffe\nmov %1, x17"
                         : "=&r"(a), "=r"(b) : "r"(pointer), "r"(i)
                         : "x15", "x16", "x17");
        changed_a |= a != pointer;
        changed_b |= b != pointer;
      }
      return !(changed_a && changed_b);
    }
  )",
                 {}, /*EmitLLVM=*/false, "arm64e-apple-macos",
                 &ExecutionAvailable);
  if (HasFatalFailure())
    return;
  if (!ExecutionAvailable)
    GTEST_SKIP() << "this host cannot execute arm64e PAuth_LR with active keys";
  const auto Oracle = llvm::MemoryBuffer::getFile(
      std::string(NEVERD_ARM64E_TEST_HEADERS) + "/pauth_lr_oracle.c");
  ASSERT_TRUE(Oracle);
  std::vector<MedFunc> Functions;
  for (const auto &[Id, Name] :
       {std::pair{Intrinsic::A64_Pacia2, "test_pacia2"},
        std::pair{Intrinsic::A64_Pacib2, "test_pacib2"},
        std::pair{Intrinsic::A64_Autia2, "test_autia2"},
        std::pair{Intrinsic::A64_Autib2, "test_autib2"},
        std::pair{Intrinsic::A64_AuthReturnIA2, "test_returnia2"},
        std::pair{Intrinsic::A64_AuthReturnIB2, "test_returnib2"}}) {
    auto F = pointerFunction(Id, Name);
    F.Entry += Functions.size() * 0x10;
    F.Blocks.front().StartAddr = F.Entry;
    Functions.push_back(std::move(F));
  }
  executeCProjections(Functions, (*Oracle)->getBuffer(), "arm64e-apple-macos");
}

TEST(AArch64PACMSemantics, PendingPrefixIsNeverAnOrdinaryHint) {
  for (bool Fast : {false, true}) {
    const auto Ops = lift(kAArch64PACMEncoding, Fast);
    ASSERT_EQ(Ops.size(), 1u);
    expectIntrinsic(Ops.front(), Intrinsic::A64_PACMPending, 0);
    EXPECT_FALSE(lowAArch64PAuthShapeIsValid(Ops.front()));
  }
}

TEST(AArch64PAuthLRSemantics, ExplicitFormsRetainBothModifiersAndOriginalPC) {
  const struct {
    uint32_t Word;
    Intrinsic Id;
    uint64_t Pointer;
    uint64_t FirstModifier;
    uint64_t SecondModifier;
    bool Constant;
    bool Return;
  } Cases[] = {
      {0xdac18bfe, Intrinsic::A64_Pacia2, 136, 128, 120, false, false},
      {0xdac18ffe, Intrinsic::A64_Pacib2, 136, 128, 120, false, false},
      {0xdac1bbfe, Intrinsic::A64_Autia2, 136, 128, 120, false, false},
      {0xdac1bffe, Intrinsic::A64_Autib2, 136, 128, 120, false, false},
      {0xdac1a3fe, Intrinsic::A64_Pacia2, 240, 248, 0x1000, true, false},
      {0xdac1a7fe, Intrinsic::A64_Pacib2, 240, 248, 0x1000, true, false},
      {0xdac183fe, Intrinsic::A64_Pacia2, 240, 248, 0x1000, true, false},
      {0xdac187fe, Intrinsic::A64_Pacib2, 240, 248, 0x1000, true, false},
      {0xf380001f, Intrinsic::A64_Autia2, 240, 248, 0x1000, true, false},
      {0xf3a0003f, Intrinsic::A64_Autib2, 240, 248, 0xffc, true, false},
      {0xf39fffff, Intrinsic::A64_Autia2, 240, 248, uint64_t(0x1000 - 262140),
       true, false},
      {0xdac1901e, Intrinsic::A64_Autia2, 240, 248, 0, false, false},
      {0xdac197de, Intrinsic::A64_Autib2, 240, 248, 240, false, false},
      {0xdac193fe, Intrinsic::A64_Autia2, 240, 248, 0, true, false},
      {0xdac197fe, Intrinsic::A64_Autib2, 240, 248, 0, true, false},
      {0x5500001f, Intrinsic::A64_AuthReturnIA2, 240, 248, 0x1000, true, true},
      {0x5520003f, Intrinsic::A64_AuthReturnIB2, 240, 248, 0xffc, true, true},
      {0x551fffff, Intrinsic::A64_AuthReturnIA2, 240, 248,
       uint64_t(0x1000 - 262140), true, true},
      {0xd65f0be2, Intrinsic::A64_AuthReturnIA2, 240, 248, 16, false, true},
      {0xd65f0ffe, Intrinsic::A64_AuthReturnIB2, 240, 248, 240, false, true},
  };
  for (bool Fast : {false, true})
    for (const auto &Case : Cases) {
      SCOPED_TRACE(testing::Message() << Case.Word << " " << Fast);
      const auto Ops = lift(Case.Word, Fast);
      ASSERT_EQ(Ops.size(), Case.Return ? 2u : 1u);
      expectIntrinsic(Ops[0], Case.Id, 3);
      expectRegister(Ops[0].Inputs[1], Case.Pointer);
      expectRegister(Ops[0].Inputs[2], Case.FirstModifier);
      EXPECT_EQ(Ops[0].Inputs[3].isConst(), Case.Constant);
      EXPECT_EQ(Ops[0].Inputs[3].Offset, Case.SecondModifier);
      EXPECT_EQ(Ops[0].Inputs[3].Size, 8);
      EXPECT_TRUE(lowAArch64PAuthShapeIsValid(Ops[0]));
      EXPECT_EQ(a64fast::isTerminatorWord(Case.Word), Case.Return);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::AArch64));
      uint8_t Bytes[4];
      writeLE<uint32_t>(Bytes, Case.Word);
      DecodedInsn Insn;
      ASSERT_EQ(Dec.decodeOne(Bytes, 4, 0x1000, Insn), 4);
      EXPECT_EQ(Dec.isFunctionTerminator(Insn), Case.Return);
      if (Case.Return) {
        EXPECT_TRUE(Ops[0].Output.isTemp());
        EXPECT_EQ(Ops[1].Opcode, NdOp::RETURN);
        EXPECT_EQ(Ops[1].Inputs[0], Ops[0].Output);
      } else
        EXPECT_EQ(Ops[0].Output, Ops[0].Inputs[1]);
      for (unsigned Index = 0; Index < 4; ++Index) {
        auto Bad = Ops[0];
        (Index ? Bad.Inputs[Index] : Bad.Output).Size = 4;
        EXPECT_FALSE(lowAArch64PAuthShapeIsValid(Bad));
      }
    }
}

TEST(AArch64PAuthLRSemantics, LLVMAndBothCRoutesPreserveThreeInputsAndFaults) {
  const struct {
    Intrinsic Id;
    const char *MachineAsm;
    bool Return;
  } Cases[] = {
      {Intrinsic::A64_Pacia2, ".inst 0xdac18bfe", false},
      {Intrinsic::A64_Pacib2, ".inst 0xdac18ffe", false},
      {Intrinsic::A64_Autia2, ".inst 0xdac1bbfe", false},
      {Intrinsic::A64_Autib2, ".inst 0xdac1bffe", false},
      {Intrinsic::A64_AuthReturnIA2, ".inst 0xdac1bbfe", true},
      {Intrinsic::A64_AuthReturnIB2, ".inst 0xdac1bffe", true},
  };
  for (const auto &Case : Cases)
    for (bool Dead : {false, true}) {
      SCOPED_TRACE(testing::Message() << intrinsicName(Case.Id) << " " << Dead);
      auto F = pointerFunction(Case.Id, "authenticate_three");
      const auto &Op = F.Blocks[0].Ops[0];
      ASSERT_TRUE(aarch64PAuthMedShapeIsValid(Op));
      EXPECT_FALSE(endsCallArgumentSetup(Op));
      for (unsigned Index = 0; Index < 4; ++Index) {
        auto Bad = Op;
        (Index ? Bad.Inputs[Index] : Bad.Output).Size = 4;
        EXPECT_FALSE(aarch64PAuthMedShapeIsValid(Bad));
        EXPECT_TRUE(endsCallArgumentSetup(Bad));
      }
      if (Dead)
        F.Blocks[0].Ops.back().Inputs[0] = MedVar::makeConst(42, 8);
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({F}, Context, "pauth-lr", Arch::AArch64);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      optimize(*Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      const auto *Fn = Module->getFunction(F.Name);
      ASSERT_TRUE(Fn);
      unsigned Count = 0;
      for (const auto &Block : *Fn)
        for (const auto &Insn : Block) {
          const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
          const auto *Asm =
              Call ? llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())
                   : nullptr;
          if (!Asm)
            continue;
          ++Count;
          EXPECT_TRUE(Asm->hasSideEffects());
          EXPECT_EQ(Asm->getAsmString().find(Case.MachineAsm), 0u);
          EXPECT_EQ(Asm->getAsmString().find("brk #0xc470") !=
                        std::string::npos,
                    Case.Return);
          EXPECT_EQ(Asm->getConstraintString(),
                    Case.Return ? "={x17},0,{x16},{x15},~{x14},~{cc}"
                                : "={x17},0,{x16},{x15}");
          ASSERT_EQ(Call->arg_size(), 3u);
          for (unsigned Index = 0; Index < 3; ++Index)
            EXPECT_EQ(Call->getArgOperand(Index), Fn->getArg(Index));
          const auto *Return =
              llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
          ASSERT_TRUE(Return);
          if (!Dead)
            EXPECT_EQ(Return->getReturnValue(), Call);
        }
      EXPECT_EQ(Count, 1u);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      for (bool LLVMRoute : {false, true}) {
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVMRoute)
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
        else {
          const auto High = MedToHighConverter().convert(F, Arch::AArch64);
          ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
        }
        EXPECT_NE(Source.find(Case.MachineAsm), std::string::npos) << Source;
        EXPECT_EQ(Source.find("unknown_intrinsic"), std::string::npos)
            << Source;
        compileArm64eC(Source);
      }
      const auto Object =
          Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
      EXPECT_TRUE(Object.Success);
    }
}

TEST(AArch64PAuthLRSemantics, CTemporariesCannotCaptureAnyModifier) {
  const std::vector<std::string> Args = {"__pointer", "__pointer_",
                                         "__pointer__"};
  for (const auto &Operation : kAArch64PAuthLROperations)
    for (bool LLVMRoute : {false, true}) {
      bool HasIntrinsics = false;
      const std::string Body =
          LLVMRoute ? renderARMInlineAsm(Arch::AArch64, Operation.llvmAsm(), "",
                                         "__pointer", true, Args)
                              .Code +
                          "return __pointer;"
                    : "return " +
                          renderARMIntrinsicCall(Operation.Id, Args, 8,
                                                 HasIntrinsics) +
                          ";";
      compileArm64eC(
          "#include <stdint.h>\nuint64_t capture(uint64_t __pointer, uint64_t "
          "__pointer_, uint64_t __pointer__) { " +
              Body + " }\n",
          [&](llvm::StringRef Path) {
            llvm::LLVMContext Context;
            llvm::SMDiagnostic Diagnostic;
            auto Module = llvm::parseIRFile(Path, Diagnostic, Context);
            ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
            auto *Fn = Module->getFunction("capture");
            ASSERT_TRUE(Fn);
            Fn->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*Module);
            ASSERT_FALSE(llvm::verifyModule(*Module));
            unsigned Count = 0;
            for (const auto &Block : *Fn)
              for (const auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                const auto *Asm = Call ? llvm::dyn_cast<llvm::InlineAsm>(
                                             Call->getCalledOperand())
                                       : nullptr;
                if (!Asm || Asm->getAsmString().find(Operation.MachineAsm) ==
                                std::string::npos)
                  continue;
                ++Count;
                ASSERT_EQ(Call->arg_size(), 3u);
                for (unsigned Index = 0; Index < 3; ++Index)
                  EXPECT_EQ(Call->getArgOperand(Index), Fn->getArg(Index));
                const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                ASSERT_TRUE(Return);
                EXPECT_EQ(Return->getReturnValue(), Call);
              }
            EXPECT_EQ(Count, 1u);
          },
          /*EmitLLVM=*/true);
    }
}

TEST(AArch64PAuthSemantics, AuthenticatedTailCallsRequireARestoredFrame) {
  const struct {
    bool TailCall;
    std::vector<uint32_t> Words;
  } Cases[] = {
      {true, {0xd61f081f}},                          // braaz x0
      {true, {0xd71f081f}},                          // braa x0, sp
      {true, {0xd503237f, 0xd50323ff, 0xd61f081f}},  // matching PACIBSP/AUTIBSP
      {false, {0xd10043ff, 0xd61f081f}},             // live frame
      {false, {0x910003e0, 0xd61f081f}},             // stack target
      {false, {0x10000020, 0xd61f081f}},             // local code target
      {false, {0xd503237f, 0xd61f081f}},             // link still signed
      {false, {0xd503237f, 0xd50323bf, 0xd61f081f}}, // wrong authentication key
      {false,
       {0xd503237f, 0xd10043ff, 0xd50323ff, 0x910043ff,
        0xd61f081f}},        // wrong modifier
      {false, {0xd61f0bdf}}, // braaz x30 returns
      {true,
       {0xdac1a3fe, 0xf380003f, 0xd61f081f}}, // matching PACIASPPC/AUTIASPPC
      {true, {0xdac1a7fe, 0xf3a0003f, 0xd61f081f}}, // matching B key
      {true,
       {0xdac1a3fe, 0x10ffffe1, 0xdac1903e, 0xd61f081f}}, // ADR of signing PC
      {false, {0xdac1a3fe, 0xd61f081f}}, // second-modifier link still signed
      {false, {0xdac1a3fe, 0xf380001f, 0xd61f081f}}, // wrong signing PC
      {false, {0xdac1a3fe, 0xf3a0003f, 0xd61f081f}}, // wrong key
      {false, {0xdac1a3fe, 0xd50323bf, 0xd61f081f}}, // missing second modifier
      {false, {0xd503233f, 0xf380003f, 0xd61f081f}}, // extra second modifier
#define NEVERD_A64_PAUTH_FRAME_CASE(TailCall, ...) {TailCall, {__VA_ARGS__}},
#include "AArch64PAuthFrameFixtures.def"
  };
  unsigned CaseIndex = 0;
  for (const auto &Case : Cases) {
    SCOPED_TRACE(CaseIndex++);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = Case.Words.size() * 4;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    for (size_t I = 0; I < Case.Words.size(); ++I)
      writeLE<uint32_t>(Text.Data.data() + I * 4, Case.Words[I]);
    Image.Segments.push_back(Text);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    Dec.setStrict(true);
    auto Low = CFGBuilder().build(Image, Dec, Text.VA, "authenticated_tail");
    unsigned Calls = 0, Branches = 0;
    for (const auto &Block : Low.Blocks)
      for (const auto &Op : Block.Ops) {
        Calls += Op.Opcode == NdOp::INDIR_CALL;
        Branches += Op.Opcode == NdOp::INDIR_BR;
      }
    EXPECT_EQ(Calls, Case.TailCall ? 1u : 0u);
    EXPECT_EQ(Branches, Case.TailCall ? 0u : 1u);
  }
}

TEST(AArch64PAuthSemantics,
     CompilerGeneratedArm64eObjectRetainsAuthentication) {
  compileArm64eC(
      R"(
typedef unsigned long long word;
__attribute__((noinline)) word target(word x) { return x + 1; }
word (*const slot)(word) = target;
__attribute__((noinline)) word dispatch(word (*fn)(word), word x) {
  return fn(x);
}
__attribute__((noinline)) word dispatch_after_call(word (*fn)(word), word x) {
  return fn(target(x));
}
)",
      [](llvm::StringRef Path) {
        auto Image = MachOLoader().load(Path.str());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        ASSERT_TRUE(Image->isArm64e());
        ASSERT_FALSE(Image->MachOAuthenticatedPointers.empty());
        const auto *Dispatch = Image->findSymbol("_dispatch");
        ASSERT_TRUE(Dispatch);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::AArch64));
        Dec.setStrict(true);
        auto Low = CFGBuilder().build(*Image, Dec, Dispatch->Addr, "dispatch");
        LowToMedConverter Converter;
        Converter.setBinaryImage(&*Image);
        auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
        recoverCallAbi(Med, Arch::AArch64, {}, &*Image);
        unsigned Authentication = 0;
        for (const auto &Block : Med.Blocks)
          for (const auto &Op : Block.Ops)
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].ConstVal ==
                    static_cast<uint64_t>(Intrinsic::A64_AuthBranch))
              ++Authentication;
        EXPECT_GT(Authentication, 0);
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Med}, Context, "clang-arm64e", Arch::AArch64,
                                  {}, &*Image, BinaryFormat::MachO);
        ASSERT_TRUE(Module);
        EXPECT_FALSE(llvm::verifyModule(*Module));
        optimize(*Module);
        const auto *Fn = Module->getFunction("dispatch");
        ASSERT_TRUE(Fn);
        ASSERT_EQ(Fn->arg_size(), 2u);
        unsigned IndirectCalls = 0;
        for (const auto &Block : *Fn)
          for (const auto &Insn : Block)
            if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                Call && Call->isIndirectCall() && !Call->isInlineAsm()) {
              // Without a prototype, ABI recovery may retain extra live
              // registers. The explicit X0 argument must still be x.
              ASSERT_GE(Call->arg_size(), 1u);
              EXPECT_EQ(Call->getArgOperand(0), Fn->getArg(1));
              ++IndirectCalls;
            }
        EXPECT_EQ(IndirectCalls, 1);
        const auto *AfterCall = Image->findSymbol("_dispatch_after_call");
        ASSERT_TRUE(AfterCall);
        auto NonLeaf = CFGBuilder().build(*Image, Dec, AfterCall->Addr,
                                          "dispatch_after_call");
        unsigned TailCalls = 0, UnresolvedBranches = 0;
        bool HasFailureTrap = false;
        for (const auto &Block : NonLeaf.Blocks)
          for (const auto &Op : Block.Ops) {
            TailCalls += Op.Opcode == NdOp::INDIR_CALL;
            UnresolvedBranches += Op.Opcode == NdOp::INDIR_BR;
            HasFailureTrap |=
                Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Brk);
          }
        EXPECT_EQ(TailCalls, 1);
        EXPECT_EQ(UnresolvedBranches, 0);
        auto NonLeafMed =
            Converter.convert(NonLeaf, Arch::AArch64, BinaryFormat::MachO);
        const auto *Target = Image->findSymbol("_target");
        ASSERT_TRUE(Target);
        ASSERT_EQ(Target->Addr, 0u);
        const std::vector<std::pair<va_t, std::string>> Names = {
            {Target->Addr, Target->Name}};
        recoverCallAbi(NonLeafMed, Arch::AArch64,
                       {{Target->Addr, Target->Name}}, &*Image);
        auto NonLeafModule = MedLLVMEmitter().emit(
            {NonLeafMed}, Context, "clang-arm64e-nonleaf", Arch::AArch64, Names,
            &*Image, BinaryFormat::MachO);
        ASSERT_TRUE(NonLeafModule);
        optimize(*NonLeafModule);
        EXPECT_FALSE(llvm::verifyModule(*NonLeafModule));
        EXPECT_TRUE(NonLeafModule->getFunction("target"));
        const auto *NonLeafFn =
            NonLeafModule->getFunction("dispatch_after_call");
        ASSERT_TRUE(NonLeafFn);
        unsigned ForwardedCalls = 0;
        for (const auto &Block : *NonLeafFn)
          for (const auto &Insn : Block)
            if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                Call && Call->isIndirectCall() && !Call->isInlineAsm()) {
              ASSERT_GE(Call->arg_size(), 1u);
              const auto *Forwarded =
                  llvm::dyn_cast<llvm::CallBase>(Call->getArgOperand(0));
              ASSERT_TRUE(Forwarded);
              ASSERT_TRUE(Forwarded->getCalledFunction());
              EXPECT_EQ(Forwarded->getCalledFunction()->getName(), "target");
              ++ForwardedCalls;
            }
        EXPECT_EQ(ForwardedCalls, 1);
        CEmitterOptions Options;
        Options.TheArch = Arch::AArch64;
        Options.Format = BinaryFormat::MachO;
        std::string LowC, HighC;
        llvm::raw_string_ostream LowOS(LowC), HighOS(HighC);
        ASSERT_TRUE(LLVMCEmitter().emit(*NonLeafModule, LowOS, Options));
        const auto High =
            MedToHighConverter().convert(NonLeafMed, Arch::AArch64);
        ASSERT_TRUE(HighCEmitter().emit({High}, HighOS, Options));
        // Clang's optimized tail call observes AUT's result in a failure-bit
        // check. Keep that trap explicit. Its unoptimized normal-return
        // recipe can instead carry the return-protection attribute.
        if (HasFailureTrap) {
          EXPECT_EQ(HighC.find("branch-protection="), std::string::npos)
              << HighC;
          EXPECT_NE(HighC.find("__builtin_return_address(0)"),
                    std::string::npos)
              << HighC;
          EXPECT_NE(HighC.find("autib %0, %1"), std::string::npos) << HighC;
          EXPECT_NE(HighC.find("__builtin_trap()"), std::string::npos) << HighC;
        } else {
          EXPECT_NE(HighC.find("branch-protection=pac-ret+b-key"),
                    std::string::npos)
              << HighC;
          EXPECT_EQ(HighC.find("__builtin_return_address(0)"),
                    std::string::npos)
              << HighC;
        }
        // Authentication of the callback pointer is separate in both forms.
        EXPECT_NE(HighC.find("__builtin_ptrauth_auth"), std::string::npos);
        EXPECT_EQ(HighC.find("unknown register"), std::string::npos);
        compileArm64eC(LowC);
        compileArm64eC(HighC);
      });
}

namespace {
namespace pacm_fixture {
#define NEVERD_PACM_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define NEVERD_PACM_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64PACMFixtures.def"

BinaryImage image(llvm::ArrayRef<uint32_t> Words) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = kBase;
  Text.Size = Text.FileSz = Words.size() * kWordBytes;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  for (size_t I = 0; I < Words.size(); ++I)
    writeLE<uint32_t>(Text.Data.data() + I * kWordBytes, Words[I]);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

LowBlock instructionBlock(LowOp Op) {
  LowBlock Block;
  Block.StartAddr = Op.Addr = kBase;
  Block.EndAddr = kBase + kWordBytes;
  Block.Ops.push_back(Op);
  LowInstructionBoundary Boundary;
  Boundary.Address = Block.StartAddr;
  Boundary.Size = kWordBytes;
  Boundary.OpCount = Block.Ops.size();
  Block.InstructionBoundaries.push_back(Boundary);
  return Block;
}
} // namespace pacm_fixture
} // namespace

TEST(AArch64PACMSemantics, PointerOperandsRequireScalarValueKinds) {
  const Intrinsic Cases[] = {
#define NEVERD_PACM_SHAPE_ID(Id) Intrinsic::Id,
#include "AArch64PACMFixtures.def"
  };
  for (Intrinsic Id : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(Id));
    const auto Function = pointerFunction(Id, pacm_fixture::kShapeName);
    const auto &Op = Function.Blocks.front().Ops.front();
    ASSERT_TRUE(aarch64PAuthMedShapeIsValid(Op));
    for (unsigned Index = 1; Index < Op.NumInputs; ++Index) {
      SCOPED_TRACE(Index);
      // AuthBranch's key is an immediate, not a scalar value operand.
      if (Id == Intrinsic::A64_AuthBranch && Op.Inputs[Index].isConst())
        continue;
      for (auto Kind :
           {MedVar::Reg, MedVar::Temp, MedVar::Param, MedVar::Const}) {
        auto Valid = Op;
        Valid.Inputs[Index].Kind = Kind;
        EXPECT_TRUE(aarch64PAuthMedShapeIsValid(Valid));
      }
      for (auto Kind : {MedVar::Stack, MedVar::RetVal, MedVar::Flag,
                        MedVar::EHException, MedVar::EHSelector,
                        MedVar::SEHExceptionCode, MedVar::Unspecified}) {
        SCOPED_TRACE(Kind);
        auto Bad = Function;
        auto &Malformed = Bad.Blocks.front().Ops.front();
        Malformed.Inputs[Index].Kind = Kind;
        EXPECT_FALSE(aarch64PAuthMedShapeIsValid(Malformed));
        EXPECT_TRUE(endsCallArgumentSetup(Malformed));
        EXPECT_FALSE(verifyMedFunc(Bad, pacm_fixture::kShapeName));
      }
    }
  }
}

TEST(AArch64PACMSemantics, MalformedModesAndArityFailIRVerification) {
  enum class Invalid { ModeWidth, ModeKind, MissingMode, ExtraMode };
  for (const auto &Operation : kAArch64PAuthMOperations) {
    SCOPED_TRACE(Operation.Mnemonic);
    const auto Function =
        pointerFunction(Operation.Id, pacm_fixture::kShapeName);
    const auto &Med = Function.Blocks.front().Ops.front();
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = NdVar::tmp(0, kAArch64PAuthPointerBytes);
    Op.addInput(NdVar::cst(static_cast<uint64_t>(Operation.Id),
                           pacm_fixture::kIntrinsicIdBytes));
    for (unsigned Index = 1; Index < Med.NumInputs; ++Index)
      Op.addInput(NdVar::reg(Med.Inputs[Index].RegOff, Med.Inputs[Index].Size));
    const auto Block = pacm_fixture::instructionBlock(Op);
    auto Valid = validateLowInstructionBoundaries(Block);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    ASSERT_TRUE(verifyMedFunc(Function, pacm_fixture::kShapeName));
    for (auto Mutation : {Invalid::ModeWidth, Invalid::ModeKind,
                          Invalid::MissingMode, Invalid::ExtraMode}) {
      auto BadLow = Block;
      auto BadMed = Function;
      auto &Low = BadLow.Ops.front();
      auto &Med = BadMed.Blocks.front().Ops.front();
      switch (Mutation) {
      case Invalid::ModeWidth:
        Low.Inputs[kAArch64PACMOperandCount].Size =
            Med.Inputs[kAArch64PACMOperandCount].Size =
                pacm_fixture::kInvalidModeBytes;
        break;
      case Invalid::ModeKind:
        Low.Inputs[kAArch64PACMOperandCount].Space = VnodeSpace::RAM;
        Med.Inputs[kAArch64PACMOperandCount].Kind = MedVar::Stack;
        break;
      case Invalid::MissingMode:
        --Low.NumInputs;
        --Med.NumInputs;
        break;
      case Invalid::ExtraMode: {
        const auto LowMode = Low.Inputs[kAArch64PACMOperandCount];
        const auto MedMode = Med.Inputs[kAArch64PACMOperandCount];
        Low.addInput(LowMode);
        Med.addInput(MedMode);
        break;
      }
      }
      EXPECT_FALSE(lowAArch64PAuthShapeIsValid(Low));
      auto Error = validateLowInstructionBoundaries(BadLow);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_FALSE(aarch64PAuthMedShapeIsValid(Med));
      EXPECT_FALSE(verifyMedFunc(BadMed, pacm_fixture::kShapeName));
    }
  }
}

TEST(AArch64PACMSemantics, PendingMarkersRequireTheirLowIRShape) {
  const auto Ops = lift(pacm_fixture::kPACM, false);
  ASSERT_EQ(Ops.size(), 1u);
  const auto Block = pacm_fixture::instructionBlock(Ops.front());
  auto Valid = validateLowInstructionBoundaries(Block);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  using Mutation = void (*)(LowOp &);
  const Mutation Mutations[] = {
      [](LowOp &Op) { Op.Inputs[0].Size = pacm_fixture::kInvalidModeBytes; },
      [](LowOp &Op) { Op.Inputs[0].Space = VnodeSpace::REG; },
      [](LowOp &Op) { Op.Output = NdVar::tmp(0, kAArch64PAuthPointerBytes); },
      [](LowOp &Op) { --Op.NumInputs; },
      [](LowOp &Op) { Op.addInput(NdVar::cst(0, kAArch64PAuthPointerBytes)); },
      [](LowOp &Op) { Op.MemoryOrdering = NdMemoryOrdering::Acquire; },
      [](LowOp &Op) { Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS; },
  };
  for (Mutation Mutate : Mutations) {
    auto Bad = Block;
    Mutate(Bad.Ops.front());
    auto Error = validateLowInstructionBoundaries(Bad);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }

  LowFunc Low;
  Low.Entry = Block.StartAddr;
  Low.Blocks.push_back(Block);
  LowToMedConverter Converter;
  EXPECT_THROW(Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO),
               LowToMedConversionError);
  for (bool HasOutput : {false, true}) {
    auto Function =
        pointerFunction(Intrinsic::A64_AutiaM, pacm_fixture::kShapeName);
    auto &Pending = Function.Blocks.front().Ops.front();
    Pending.Inputs.front() =
        MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_PACMPending),
                          pacm_fixture::kIntrinsicIdBytes);
    if (!HasOutput) {
      Pending.Output = {};
      Pending.NumInputs = 1;
    }
    Function.Blocks.front().Ops.back().Inputs.front() =
        MedVar::makeConst(0, kAArch64PAuthPointerBytes);
    EXPECT_FALSE(verifyMedFunc(Function, pacm_fixture::kShapeName));
    llvm::LLVMContext Context;
    EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, Function.Name,
                                       Arch::AArch64),
                 std::runtime_error);
  }
}

TEST(AArch64PACMSemantics, AllConsumersCaptureOriginalImplicitOperands) {
  const struct {
    uint32_t Word;
    Intrinsic Id;
    uint64_t Pointer, First, Second;
    bool PC, Return;
  } Cases[] = {
#define NEVERD_PACM_CONSUMER(Word, Id, Pointer, Mod1, Mod2, PC, Return)        \
  {Word, Intrinsic::Id, Pointer, Mod1, Mod2, PC, Return},
#include "AArch64PACMFixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Word);
    auto Image = pacm_fixture::image(
        {pacm_fixture::kPACM, Case.Word, pacm_fixture::kReturn});
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    Dec.setStrict(true);
    const auto Low =
        CFGBuilder().build(Image, Dec, pacm_fixture::kBase, "prefixed");
    EXPECT_TRUE(Low.UnsupportedInstructionAddresses.empty());
    if (auto Error = validateLowInstructionBoundaries(Low))
      FAIL() << llvm::toString(std::move(Error));
    unsigned Count = 0;
    for (const auto &Block : Low.Blocks)
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::INTRINSIC)
          continue;
        ++Count;
        expectIntrinsic(Op, Case.Id, 4);
        EXPECT_TRUE(lowAArch64PAuthShapeIsValid(Op));
        expectRegister(Op.Inputs[1], Case.Pointer);
        expectRegister(Op.Inputs[2], Case.First);
        EXPECT_EQ(Op.Inputs[3].Offset, Case.Second);
        EXPECT_EQ(Op.Inputs[3].isConst(), Case.PC);
        EXPECT_EQ(Op.Inputs[4].Offset, 1u);
        EXPECT_TRUE(Op.Inputs[4].isConst());
      }
    EXPECT_EQ(Count, 1u);
    EXPECT_TRUE(Low.FunctionTemporaries.empty());
  }
}

TEST(AArch64PACMSemantics, ExecutedEdgesOwnPrefixState) {
  const struct {
    const char *Name;
    unsigned Count;
    bool Mixed;
    std::vector<uint32_t> Words;
  } Cases[] = {
#define NEVERD_PACM_FLOW(Name, Count, Mixed, ...)                              \
  {#Name, Count, Mixed, {__VA_ARGS__}},
#include "AArch64PACMFixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto Image = pacm_fixture::image(Case.Words);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    Dec.setStrict(true);
    CFGBuilder Builder;
    for (unsigned Attempt = 0; Attempt < 2; ++Attempt) {
      const auto Low =
          Builder.build(Image, Dec, pacm_fixture::kBase, Case.Name);
      EXPECT_TRUE(Low.UnsupportedInstructionAddresses.empty());
      if (auto Error = validateLowInstructionBoundaries(Low))
        FAIL() << llvm::toString(std::move(Error));
      unsigned Count = 0;
      for (const auto &Block : Low.Blocks)
        for (const auto &Op : Block.Ops) {
          if (Op.Opcode != NdOp::INTRINSIC)
            continue;
          const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
          EXPECT_NE(Id, Intrinsic::A64_PACMPending);
          if (!aarch64PAuthMOperation(Id))
            continue;
          ++Count;
          EXPECT_EQ(Op.Inputs[4].isTemp(), Case.Mixed);
        }
      EXPECT_EQ(Count, Case.Count);
      EXPECT_EQ(Low.FunctionTemporaries.size(), Case.Mixed ? 1u : 0u);
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto Med =
          Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Med}, Context, Case.Name, Arch::AArch64);
      ASSERT_TRUE(Module);
      EXPECT_FALSE(llvm::verifyModule(*Module));
      optimize(*Module);
      EXPECT_FALSE(llvm::verifyModule(*Module));
      if (Case.Mixed) {
        bool SawMode = false;
        for (const auto &Block : *Module->getFunction(Case.Name))
          for (const auto &Insn : Block) {
            const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
            const auto *Asm =
                Call ? llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())
                     : nullptr;
            if (!Asm || Asm->getAsmString().find(pacm_fixture::kPrefixLLVM) ==
                            std::string::npos)
              continue;
            ASSERT_EQ(Call->arg_size(), 4u);
            EXPECT_FALSE(llvm::isa<llvm::UndefValue>(Call->getArgOperand(3)));
            EXPECT_FALSE(llvm::isa<llvm::PoisonValue>(Call->getArgOperand(3)));
            // Loop peeling can specialize the first execution to one and
            // its backedge to zero; the semantic oracle checks both paths.
            if (const auto *Mode =
                    llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(3)))
              EXPECT_LE(Mode->getZExtValue(), 1u);
            SawMode = true;
          }
        EXPECT_TRUE(SawMode);
      }
      if (Attempt != 0)
        continue;
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      for (bool LLVMRoute : {false, true}) {
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVMRoute)
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
        else
          ASSERT_TRUE(HighCEmitter().emit(
              {MedToHighConverter().convert(Med, Arch::AArch64)}, Out,
              Options));
        compileArm64eC(Source);
      }
    }
  }
}

TEST(AArch64PACMSemantics, BothCRoutesAndLLVMKeepThePrefixAdjacent) {
  for (const auto &Case : kAArch64PAuthMOperations)
    for (bool Dead : {false, true}) {
      SCOPED_TRACE(testing::Message() << intrinsicName(Case.Id) << " " << Dead);
      auto F = pointerFunction(Case.Id, "prefixed_pointer");
      const auto &Op = F.Blocks[0].Ops[0];
      ASSERT_TRUE(aarch64PAuthMedShapeIsValid(Op));
      EXPECT_FALSE(endsCallArgumentSetup(Op));
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        auto Bad = Op;
        (I ? Bad.Inputs[I] : Bad.Output).Size = 4;
        EXPECT_FALSE(aarch64PAuthMedShapeIsValid(Bad));
      }
      if (Dead)
        F.Blocks[0].Ops.back().Inputs[0] = MedVar::makeConst(0, 8);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({F}, Context, F.Name, Arch::AArch64);
      ASSERT_TRUE(Module);
      EXPECT_FALSE(llvm::verifyModule(*Module));
      optimize(*Module);
      EXPECT_FALSE(llvm::verifyModule(*Module));
      unsigned Count = 0;
      for (const auto &Block : *Module->getFunction(F.Name))
        for (const auto &Insn : Block) {
          const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
          const auto *Asm =
              Call ? llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())
                   : nullptr;
          if (!Asm)
            continue;
          ++Count;
          EXPECT_TRUE(Asm->hasSideEffects());
          EXPECT_NE(Asm->getAsmString().find(pacm_fixture::kPrefixLLVM),
                    std::string::npos);
          EXPECT_EQ(Asm->getAsmString().find(pacm_fixture::kTrap) !=
                        std::string::npos,
                    Case.Return);
          ASSERT_EQ(Call->arg_size(), 4u);
          for (unsigned I = 0; I < 4; ++I)
            EXPECT_EQ(Call->getArgOperand(I),
                      Module->getFunction(F.Name)->getArg(I));
        }
      EXPECT_EQ(Count, 1u);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      for (bool LLVMRoute : {false, true}) {
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVMRoute)
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
        else
          ASSERT_TRUE(HighCEmitter().emit(
              {MedToHighConverter().convert(F, Arch::AArch64)}, Out, Options));
        EXPECT_NE(Source.find(pacm_fixture::kPrefixC), std::string::npos)
            << Source;
        compileArm64eC(Source);
      }
      EXPECT_TRUE(Codegen()
                      .compile(*Module, Arch::AArch64, BinaryFormat::MachO)
                      .Success);
    }
}

TEST(AArch64PACMSemantics, CTemporariesDoNotCaptureTheMode) {
  const std::vector<std::string> Args = {"__pointer", "__pointer_",
                                         "__pointer__", "__pointer___"};
  for (const auto &Operation : kAArch64PAuthMOperations)
    for (bool LLVMRoute : {false, true}) {
      bool HasIntrinsics = false;
      const std::string Body =
          LLVMRoute ? renderARMInlineAsm(Arch::AArch64, Operation.llvmAsm(), "",
                                         "__pointer", true, Args)
                              .Code +
                          "return __pointer;"
                    : "return " +
                          renderARMIntrinsicCall(Operation.Id, Args, 8,
                                                 HasIntrinsics) +
                          ";";
      compileArm64eC(
          std::string(pacm_fixture::kCaptureArgs) + Body + "}\n",
          [&](llvm::StringRef Path) {
            llvm::LLVMContext Context;
            llvm::SMDiagnostic Diagnostic;
            auto Module = llvm::parseIRFile(Path, Diagnostic, Context);
            ASSERT_TRUE(Module);
            auto *Fn = Module->getFunction("capture");
            ASSERT_TRUE(Fn);
            Fn->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*Module);
            unsigned Count = 0;
            for (const auto &Block : *Fn)
              for (const auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                if (!Call || !Call->isInlineAsm())
                  continue;
                ++Count;
                ASSERT_EQ(Call->arg_size(), 4u);
                for (unsigned I = 0; I < 4; ++I)
                  EXPECT_EQ(Call->getArgOperand(I), Fn->getArg(I));
              }
            EXPECT_EQ(Count, 1u);
          },
          /*EmitLLVM=*/true);
    }
}

TEST(AArch64PACMSemantics, NativePrefixModesAgreeWithBothCProjections) {
#define NEVERD_PACM_NATIVE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "AArch64PACMNative.def"
  if (!hasNativePAuth())
    GTEST_SKIP() << kNativeUnavailable;
  bool ExecutionAvailable = false;
  compileArm64eC(std::string(kOracle) + kProbeMain, {}, /*EmitLLVM=*/false,
                 kNativeTarget, &ExecutionAvailable);
  if (HasFatalFailure())
    return;
  if (!ExecutionAvailable)
    GTEST_SKIP() << kNativeUnavailable;
  std::vector<MedFunc> Functions;
  for (const auto &[Id, Name] :
       {std::pair{Intrinsic::A64_PaciaM, "test_pacia_m"},
        std::pair{Intrinsic::A64_PacibM, "test_pacib_m"},
        std::pair{Intrinsic::A64_AutiaM, "test_autia_m"},
        std::pair{Intrinsic::A64_AutibM, "test_autib_m"},
        std::pair{Intrinsic::A64_AuthReturnIAM, "test_returnia_m"},
        std::pair{Intrinsic::A64_AuthReturnIBM, "test_returnib_m"}}) {
    auto F = pointerFunction(Id, Name);
    F.Entry += Functions.size() * pacm_fixture::kWordBytes;
    F.Blocks.front().StartAddr = F.Entry;
    Functions.push_back(std::move(F));
  }
  executeCProjections(Functions, std::string(kOracle) + kCompareMain,
                      kNativeTarget);
}

TEST(AArch64PACMSemantics, IncomingModeSurvivesSSAAndLoopBackedges) {
  const struct {
    const char *Name;
    std::array<uint64_t, 3> Expected;
    std::vector<uint32_t> Words;
  } Cases[] = {
#define NEVERD_PACM_MODE_CASE(Name, Zero, One, Two, ...)                       \
  {#Name, {Zero, One, Two}, {__VA_ARGS__}},
#include "AArch64PACMFixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto Image = pacm_fixture::image(Case.Words);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    Dec.setStrict(true);
    auto Low = CFGBuilder().build(Image, Dec, pacm_fixture::kBase, Case.Name);
    ASSERT_TRUE(Low.UnsupportedInstructionAddresses.empty());
    unsigned Observed = 0;
    for (auto &Block : Low.Blocks)
      for (auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::INTRINSIC ||
            !aarch64PAuthMOperation(
                static_cast<Intrinsic>(Op.Inputs[0].Offset)))
          continue;
        ++Observed;
        Op.Opcode = NdOp::COPY;
        Op.Inputs[0] = Op.Inputs[4];
        Op.NumInputs = 1;
      }
    ASSERT_EQ(Observed, 1u);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Med}, Context, Case.Name, Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    auto *Fn = Module->getFunction(Case.Name);
    ASSERT_TRUE(Fn);
    ASSERT_EQ(Fn->arg_size(), 1u);
    Fn->addFnAttr(llvm::Attribute::AlwaysInline);
    std::vector<llvm::Function *> Observers;
    for (unsigned I = 0; I < Case.Expected.size(); ++I) {
      auto *Observer = llvm::Function::Create(
          llvm::FunctionType::get(Fn->getReturnType(), false),
          llvm::GlobalValue::ExternalLinkage, "observe", *Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Observer));
      auto *Input = llvm::ConstantInt::get(Fn->getArg(0)->getType(), I);
      Builder.CreateRet(Builder.CreateCall(Fn, {Input}));
      Observers.push_back(Observer);
    }
    optimize(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    for (unsigned I = 0; I < Observers.size(); ++I) {
      ASSERT_EQ(Observers[I]->size(), 1u);
      const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
          Observers[I]->front().getTerminator());
      ASSERT_TRUE(Return);
      const auto *Result =
          llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
      ASSERT_TRUE(Result);
      EXPECT_EQ(Result->getZExtValue(), Case.Expected[I]);
    }
  }
}

TEST(AArch64PACMSemantics, UnresolvedEntryStateCannotEscapeIntoSource) {
  const struct {
    unsigned EntryOffset;
    std::vector<uint32_t> Words;
  } Cases[] = {
#define NEVERD_PACM_UNRESOLVED(Offset, ...) {Offset, {__VA_ARGS__}},
#include "AArch64PACMFixtures.def"
  };
  for (const auto &Case : Cases) {
    auto Image = pacm_fixture::image(Case.Words);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    Dec.setStrict(true);
    const auto Low =
        CFGBuilder().build(Image, Dec, pacm_fixture::kBase + Case.EntryOffset,
                           "unresolved_prefix");
    EXPECT_FALSE(Low.UnsupportedInstructionAddresses.empty());
    LowToMedConverter Converter;
    EXPECT_THROW(Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO),
                 LowToMedConversionError);
  }
  auto Med = pointerFunction(Intrinsic::A64_AutiaM, "pending_prefix");
  Med.Blocks.front().Ops.front().Inputs[0] =
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::A64_PACMPending), 2);
  llvm::LLVMContext Context;
  EXPECT_THROW(MedLLVMEmitter().emit({Med}, Context, Med.Name, Arch::AArch64),
               std::runtime_error);
  bool HasIntrinsics = false;
  EXPECT_THROW(
      renderARMIntrinsicCall(Intrinsic::A64_PACMPending, {}, 0, HasIntrinsics),
      std::invalid_argument);
}

TEST(AArch64PACMSemantics, PrefixedReturnsKeepX0AndTheOriginalSigningPC) {
  const struct {
    const char *Name;
    uint64_t Value;
    std::vector<uint32_t> Words;
  } Cases[] = {
#define NEVERD_PACM_RETURN_CASE(Name, Value, ...) {#Name, Value, {__VA_ARGS__}},
#include "AArch64PACMFixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto Image = pacm_fixture::image(Case.Words);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    Dec.setStrict(true);
    const auto Low =
        CFGBuilder().build(Image, Dec, pacm_fixture::kBase, Case.Name);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Med}, Context, Case.Name, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimize(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    unsigned Operations = 0, Returns = 0;
    for (const auto &Block : *Module->getFunction(Case.Name))
      for (const auto &Insn : Block) {
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
            Call && Call->isInlineAsm()) {
          ++Operations;
          ASSERT_EQ(Call->arg_size(), 4u);
          const auto *PC =
              llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(2));
          ASSERT_TRUE(PC);
          EXPECT_EQ(PC->getZExtValue(),
                    pacm_fixture::kBase + pacm_fixture::kWordBytes);
        }
        if (const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Insn)) {
          ++Returns;
          const auto *Value =
              llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
          ASSERT_TRUE(Value);
          EXPECT_EQ(Value->getZExtValue(), Case.Value);
        }
      }
    EXPECT_EQ(Operations, 2u);
    EXPECT_EQ(Returns, 1u);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    for (bool LLVMRoute : {false, true}) {
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute)
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      else
        ASSERT_TRUE(HighCEmitter().emit(
            {MedToHighConverter().convert(Med, Arch::AArch64)}, Out, Options));
      compileArm64eC(Source);
    }
  }
}

namespace {
namespace exception_return_fixture {
#define NEVERD_A64_ERET_VALUE(Type, Name, Value)                               \
  inline constexpr Type Name = Value;
#define NEVERD_A64_ERET_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64ExceptionReturnFixtures.def"

LowFunc lowFunction(bool HasOutput) {
  LowFunc Function;
  Function.Name = kFunctionName;
  Function.Entry = kBase;
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = kBase;
  Op.addInput(
      NdVar::cst(static_cast<uint64_t>(Intrinsic::Eret), kIntrinsicIdBytes));
  if (HasOutput)
    Op.Output = NdVar::reg(a64reg::X0, kPointerBytes);
  LowBlock Block;
  Block.StartAddr = kBase;
  Block.EndAddr = kBase + kWordBytes;
  Block.Ops.push_back(Op);
  LowInstructionBoundary Boundary;
  Boundary.Address = kBase;
  Boundary.Size = kWordBytes;
  Boundary.OpCount = Block.Ops.size();
  Block.InstructionBoundaries.push_back(Boundary);
  Function.Blocks.push_back(Block);
  return Function;
}

MedFunc medFunction(bool HasOutput) {
  MedFunc Function;
  Function.Name = kFunctionName;
  Function.Entry = kBase;
  Function.ReturnType = NdType::makeVoid();
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = kBase;
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Eret),
                                kIntrinsicIdBytes));
  if (HasOutput) {
    Op.Output.Kind = MedVar::Reg;
    Op.Output.TheArch = Arch::AArch64;
    Op.Output.Size = kPointerBytes;
    Op.Output.RegOff = a64reg::X0;
    Op.Output.SSAVer = 1;
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Function.Blocks.emplace_back();
  Function.Blocks.front().Id = 0;
  Function.Blocks.front().StartAddr = kBase;
  Function.Blocks.front().Ops = {Op, Return};
  return Function;
}
} // namespace exception_return_fixture
} // namespace

TEST(AArch64ExceptionReturnSemantics, RawInstructionsNeverPublishOrdinaryIR) {
  using namespace exception_return_fixture;
  const uint32_t Words[] = {
#define NEVERD_A64_ERET_WORD(Word) Word,
#include "AArch64ExceptionReturnFixtures.def"
  };
  for (uint32_t Word : Words)
    for (bool Strict : {false, true})
      for (bool Fast : {false, true}) {
        SCOPED_TRACE(testing::Message() << Word << Strict << Fast);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::AArch64));
        Dec.setStrict(Strict);
        std::array<uint8_t, kWordBytes> Bytes;
        writeLE<uint32_t>(Bytes.data(), Word);
        DecodedInsn Insn;
        ASSERT_EQ(
            Fast ? Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kBase, Insn)
                 : Dec.decodeOne(Bytes.data(), Bytes.size(), kBase, Insn),
            kWordBytes);
        std::vector<LowOp> Ops;
        EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
  for (bool Fast : {false, true}) {
    const auto Ops = lift(kReturn, Fast);
    ASSERT_FALSE(Ops.empty());
    EXPECT_EQ(Ops.back().Opcode, NdOp::RETURN);
  }
}

TEST(AArch64ExceptionReturnSemantics, HandwrittenIRCannotReachLLVMFallback) {
  using namespace exception_return_fixture;
  for (bool HasOutput : {false, true}) {
    const auto Low = lowFunction(HasOutput);
    auto Error = validateLowInstructionBoundaries(Low.Blocks.front());
    ASSERT_TRUE(bool(Error));
    EXPECT_EQ(llvm::toString(std::move(Error)),
              kAArch64ExceptionReturnUnsupported);
    for (Arch Target : {Arch::AArch64, Arch::X64}) {
      LowToMedConverter Converter;
      EXPECT_THROW(Converter.convert(Low, Target, BinaryFormat::MachO),
                   LowToMedConversionError);
      const auto Function = medFunction(HasOutput);
      EXPECT_FALSE(verifyMedFunc(Function, kFunctionName));
      for (bool Masked : {false, true}) {
        llvm::LLVMContext Context;
        const std::vector<char> Mask{0};
        EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, Function.Name,
                                           Target, {}, nullptr,
                                           BinaryFormat::MachO, false,
                                           Masked ? &Mask : nullptr),
                     std::runtime_error);
      }
    }
  }
  auto Ordinary = medFunction(false);
  Ordinary.Blocks.front().Ops.erase(Ordinary.Blocks.front().Ops.begin());
  EXPECT_TRUE(verifyMedFunc(Ordinary, kFunctionName));
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Ordinary}, Context, Ordinary.Name, Arch::AArch64);
  ASSERT_TRUE(Module);
  EXPECT_FALSE(llvm::verifyModule(*Module));
}

TEST(AArch64ExceptionReturnSemantics, DirectCRenderingCannotEmitBareAssembly) {
  using namespace exception_return_fixture;
  for (uint16_t ResultBytes : {0u, kPointerBytes}) {
    bool HasIntrinsics = false;
    EXPECT_THROW(
        renderARMIntrinsicCall(Intrinsic::Eret, {}, ResultBytes, HasIntrinsics),
        std::invalid_argument);
    for (Arch Target : {Arch::AArch64, Arch::ARM, Arch::X64, Arch::X86}) {
      EXPECT_THROW(renderIntrinsicCall(Intrinsic::Eret, Target, {}, ResultBytes,
                                       HasIntrinsics),
                   std::invalid_argument);
      EXPECT_THROW(MultiOutputRender{}(Target, Intrinsic::Eret, {}, {}, {}, {}),
                   std::invalid_argument);
    }
    EXPECT_FALSE(HasIntrinsics);
  }
}

TEST(AArch64ExceptionReturnSemantics, HiddenHighIRCannotPublishSource) {
  using namespace exception_return_fixture;
  for (bool Hidden : {false, true})
    for (bool HasOutput : {false, true}) {
      HighFunc Function;
      Function.Entry = kBase;
      Function.Name = kFunctionName;
      Function.ReturnType = NdType::makeVoid();
      HighStmt Invoke;
      Invoke.Kind = StmtKind::ExprStmt;
      Invoke.Val = std::make_shared<HighExpr>();
      Invoke.Val->Kind = ExprKind::Call;
      Invoke.Val->Type = HasOutput ? NdType::makeInt(kPointerBytes, false)
                                   : NdType::makeVoid();
      Invoke.Val->IntrinsicId = Intrinsic::Eret;
      if (Hidden) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        Branch.Cond = HighExpr::makeConst(0, 1);
        Branch.Body.push_back(Invoke);
        Function.Body.push_back(Branch);
      } else
        Function.Body.push_back(Invoke);
      for (Arch Target : {Arch::AArch64, Arch::X64}) {
        CEmitterOptions Options;
        Options.TheArch = Target;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        EXPECT_THROW(HighCEmitter().emit({Function}, Out, Options),
                     std::invalid_argument);
        EXPECT_TRUE(Source.empty());
      }
    }
}
