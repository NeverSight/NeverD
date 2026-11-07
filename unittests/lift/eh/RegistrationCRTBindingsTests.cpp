//===- RegistrationCRTBindingsTests.cpp - CRT binding effects ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Separate normal setter binding effects from callback and frame contracts.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

BinaryImage setterImage(uint32_t Base, const char *Module, const char *Name,
                        unsigned Form, unsigned Mutation) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Base;
  Image.Entry = Base + 0x1000;
  const va_t Slot = Base + 0x3000;
  const va_t Stub = Image.Entry + 0x80;
  std::vector<uint8_t> Code;
  auto word = [&](uint32_t Value) {
    const auto At = Code.size();
    Code.resize(At + 4);
    writeLE<uint32_t>(Code.data() + At, Value);
  };
  auto direct = [&](va_t Target) {
    Code.push_back(0xe8);
    word(uint32_t(Target - (Image.Entry + Code.size() + 4)));
  };
  auto store = [&](va_t Target) {
    Code.insert(Code.end(), {0xc7, 0x05});
    word(Target);
    word(0);
  };
  auto invoke = [&](va_t Target, bool Setter) {
    if (Setter && Mutation == 14) {
      Code.push_back(0x68);
      word(0x80000000u);
    } else if (Setter && Mutation == 15)
      Code.push_back(0x50);
    else
      Code.insert(Code.end(),
                  {0x6a, uint8_t(Setter ? Mutation == 13 ? 0xff : 2 : 0)});
    if (Setter && Form == 2)
      direct(Stub);
    else if (Form == 1) {
      Code.insert(Code.end(), {0x8b, 0x35});
      word(Target);
      Code.insert(Code.end(), {0xff, 0xd6});
    } else {
      Code.insert(Code.end(), {0xff, 0x15});
      word(Target);
    }
    if (Setter)
      Code.insert(Code.end(), {0x83, 0xc4, 4});
  };
  if (Mutation == 1)
    store(Slot);
  invoke(Slot, true);
  if (Mutation == 2)
    store(Slot + 4);
  if (Mutation == 3)
    direct(Stub + 6);
  invoke(Slot + 4, false);
  if (Mutation == 12)
    store(Slot + 4);
  Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
  EXPECT_LT(Code.size(), size_t(Stub - Image.Entry));
  Code.resize(Stub - Image.Entry, 0xcc);
  Code.insert(Code.end(), {0xff, 0x25});
  word(Slot);
  // The intervening helper must have an unproved effect, not just a RET.
  Code.insert(Code.end(), {0x8b, 0x00, 0xc3});
  Code.resize(0x90, 0xcc);
  direct(Image.Entry);
  invoke(Slot + 4, false);
  Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Mutation == 10)
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
  if (Mutation == 11)
    Data.FileSz = 7;
  Image.Segments.push_back(std::move(Data));
  for (const auto &Segment : Image.Segments) {
    Section Section;
    Section.Name = Segment.Name;
    Section.VA = Segment.VA;
    Section.Size = Segment.Size;
    Section.FileSz = Segment.FileSz;
    Section.Flags = Segment.Flags;
    Image.Sections.push_back(std::move(Section));
  }
  for (unsigned I = 0; I != 2; ++I) {
    Import Imported;
    Imported.Name = I ? "Sleep" : Name;
    Imported.Module = I ? "KERNEL32.dll" : Module;
    Imported.IATAddr = Slot + 4 * I;
    if (!I && Mutation == 4)
      Imported.Module = "application.dll";
    if (!I && Mutation == 5)
      Imported.IsDelayImport = true;
    if (!I && Mutation == 8)
      Imported.Name += "@4";
    Image.Imports.push_back(Imported);
    EXPECT_TRUE(
        Image.recordImportStorageSlot(Imported.IATAddr, Imported.Name, 0,
                                      ImportStorageEvidence::ImportDirectory));
  }
  if (Mutation == 6)
    Image.ImportStorageSlots.erase(Slot);
  if (Mutation == 7)
    Image.ImportStorageSlots.at(Slot).Name = "_wrong_storage";
  return Image;
}

TEST(RegistrationCRTBindings, LegacySettersRetainOnlyReachingBindings) {
  for (uint32_t Base : {0x400000u, 0x18000000u, 0x50000000u})
    for (const char *Module : {"msvcrt.dll", "MSVCR100.DLL"})
      for (const char *Name : {"__set_app_type", "__setusermatherr",
                               "_configthreadlocale", "_onexit"})
        for (unsigned Form = 0; Form != 3; ++Form)
          for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
            SCOPED_TRACE(::testing::Message()
                         << Base << ':' << Module << ':' << Name << ':' << Form
                         << ':' << Mutation);
            const auto Image = setterImage(Base, Module, Name, Form, Mutation);
            const bool Setter =
                llvm::StringRef(Module).equals_insensitive("msvcr100.dll") &&
                (llvm::StringRef(Name) == "__set_app_type" ||
                 llvm::StringRef(Name) == "__setusermatherr");
            const bool Valid = Setter && (Mutation == 0 || Mutation >= 12);
            size_t Work =
                Mutation == 9 ? limits::kMaxRegistrationEHStateWork : 0;
            EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry, &Work),
                      Valid ? std::optional<uint32_t>{0} : std::nullopt);
            EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
            if (!Mutation) {
              Decoder Dec;
              ASSERT_TRUE(Dec.init(Image));
              auto Function = CFGBuilder().buildIntegerFrameProof(
                  Image, Dec, Image.Entry, "crt-setter-bindings");
              RegistrationCallCalleeIndex Index(Image);
              const auto Contracts = Index.stackContracts(Function);
              ASSERT_TRUE(Contracts);
              ASSERT_EQ(Contracts->size(), 2u);
              for (const auto &Contract : *Contracts)
                EXPECT_EQ(Contract.PreservesImportBindings,
                          Setter &&
                              Contract.Target == (Form == 2 ? Image.Entry + 0x80
                                                            : Base + 0x3000));
              // The normal setter effect cannot become a complete private
              // frame, callback, scalar result or initializer contract.
              EXPECT_FALSE(
                  getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry));
              EXPECT_FALSE(
                  getCheckedX86RegistrationObjectCalleeABI(Image, Image.Entry));
              EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
                  Image, Image.Entry));
              // Ordinary root summaries still expose no binding preservation;
              // its later Sleep is opaque even when the setter is checked.
              EXPECT_FALSE(Index.stackPop(Image.Entry + 0x90));
              if (Setter) {
                auto Caller = CFGBuilder().buildIntegerFrameProof(
                    Image, Dec, Image.Entry + 0x90, "crt-setter-outer");
                const auto Outer = Index.stackContracts(Caller);
                ASSERT_TRUE(Outer);
                bool Found = false;
                for (const auto &Contract : *Outer)
                  if (Contract.Target == Image.Entry) {
                    Found = true;
                    EXPECT_EQ(
                        Contract.RequiredImports,
                        (std::vector<va_t>{Base + 0x3000, Base + 0x3004}));
                    EXPECT_FALSE(Contract.PreservesImportBindings);
                  }
                EXPECT_TRUE(Found);
              }
            }
          }
}
} // namespace
