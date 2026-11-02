//===- RegistrationJumpRuntimeTests.cpp - Nonlocal runtime identity -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Refuse lookalike, changed and unauthenticated jump-runtime bindings.

#include "../../../lib/ir/low/X86/RegistrationJumpRuntime.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdint>
#include <string>
#include <utility>

using namespace neverd;
using namespace neverd::registration_state;

namespace {
constexpr va_t kVeneer = 0x401000;
constexpr va_t kCaller = kVeneer + 0x10;
constexpr va_t kImportSlot = 0x402000;

BinaryImage jumpRuntimeImage(const char *Name) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Segment Code;
  Code.VA = kVeneer;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(0x20, 0xcc);
  Code.Data[0] = 0xff;
  Code.Data[1] = 0x25;
  writeLE<uint32_t>(Code.Data.data() + 2, kImportSlot);
  Code.Data[0x10] = 0xe8;
  writeLE<uint32_t>(Code.Data.data() + 0x11, uint32_t(kVeneer - (kCaller + 5)));
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  Segment IAT;
  IAT.VA = kImportSlot;
  IAT.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  IAT.Data.assign(4, 0);
  IAT.Size = IAT.FileSz = IAT.Data.size();
  Image.Segments.push_back(std::move(IAT));
  for (const auto &Segment : Image.Segments) {
    Section Section;
    Section.VA = Segment.VA;
    Section.Flags = Segment.Flags;
    Section.Size = Section.FileSz = Segment.Size;
    Image.Sections.push_back(std::move(Section));
  }
  Image.BaseRelocations.push_back({kVeneer + 2, 3});
  Image.Imports.push_back({"msvcrt.dll", Name, 0, kImportSlot});
  EXPECT_TRUE(Image.recordImportStorageSlot(
      kImportSlot, Name, 0, ImportStorageEvidence::ImportDirectory));
  return Image;
}
} // namespace

TEST(RegistrationJumpRuntime, SyntheticBindingsRetainCompleteStorageProofs) {
  struct Routine {
    const char *Name;
    JumpRuntimeBinding::Kind Kind;
  };
  const Routine Routines[] = {
      {"_setjmp3", JumpRuntimeBinding::Kind::Capture},
      {"_longjmpex", JumpRuntimeBinding::Kind::Resume},
      {"_seh_longjmp_unwind", JumpRuntimeBinding::Kind::EH3Unwind},
      {"_seh_longjmp_unwind4", JumpRuntimeBinding::Kind::EH4Unwind}};
  for (const auto &[Name, Kind] : Routines)
    for (bool Indirect : {false, true})
      for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
        SCOPED_TRACE(Name);
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(Mutation);
        auto Image = jumpRuntimeImage(Name);
        switch (Mutation) {
        case 1:
          Image.Segments[0].Data[0] = 0x90;
          break;
        case 2:
          Image.Segments[0].Data[1] = 0x15;
          break;
        case 3:
        case 4:
          Image.Segments[0].Data.resize(Mutation - 2);
          break;
        case 5:
          Image.Segments[0].Flags =
              Image.Segments[0].Flags | SegmentFlags::Writable;
          Image.Sections[0].Flags = Image.Segments[0].Flags;
          break;
        case 6:
          Image.Sections.push_back(Image.Sections[0]);
          break;
        case 7:
          Image.BaseRelocations.front().Address = kVeneer + 1;
          break;
        case 8:
          Image.BaseRelocations.push_back(Image.BaseRelocations.front());
          break;
        case 9:
          Image.Imports[0].Module = "application.dll";
          break;
        case 10:
          Image.Imports[0].Name = "opaque_call";
          break;
        case 11:
          Image.Imports[0].IsDelayImport = true;
          break;
        case 12:
          Image.ImportStorageSlots.clear();
          break;
        case 13:
          Image.ConflictingImportStorageSlots.insert(kImportSlot);
          break;
        case 14:
          Image.Segments[1].Data.resize(3);
          break;
        case 15:
          Image.Sections.push_back(Image.Sections[1]);
          break;
        case 16:
          Image.Imports.push_back(Image.Imports[0]);
          Image.Imports.back().Name = "opaque_call";
          break;
        case 17:
          Image.BaseRelocations.push_back({kVeneer + 8, 3});
          break;
        case 18:
          Image.Arch = Arch::ARM;
          break;
        case 19:
          Image.Bits = Bitness::Bits64;
          break;
        case 20:
          Image.Format = BinaryFormat::ELF;
          break;
        case 21:
          Image.IsRelocatable = true;
          break;
        default:
          break;
        }
        // An indirect call binds the IAT itself, independently of the veneer.
        const bool Valid = Mutation == 0 || Mutation == 17 ||
                           (Indirect && Mutation >= 1 && Mutation <= 8);
        const auto Binding = checkedJumpRuntime(
            Image, Indirect ? kImportSlot : kVeneer, Indirect);
        EXPECT_EQ(Binding.has_value(), Valid);
        if (Binding) {
          EXPECT_EQ(Binding->Routine, Kind);
          EXPECT_EQ(Binding->Target, Indirect ? kImportSlot : kVeneer);
          EXPECT_EQ(Binding->ImportSlot, kImportSlot);
          EXPECT_EQ(Binding->Indirect, Indirect);
        }
      }
}

TEST(RegistrationJumpRuntime, PhysicalCallAndRuntimeBindingAreRechecked) {
  for (const char *Name : {"_setjmp3", "_longjmpex"}) {
    auto Image = jumpRuntimeImage(Name);
    LowOp Call;
    Call.Addr = kCaller;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(kVeneer, 4));
    const auto Matches = [&] {
      return isSEHJumpRuntimeInstruction(Image, kCaller, 5, {Call});
    };
    ASSERT_TRUE(Matches());
    Image.Segments[0].Data[0] = 0x90;
    EXPECT_FALSE(Matches());
    Image.Segments[0].Data[0] = 0xff;
    ASSERT_TRUE(Matches());
    Image.Imports[0].Name = "opaque_call";
    EXPECT_FALSE(Matches());
    Image.Imports[0].Name = Name;
    ASSERT_TRUE(Matches());
    Image.BaseRelocations.push_back({kCaller + 1, 3});
    EXPECT_FALSE(Matches());
    Image.BaseRelocations.pop_back();
    ASSERT_TRUE(Matches());
    ++Image.Segments[0].Data[0x11];
    EXPECT_FALSE(Matches());
    --Image.Segments[0].Data[0x11];
    ASSERT_TRUE(Matches());
    Image.Segments[0].Data[0x10] = 0xe9;
    EXPECT_FALSE(Matches());
  }
}

#ifdef NEVERD_BINARY_CORPUS_ROOT
TEST(RegistrationJumpRuntime, RequiresCurrentImportAndImmutableVeneer) {
  struct Routine {
    va_t Target;
    va_t Slot;
    JumpRuntimeBinding::Kind Kind;
  };
  const Routine Routines[] = {
      {0x407c7e, 0x408058, JumpRuntimeBinding::Kind::Capture},
      {0x407c66, 0x408044, JumpRuntimeBinding::Kind::Resume},
      {0x407c78, 0x40805c, JumpRuntimeBinding::Kind::EH3Unwind}};
  for (const auto &Routine : Routines)
    for (bool Indirect : {false, true})
      for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
        SCOPED_TRACE(Routine.Target);
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(Mutation);
        auto Image = COFFLoader().load(std::string(NEVERD_BINARY_CORPUS_ROOT) +
                                       "/corpus/windows-eh/msvc/vs2019/x86/"
                                       "native/no-gs/o0/windows-seh-tests/"
                                       "xcpt4-msvc-x86-native-no-gs-o0.exe");
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        auto Imported = llvm::find_if(Image->Imports, [&](const Import &I) {
          return I.IATAddr == Routine.Slot;
        });
        ASSERT_NE(Imported, Image->Imports.end());
        if (Mutation == 1)
          Imported->Module = "application.dll";
        if (Mutation == 2)
          Imported->Name = "same_abi_opaque";
        if (Mutation == 3)
          Imported->IsDelayImport = true;
        if (Mutation == 4) {
          Image->Imports.push_back(*Imported);
          Image->Imports.back().Module = "application.dll";
        }
        if (Mutation == 5)
          Image->ImportStorageSlots.clear();
        if (Mutation == 6)
          Image->ConflictingImportStorageSlots.insert(Routine.Slot);
        if (Mutation == 7)
          Image->Bits = Bitness::Bits64;
        if (Mutation == 8)
          Image->Arch = Arch::ARM;
        if (Mutation == 9)
          Image->Format = BinaryFormat::ELF;
        if (Mutation == 10 || Mutation == 11) {
          auto *Bytes = const_cast<uint8_t *>(Image->readVA(Routine.Target, 6));
          ASSERT_TRUE(Bytes);
          if (Mutation == 10)
            Bytes[1] = 0x15; // CALL is not the JMP veneer.
          else
            ++Bytes[2]; // A neighbouring IAT byte is not this binding.
        }
        if (Mutation == 12) {
          for (auto &Segment : Image->Segments)
            if (Segment.isExecutable())
              Segment.Flags = Segment.Flags | SegmentFlags::Writable;
          for (auto &Section : Image->Sections)
            if (Section.isExecutable())
              Section.Flags = Section.Flags | SegmentFlags::Writable;
        }
        if (Mutation == 13)
          Imported->Module = "VCRUNTIME140D.dll";
        const auto Binding = checkedJumpRuntime(
            *Image, Indirect ? Routine.Slot : Routine.Target, Indirect);
        const bool Valid = Mutation == 0 || Mutation == 13 ||
                           (Indirect && Mutation >= 10 && Mutation <= 12);
        EXPECT_EQ(Binding.has_value(), Valid);
        if (Binding) {
          EXPECT_EQ(Binding->Routine, Routine.Kind);
          EXPECT_EQ(Binding->ImportSlot, Routine.Slot);
          EXPECT_EQ(Binding->Indirect, Indirect);
        }
      }
}
#endif
