//===- RegistrationControlFPStackTests.cpp - CRT scalar branch proof ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep invocation-specific CRT results separate from frame memory facts.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

BinaryImage precisionImage(uint32_t Base, const char *Module, bool Veneer,
                           uint32_t Control, unsigned Mutation,
                           bool TerminatingError = false) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Base;
  Image.Entry = Base + 0x1000;
  const va_t Slot = Base + 0x3000;
  const va_t Stub = Image.Entry + 0x60;
  const va_t Error = Image.Entry + 0x70;
  std::vector<uint8_t> Code;
  auto word = [&](uint32_t Value) {
    const size_t At = Code.size();
    Code.resize(At + 4);
    writeLE<uint32_t>(Code.data() + At, Value);
  };
  auto direct = [&](va_t Target) {
    Code.push_back(0xe8);
    word(uint32_t(Target - (Image.Entry + Code.size() + 4)));
  };
  Code.push_back(0x56); // Preserve ESI without using a saved memory fact.
  Code.push_back(0x68);
  word(Mutation == 2 ? 0x10000 : Mutation == 3 ? 0x70000 : 0x30000);
  Code.push_back(0x68);
  word(Mutation == 1 ? 0x30000 : Control);
  if (Mutation == 4)
    Code.insert(Code.end(), {0x89, 0xe6}); // Output aliases the outgoing frame.
  else
    Code.insert(Code.end(), {0x33, 0xf6}); // Exact scalar null in ESI.
  Code.push_back(0x56);
  if (Mutation == 7) {
    Code.push_back(0xa1); // An opaque read may dispatch a continuing VEH.
    word(Slot + 4);
  }
  if (Mutation == 8)
    Code.insert(Code.end(), {0xc6, 0x44, 0x24, 8, 0});
  if (Mutation == 9)
    Code.insert(Code.end(),
                {0x89, 0x07}); // Unknown alias may change arguments.
  if (Mutation == 10)
    Code.insert(Code.end(),
                {0x83, 0xc7, 1}); // Unrelated pure scalar operation.
  if (Mutation == 11)
    direct(Error);
  if (Mutation == 12)
    Code.insert(Code.end(), {0xeb, 0}); // A separate packet join is required.
  if (Mutation == 13) {
    Code.insert(Code.end(), {0xc7, 0x05});
    word(Slot);
    word(0);
  }
  if (Veneer)
    direct(Stub);
  else {
    Code.insert(Code.end(), {0xff, 0x15});
    word(Slot);
  }
  Code.insert(Code.end(), {0x83, 0xc4, 12, 0x85, 0xc0, 0x74, 10});
  // The error callee returns without popping the five words. Both source
  // paths remain in the CFG; only a proved zero result can skip this bad SP.
  Code.insert(Code.end(), 5, 0x56);
  direct(Error);
  Code.insert(Code.end(), {0x5e, 0xc3});
  EXPECT_LE(Code.size(), size_t(Stub - Image.Entry));
  Code.resize(Stub - Image.Entry, 0xcc);
  Code.insert(Code.end(), {0xff, 0x25});
  word(Slot);
  Code.resize(Error - Image.Entry, 0xcc);
  if (TerminatingError) {
    Code.insert(Code.end(), {0xff, 0x25});
    word(Slot + 4);
  } else
    Code.push_back(0xc3);
  // A separate ordinary caller must retain the precision call's IAT binding.
  Code.resize(0x80, 0xcc);
  if (Mutation == 16) {
    Code.insert(Code.end(), {0xc7, 0x05});
    word(Slot);
    word(0);
  }
  direct(Image.Entry);
  Code.push_back(0xc3);
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Mutation == 14)
    Text.Flags = Text.Flags | SegmentFlags::Writable;
  Text.Data = std::move(Code);
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  Segment Data;
  Data.Name = ".idata";
  Data.VA = Slot;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(8);
  Data.Size = Data.FileSz = Data.Data.size();
  Image.Segments.push_back(std::move(Data));
  for (const auto &Segment : Image.Segments) {
    Section Section;
    Section.Name = Segment.Name;
    Section.VA = Segment.VA;
    Section.Size = Section.FileSz = Segment.Size;
    Section.Flags = Segment.Flags;
    Image.Sections.push_back(std::move(Section));
  }
  Import Imported;
  Imported.Name = "_controlfp_s";
  Imported.Module = Mutation == 5 ? "application.dll" : Module;
  Imported.IsDelayImport = Mutation == 6;
  Imported.IATAddr = Slot;
  Image.Imports.push_back(std::move(Imported));
  EXPECT_TRUE(Image.recordImportStorageSlot(
      Slot, "_controlfp_s", 0, ImportStorageEvidence::ImportDirectory));
  if (TerminatingError) {
    Import ErrorImport;
    ErrorImport.Name = "_invoke_watson";
    ErrorImport.Module = Module;
    ErrorImport.IATAddr = Slot + 4;
    Image.Imports.push_back(std::move(ErrorImport));
    EXPECT_TRUE(Image.recordImportStorageSlot(
        Slot + 4, "_invoke_watson", 0, ImportStorageEvidence::ImportDirectory));
  }
  return Image;
}

TEST(RegistrationControlFPStack, OnlyAnExactPrivateScalarPacketPrunesFailure) {
  for (uint32_t Base : {0x400000u, 0x18000000u, 0x50000000u})
    for (const char *Module : {"msvcrt.dll", "MSVCR100.DLL"})
      for (bool Veneer : {false, true})
        for (bool TerminatingError : {false, true})
          for (uint32_t Control : {0u, 0x10000u, 0x20000u})
            for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
              SCOPED_TRACE(::testing::Message()
                           << Base << ':' << Module << ':' << Veneer << ':'
                           << Control << ':' << Mutation << ':'
                           << TerminatingError);
              const auto Image = precisionImage(Base, Module, Veneer, Control,
                                                Mutation, TerminatingError);
              size_t Work =
                  Mutation == 15 ? limits::kMaxRegistrationEHStateWork : 0;
              const auto Pop =
                  getCheckedX86CalleeStackPop(Image, Image.Entry, &Work);
              EXPECT_EQ(Pop, Mutation == 0 || Mutation == 10
                                 ? std::optional<uint32_t>(0)
                                 : std::nullopt);
              EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
              if (!Mutation)
                EXPECT_FALSE(
                    getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry));
            }
}

TEST(RegistrationControlFPStack, ResultProofRetainsItsTransitiveImportBinding) {
  for (bool Veneer : {false, true})
    for (bool TerminatingError : {false, true})
      for (bool Rebind : {false, true}) {
        auto Image = precisionImage(0x400000, "MSVCR100.DLL", Veneer, 0x10000,
                                    Rebind ? 16 : 0, TerminatingError);
        const va_t Caller = Image.Entry + 0x80;
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        auto Function = CFGBuilder().buildIntegerFrameProof(
            Image, Dec, Caller, "precision-stack-caller");
        RegistrationCallCalleeIndex Index(Image);
        const auto Contracts = Index.stackContracts(Function);
        ASSERT_TRUE(Contracts);
        bool Found = false;
        for (const auto &Contract : *Contracts)
          if (Contract.Target == Image.Entry && !Contract.Indirect) {
            EXPECT_EQ(Contract.StackPopBytes, 0u);
            EXPECT_EQ(Contract.RequiredImports,
                      std::vector<va_t>{Image.Base + 0x3000});
            Found = true;
          }
        EXPECT_TRUE(Found);
        EXPECT_EQ(Index.stackPop(Caller),
                  Rebind ? std::nullopt : std::optional<uint32_t>{0});
      }
}

} // namespace
