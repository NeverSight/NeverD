//===- MicroMIPSPipelineTests.cpp - Compressed source contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise ELF-to-source mode retention and reject numeric code identities
/// until the target compiler preserves the architectural callable tag.
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <stdexcept>

using namespace neverd;

namespace {

class MicroMIPSPipeline : public ::testing::TestWithParam<Arch> {};

void checkHalfwordAlignedALUSources(Arch Target, unsigned Revision) {
  mips_test::ELFBytes File(Target);
  const bool R6 = Revision == 6;
  if (File.Is64) {
    constexpr va_t Base = 0x100010000;
    File.put(48, R6 ? 0xa2000000u : 0x82000000u, 4);
    File.put(64 + 16, Base, 8);
    File.put(64 + 24, Base, 8);
    File.put(24, Base + 0x101, 8);
  } else {
    File.put(36, R6 ? 0x92001000u : 0x72001000u, 4);
    File.put(24, 0x10101, 4);
  }
  // MOVE16 v0,a0; [D]ADDIU v0,v0,1 at a halfword boundary; JRC[16] ra.
  File.put(0x100, 0x0c44, 2);
  File.put(0x102, File.Is64 ? 0x5c42 : 0x3042, 2);
  File.put(0x104, 1, 2);
  File.put(0x106, R6 ? 0x47e3 : 0x45bf, 2);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Symbol = Symbol::makeFunc(Image->Entry, 8);
  Symbol.Name = "micro_increment";
  Image->Symbols.push_back(std::move(Symbol));
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Image->Entry};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_EQ(Result.MedFuncs.front().Params.size(), 1u);
    EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
    CEmitterOptions Emit;
    Emit.TheArch = Target;
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &*Image;
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      const auto *Body = Result.LlvmModule->getFunction("micro_increment");
      ASSERT_NE(Body, nullptr);
      EXPECT_TRUE(Body->hasFnAttribute("micromips"));
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, Emit));
    } else {
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
    }
    EXPECT_NE(Text.find("__attribute__((micromips,nomips16))"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("__attribute__((micromips))"), std::string::npos);
    const auto PreludeEnd = Text.find("#endif\n\n");
    ASSERT_NE(PreludeEnd, std::string::npos);
    EXPECT_NE(Text.find("NEVERD_MICROMIPS ", PreludeEnd), std::string::npos);
  }
}

TEST_P(MicroMIPSPipeline, HalfwordAlignedALUReachesBothSourcesWithMode) {
  checkHalfwordAlignedALUSources(GetParam(), 2);
  checkHalfwordAlignedALUSources(GetParam(), 6);
}

TEST(MicroMIPS64Pipeline, HighELFEntryReachesBothSources) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Revision : {2, 6}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Target) << " r" << Revision);
      checkHalfwordAlignedALUSources(Target, Revision);
    }
}

BinaryImage mixedImage(Arch Target) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Mode = InstructionMode::MixedMIPS;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits32;
  Image.MIPS.emplace();
  Image.MIPS->HasMicroMIPS = true;
  Image.MIPS->CodeModeEntries = {{0x1000, InstructionMode::MIPSClassic},
                                 {0x1100, InstructionMode::MicroMIPS}};
  Segment Text;
  Text.VA = 0x1000;
  Text.Data.resize(0x200);
  Text.Size = Text.FileSz = Text.Data.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Text));
  auto Symbol = Symbol::makeFunc(0x1100, 4);
  Symbol.Name = "micro_target";
  Image.Symbols.push_back(std::move(Symbol));
  return Image;
}

TEST_P(MicroMIPSPipeline, HighCDoesNotConfuseCodeBytesAndCallableValues) {
  auto Image = mixedImage(GetParam());
  using P = ConstantAddressProvenance;
  for (P Provenance : {P::CodeAddress, P::Address, P::DataAddress, P::Scalar})
    for (va_t Value : {0x1100u, 0x1101u}) {
      SCOPED_TRACE(int(Provenance));
      SCOPED_TRACE(Value);
      HighFunc Function;
      Function.Entry = 0x1000;
      Function.Name = "numeric_value";
      Function.ReturnType = NdType::makeInt(4, false);
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeConst(Value, 4, Provenance);
      Function.Body.push_back(std::move(Return));
      CEmitterOptions Emit;
      Emit.TheArch = GetParam();
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &Image;
      std::string Text;
      llvm::raw_string_ostream Output(Text);
      if (Provenance == P::CodeAddress ||
          (Provenance == P::Address && Value == 0x1101)) {
        EXPECT_THROW(HighCEmitter().emit({Function}, Output, Emit),
                     std::invalid_argument);
      } else {
        EXPECT_TRUE(HighCEmitter().emit({Function}, Output, Emit)) << Text;
        EXPECT_EQ(Text.find("micro_target"), std::string::npos) << Text;
      }
      // A data/code-byte relation may require its own original-image backing;
      // it must never silently acquire the callable's mode-tagged identity.
    }
}

TEST_P(MicroMIPSPipeline, LLVMRejectsNumericMicroIdentitiesBeforeOptimization) {
  auto Image = mixedImage(GetParam());
  using P = ConstantAddressProvenance;
  for (P Provenance : {P::CodeAddress, P::Address}) {
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "numeric_value";
    Function.ReturnType = NdType::makeInt(4, false);
    Function.Blocks.emplace_back();
    auto &Block = Function.Blocks.back();
    Block.Id = 0;
    Block.StartAddr = 0x1000;
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output.Kind = MedVar::Reg;
    Copy.Output.RegOff = mipsreg::V0;
    Copy.Output.Size = 4;
    Copy.Output.TheArch = GetParam();
    Copy.Output.Id = 1;
    Copy.Output.SSAVer = 1;
    auto Value = MedVar::makeConst(0x1101, 4);
    Value.Provenance = Provenance;
    Copy.addInput(Value);
    Block.Ops.push_back(Copy);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    bool Emitted = false;
    try {
      Emitted = bool(Emitter.emit({Function}, Context, "micro_pointer",
                                  GetParam(), {}, &Image, BinaryFormat::ELF));
    } catch (const std::invalid_argument &Error) {
      EXPECT_NE(std::string(Error.what()).find("code-pointer"),
                std::string::npos);
    }
    EXPECT_FALSE(Emitted);
  }
}

TEST_P(MicroMIPSPipeline, HighCRejectsWholeAndPartialUnboundImportValues) {
  auto Image = mixedImage(GetParam());
  Segment GOT;
  GOT.VA = 0x2000;
  GOT.Data.resize(16);
  GOT.Size = GOT.FileSz = GOT.Data.size();
  GOT.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(GOT));
  ASSERT_TRUE(Image.recordImportStorageSlot(0x2004, "external_function", 0,
                                            ImportStorageEvidence::LoaderBind));
  Import Imported;
  Imported.Name = "external_function";
  Imported.IATAddr = 0x2004;
  Image.Imports.push_back(std::move(Imported));
  const auto Check = [&](ExprPtr Value, bool Reject, bool Indexed = false) {
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "read_import_bits";
    Function.ReturnType = Value->Type;
    if (Indexed)
      Function.Params.push_back(
          {"index", NdType::makeInt(4, false), mipsreg::A0, 0});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Value);
    Function.Body.push_back(std::move(Return));
    CEmitterOptions Emit;
    Emit.TheArch = GetParam();
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &Image;
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    if (Reject)
      EXPECT_THROW(HighCEmitter().emit({Function}, Output, Emit),
                   std::invalid_argument);
    else
      EXPECT_TRUE(HighCEmitter().emit({Function}, Output, Emit));
  };
  const auto Address = [](unsigned Offset) {
    return HighExpr::makeConst(0x2000 + Offset, 4,
                               ConstantAddressProvenance::DataAddress);
  };
  for (auto [Offset, Size] :
       {std::pair{4, 4}, {4, 1}, {7, 1}, {3, 2}, {0, 4}, {8, 4}}) {
    SCOPED_TRACE(Offset);
    SCOPED_TRACE(Size);
    Check(HighExpr::makeLoad(Address(Offset), NdType::makeInt(Size, false)),
          Offset != 0 && Offset != 8);
  }
  // The merge's actual span, not just its effective address, owns admission.
  // Both full and partial reads can observe an unbound callable's tag.
  for (Intrinsic Id : {Intrinsic::MIPSLwl, Intrinsic::MIPSLwr}) {
    SCOPED_TRACE(int(Id));
    for (unsigned Offset : {4u, 5u, 6u, 7u}) {
      SCOPED_TRACE(Offset);
      auto Merge = HighExpr::makeCall(
          "", 0, {Address(Offset), HighExpr::makeConst(0, 4)});
      Merge->IntrinsicId = Id;
      Merge->Type = NdType::makeInt(4, false);
      Check(std::move(Merge), true);
    }
  }
  MedVar Index;
  Index.Kind = MedVar::Param;
  Index.TheArch = GetParam();
  Index.Size = 4;
  auto Variable = HighExpr::makeVar(Index, NdType::makeInt(4, false));
  auto Indexed = HighExpr::makeBinop(
      NdOp::INT_ADD, Address(0),
      HighExpr::makeBinop(NdOp::INT_MULT, Variable, HighExpr::makeConst(4, 4)));
  Check(HighExpr::makeLoad(Indexed, NdType::makeInt(4, false)), true, true);
  // An external parameter supplies runtime storage, not a file-byte backing.
  Check(HighExpr::makeLoad(Variable, NdType::makeInt(4, false)), false, true);
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MicroMIPSPipeline,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
