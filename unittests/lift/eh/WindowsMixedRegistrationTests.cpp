//===- WindowsMixedRegistrationTests.cpp - Mixed PE32 EH modes ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve synchronous C++ and asynchronous SEH parents in one emitted image.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFSourceCallSymbols.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/object/PELayout.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

TEST(WindowsMixedRegistration, InputPE32BothOrdersSurviveDefaultOptimization) {
#ifndef LLVM_NEVERD_X86_CXX_EH_MODE
  GTEST_SKIP() << "requires per-function synchronous x86 C++ EH mode";
#else
  const auto *Path = std::getenv("NEVERD_MIXED_REGISTRATION_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the authored mixed registration PE32 fixture";
  const auto *Output = std::getenv("NEVERD_MIXED_REGISTRATION_OUTPUT");
  auto Loaded = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto &Image = *Loaded;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  std::vector<MedFunc> Functions;
  for (const char *Name : {"cleanup_only_parent", "mixed_seh"}) {
    SCOPED_TRACE(Name);
    const auto *Export = Image.findExport(Name);
    ASSERT_TRUE(Export);
    const auto *Source = Image.ExceptionMetadata.findFunction(Export->Addr);
    ASSERT_TRUE(Source && Source->Registration);
    ASSERT_EQ(Source->ParseStatus, ExceptionParseStatus::Complete);
    auto Low = CFGBuilder().build(Image, Decode, Export->Addr, Name);
    ASSERT_TRUE(Low.RegistrationStates);
    for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
      llvm::errs() << Diagnostic << '\n';
    ASSERT_TRUE(Low.RegistrationStates->Complete);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
    inferMedTypes(Med, Arch::X86);
    recoverCallAbi(Med, Arch::X86, {}, &Image);
    Functions.push_back(std::move(Med));
  }
  auto Input = llvm::MemoryBuffer::getFile(Path);
  ASSERT_TRUE(bool(Input));
  const auto Bytes = (*Input)->getBuffer();
  const std::vector<uint8_t> Original(Bytes.bytes_begin(), Bytes.bytes_end());
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    if (Reverse)
      std::reverse(Functions.begin(), Functions.end());
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    auto Emitted = Emitter.emit(Functions, Context, "mixed-registration",
                                Arch::X86, {}, &Image, BinaryFormat::COFF);
    ASSERT_TRUE(Emitted);
    ASSERT_TRUE(Emitted->getModuleFlag("eh-asynch"));
    for (bool Optimize : {false, true}) {
      SCOPED_TRACE(Optimize);
      auto Module = llvm::CloneModule(*Emitted);
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      if (Optimize) {
        const auto Result =
            Pipeline::optimizeModule(*Module, Pipeline::OptimizationOptions{});
        ASSERT_EQ(Result.Stop, OptimizationStopReason::Stable)
            << optimizationStopMessage(Result);
      }
      for (const auto &Source : Functions) {
        const auto *Parent = Module->getFunction(Source.Name);
        ASSERT_TRUE(Parent);
        ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
        ASSERT_TRUE(Source.ExceptionMetadata);
        EXPECT_EQ(
            Parent->hasFnAttribute(llvm::RewriteWinX86SynchronousCxxAttribute),
            bool(Source.ExceptionMetadata->Cxx));
        auto Error = validateCOFFRegistrationIR(
            *Parent, *Source.ExceptionMetadata, Image);
        ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      }
      const std::string Suffix =
          std::string(Reverse ? "-seh-first" : "-cxx-first") +
          (Optimize ? "-opt" : "-raw");
      if (Output) {
        std::error_code Error;
        llvm::raw_fd_ostream Stream(std::string(Output) + Suffix + ".ll",
                                    Error);
        ASSERT_FALSE(bool(Error)) << Error.message();
        Module->print(Stream, nullptr);
        Stream.close();
        ASSERT_FALSE(Stream.has_error());
      }
      auto SourceCalls = getCOFFSourceCallSymbols(*Module, Image);
      ASSERT_TRUE(bool(SourceCalls)) << llvm::toString(SourceCalls.takeError());
      auto Resolve = [&](llvm::StringRef Symbol,
                         uint32_t) -> std::optional<uint64_t> {
        if (auto Runtime = findCOFFRegistrationRuntimeVA(Image, Symbol))
          return *Runtime;
        if (auto Address = parseNdDataSymbol(Symbol))
          return *Address;
        if (auto Address = parseNdCodePtrSymbol(Symbol))
          return *Address;
        if (const auto Import = SourceCalls->Imports.find(Symbol.str());
            Import != SourceCalls->Imports.end())
          return Import->second;
        if (const auto Code = SourceCalls->Code.find(Symbol.str());
            Code != SourceCalls->Code.end())
          return Code->second;
        return std::nullopt;
      };
      auto Binary = Original;
      COFFPatcher Patcher;
      const va_t CodeVA = Patcher.plannedExecSegmentVA(Binary, Arch::X86);
      auto Native = compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                                         CodeVA, Resolve, Image.Base);
      ASSERT_TRUE(Native.Success);
      ASSERT_TRUE(Native.Unresolved.empty())
          << ::testing::PrintToString(Native.Unresolved);
      std::vector<std::pair<va_t, va_t>> Mappings;
      for (const auto &Source : Functions) {
        const auto *Parent = Module->getFunction(Source.Name);
        ASSERT_TRUE(Parent);
        llvm::SmallString<64> Name;
        llvm::Mangler Mangler;
        Mangler.getNameWithPrefix(Name, Parent, false);
        const auto Symbol = Native.SymbolAddrs.find(Name.str().str());
        ASSERT_NE(Symbol, Native.SymbolAddrs.end());
        Mappings.emplace_back(Source.ExceptionMetadata->CodeRange.Begin,
                              Symbol->second);
      }
      auto Update = prepareCOFFRegistrationPatch(Binary, Image, Native,
                                                 Mappings, CodeVA, *Module);
      ASSERT_TRUE(bool(Update)) << llvm::toString(Update.takeError());
      ASSERT_TRUE(Update->Apply);
      ASSERT_EQ(Update->GeneratedCxxGraphs.size(), 1u);
      ASSERT_EQ(Patcher.appendExecSegment(Binary, Native.Bytes, kNdTextSection,
                                          Arch::X86),
                CodeVA);
      const auto PE = locatePEHeaders(Binary.data(), Binary.size());
      for (const auto &[Source, Generated] : Mappings) {
        std::optional<size_t> Entry;
        forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
          const uint64_t RVA = Source - Image.Base;
          if (RVA >= Section.VirtualAddress &&
              rangeInBounds(RVA - Section.VirtualAddress, 5,
                            Section.SizeOfRawData))
            Entry = Section.PointerToRawData + RVA - Section.VirtualAddress;
        });
        ASSERT_TRUE(Entry);
        ASSERT_TRUE(rangeInBounds(*Entry, 5, Binary.size()));
        Binary[*Entry] = 0xe9;
        writeLE<uint32_t>(Binary.data() + *Entry + 1,
                          uint32_t(Generated - Source - 5));
      }
      auto Applied = applyCOFFRegistrationPatch(Binary, *Update);
      ASSERT_FALSE(bool(Applied)) << llvm::toString(std::move(Applied));
      auto Checked = validateCOFFRegistrationPatch(Binary, *Update);
      ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
      if (Output) {
        std::error_code Error;
        llvm::raw_fd_ostream Stream(std::string(Output) + Suffix + ".exe",
                                    Error, llvm::sys::fs::OF_None);
        ASSERT_FALSE(bool(Error)) << Error.message();
        Stream.write(reinterpret_cast<const char *>(Binary.data()),
                     Binary.size());
        Stream.close();
        ASSERT_FALSE(Stream.has_error());
      }
    }
  }
#endif
}
} // namespace
