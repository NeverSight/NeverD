//===- RegistrationReliftTests.cpp - Reconstruct generated PE32 EH ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reprove a generated parent from image bytes before a second installation.
//===----------------------------------------------------------------------===//

#include "RegistrationSourceReceiptTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86RegistrationEntry.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

namespace {
using namespace neverd;

TEST(RegistrationRelift, InputPE32ReconstructsGeneratedFunction) {
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
  const auto *Path = std::getenv("NEVERD_REGISTRATION_RELIFT_PE32");
  const auto *Entry = std::getenv("NEVERD_REGISTRATION_RELIFT_ENTRY");
  const auto *Pop = std::getenv("NEVERD_REGISTRATION_ENTRY_POP");
  const auto *Registers = std::getenv("NEVERD_REGISTRATION_ENTRY_REGISTERS");
  if (!Path || !Entry || !Pop || !Registers)
    GTEST_SKIP() << "set the generated registration image, entry and ABI";
  const va_t Address = std::strtoull(Entry, nullptr, 0);
  const unsigned ExpectedPop = std::strtoul(Pop, nullptr, 0);
  const unsigned ExpectedRegisters = std::strtoul(Registers, nullptr, 0);
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto *EH = Image->ExceptionMetadata.findFunction(Address);
  ASSERT_TRUE(EH && EH->CodeRange.Begin == Address);
  ASSERT_TRUE(EH->Registration && EH->Cxx);
  ASSERT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(EH->Registration->RealignedFrame);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low =
      CFGBuilder().build(*Image, Decode, Address, "relifted_parent");
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &States = *Low.RegistrationStates;
  for (const auto &Diagnostic : States.Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  const auto Table = coff_loader::getCheckedX86SafeSEHTablePointer(*Image);
  ASSERT_TRUE(Table);
  EXPECT_FALSE(Low.OrdinaryModuleAnalysisRoots.count(Table->second));
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.RegistrationLifetimeComplete);
  ASSERT_TRUE(States.CallbackStatesComplete);
  ASSERT_TRUE(States.CxxContinuationsComplete);
  ASSERT_TRUE(States.CxxCatchObjectsComplete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  ASSERT_TRUE(States.CallFrameEffectsComplete);
  ASSERT_TRUE(States.CleanupFrameEffectsComplete);
  EXPECT_EQ(getCheckedX86RegistrationCxxParentABI(Low, *Image), ExpectedPop);

  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Med, Arch::X86);
  const auto ABI = projectX86RegistrationEntry(Med);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->RegisterCount, ExpectedRegisters);
  EXPECT_EQ(ABI->PopBytes, ExpectedPop);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "relifted-registration", Arch::X86,
                            {}, &*Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  if (const auto *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_IR")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Output, Error);
    ASSERT_FALSE(Error) << Error.message();
    Module->print(Out, nullptr);
  }
  const auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent && Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Proof = validateCOFFRegistrationCxxIR(*Parent, *EH, *Image);
  ASSERT_FALSE(bool(Proof)) << llvm::toString(std::move(Proof));
  if (const auto *Output = std::getenv("NEVERD_REGISTRATION_RELIFT_OUTPUT")) {
    COFFPatcher Patcher;
    Patcher.setImageContext(&*Image);
    const auto Result = Patcher.patch(Path, Output, *Module, Arch::X86);
    ASSERT_TRUE(Result.Success);
    EXPECT_EQ(Result.PatchedOriginalEntries, (std::vector<va_t>{Address}));
    auto Reloaded = COFFLoader().load(Output);
    ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
    const auto *Trampoline = Reloaded->readVA(Address, 5);
    ASSERT_TRUE(Trampoline);
    ASSERT_EQ(Trampoline[0], 0xe9);
    const va_t Target =
        uint32_t(Address + 5 + llvm::support::endian::read32le(Trampoline + 1));
    const auto *Generated = Reloaded->ExceptionMetadata.findFunction(Target);
    ASSERT_TRUE(Generated && Generated->CodeRange.Begin == Target);
    ASSERT_EQ(Generated->ParseStatus, ExceptionParseStatus::Complete);
    registration_test::writeSourceReceipt(
        Path, Output, *Image, *EH, States, *Generated,
        llvm::json::Object{{"entry_pop", ExpectedPop},
                           {"entry_registers", ExpectedRegisters}});
  }
#else
  GTEST_SKIP() << "LLVM lacks native PE32 C++ contracts";
#endif
}
} // namespace
