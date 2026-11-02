//===- UnwindFrameTestsDetail.h - Public query fixture helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared immutable Mach-O inputs and public session setup for separate C API
/// and CLI executables. No compiler or private session interface is required.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_UNWINDFRAMETESTSDETAIL_H
#define NEVERD_UNITTESTS_UNWINDFRAMETESTSDETAIL_H

#include "gtest/gtest.h"
#include "lift/language/AArch64DwarfTestsDetail.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neverd::unwind_frame_test {
namespace dwarf = aarch64_dwarf_test;

#define NEVERD_RA_CAPI_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define NEVERD_RA_CAPI_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_RA_CAPI_WORDS(Name, ...)                                        \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#include "UnwindFrameCAPIFixtures.def"

inline std::string take(const char *Text) {
  const std::string Result = Text ? Text : kJSONEmpty;
  neverd_free_string(Text);
  return Result;
}

struct ImageOptions {
  bool AArch64 = true;
  bool Dwarf = true;
  bool Compact = true;
  bool DwarfFallback = false;
  uint32_t CompactEncoding = kSignedCompact;
  uint64_t DwarfRange = dwarf::kRange;
};

inline std::vector<uint8_t> makeMachO(llvm::ArrayRef<uint8_t> Program,
                                      const ImageOptions &Options = {}) {
  using namespace llvm::MachO;
  auto Frames = dwarf::frame(
      Program, {}, Options.AArch64 ? dwarf::kBKeyNonHint : dwarf::kOrdinary,
      dwarf::kCodeAlign, kImageBase + kCodeOffset, false,
      kImageBase + kFrameOffset);
  const uint32_t FDEOffset = readLE<uint32_t>(Frames.data()) + sizeof(uint32_t);
  writeLE<uint64_t>(Frames.data() + FDEOffset + kRangeFieldOffset,
                    Options.DwarfRange);
  std::vector<uint8_t> Compact(sizeof(kCompactWords));
  for (size_t Index = 0; Index < std::size(kCompactWords); ++Index)
    writeLE<uint32_t>(Compact.data() + Index * sizeof(uint32_t),
                      kCompactWords[Index]);
  const uint32_t Encoding =
      Options.DwarfFallback
          ? (Options.AArch64 ? kAArch64DwarfMode : kX64DwarfMode) | FDEOffset
          : Options.CompactEncoding;
  writeLE<uint32_t>(Compact.data() + kEncodingOffset, Encoding);

  std::vector<uint8_t> Bytes(kFileBytes);
  auto *Header = reinterpret_cast<mach_header_64 *>(Bytes.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = Options.AArch64 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
  Header->cpusubtype = Options.AArch64
                           ? static_cast<uint32_t>(CPU_SUBTYPE_ARM64E)
                           : static_cast<uint32_t>(CPU_SUBTYPE_X86_64_ALL);
  Header->filetype = MH_DYLIB;
  Header->ncmds = kLoadCommandCount;
  const uint32_t SegmentBytes =
      sizeof(segment_command_64) +
      (1 + Options.Dwarf + Options.Compact) * sizeof(section_64);
  const uint32_t IdentityBytes = llvm::alignTo(
      sizeof(dylib_command) + sizeof(kImageFile), sizeof(uint64_t));
  Header->sizeofcmds = SegmentBytes + IdentityBytes;
  auto *Segment = reinterpret_cast<segment_command_64 *>(Header + 1);
  Segment->cmd = LC_SEGMENT_64;
  Segment->cmdsize = SegmentBytes;
  auto *Identity = reinterpret_cast<dylib_command *>(
      Bytes.data() + sizeof(mach_header_64) + SegmentBytes);
  Identity->cmd = LC_ID_DYLIB;
  Identity->cmdsize = IdentityBytes;
  Identity->dylib.name = sizeof(dylib_command);
  std::memcpy(Identity + 1, kImageFile, sizeof(kImageFile));
  std::memcpy(Segment->segname, dwarf::kMachOTextSegment,
              sizeof(dwarf::kMachOTextSegment));
  Segment->vmaddr = kImageBase;
  Segment->vmsize = Segment->filesize = Bytes.size();
  Segment->maxprot = Segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Segment->nsects = 1 + Options.Dwarf + Options.Compact;
  auto *Section = reinterpret_cast<section_64 *>(Segment + 1);
  const auto AddSection = [&](const char *Name, uint32_t Offset, uint64_t Size,
                              uint32_t Flags) {
    std::memcpy(Section->sectname, Name, std::strlen(Name));
    std::memcpy(Section->segname, dwarf::kMachOTextSegment,
                sizeof(dwarf::kMachOTextSegment));
    Section->addr = kImageBase + Offset;
    Section->size = Size;
    Section->offset = Offset;
    Section->align = dwarf::kMachOSectionAlignment;
    Section->flags = Flags;
    ++Section;
  };
  AddSection(dwarf::kMachOTextSection, kCodeOffset, dwarf::kRange,
             S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
  std::copy(std::begin(dwarf::kNopCode), std::end(dwarf::kNopCode),
            Bytes.begin() + kCodeOffset);
  if (Options.Dwarf) {
    AddSection(dwarf::kMachOFrameSection, kFrameOffset, Frames.size(), 0);
    std::copy(Frames.begin(), Frames.end(), Bytes.begin() + kFrameOffset);
  }
  if (Options.Compact) {
    AddSection(kUnwindSection, kCompactOffset, Compact.size(), 0);
    std::copy(Compact.begin(), Compact.end(), Bytes.begin() + kCompactOffset);
  }
  return Bytes;
}

class UnwindFrameTest : public testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;

  void SetUp() override {
    llvm::SmallString<kPathCapacity> Path;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(kDirectoryPrefix, Path));
    Directory = Path.str().str();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    neverd_session_set_debug_info_enabled(Session, 0);
  }

  void TearDown() override {
    if (Session)
      neverd_session_destroy(Session);
    if (!Directory.empty() && !HasFailure()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }

  void load(llvm::ArrayRef<uint8_t> Program = dwarf::kStates,
            const ImageOptions &Options = {}) {
    const auto Bytes = makeMachO(Program, Options);
    const auto Path = Directory / kImageFile;
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Output.close();
    ASSERT_TRUE(Output.good());
    ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
        << take(neverd_last_error(Session));
  }

  llvm::json::Value query(uint64_t Offset = 0) {
    auto Result = llvm::json::parse(take(
        neverd_unwind_frame_json(Session, kImageBase + kCodeOffset + Offset)));
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return nullptr;
    }
    return std::move(*Result);
  }

  static const llvm::json::Object *signing(const llvm::json::Value &Value) {
    const auto *Frame = Value.getAsObject();
    EXPECT_NE(Frame, nullptr);
    return Frame ? Frame->getObject(kReturnAddress) : nullptr;
  }
};

} // namespace neverd::unwind_frame_test

#endif // NEVERD_UNITTESTS_UNWINDFRAMETESTSDETAIL_H
