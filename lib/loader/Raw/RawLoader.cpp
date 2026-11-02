//===- RawLoader.cpp - Binary files no header describes -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/Raw/RawLoader.h"

#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/support/FilePath.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd {

namespace {

#define NEVERD_RAW_TEXT(Id, Text) constexpr llvm::StringLiteral k##Id(Text);
#include "neverd/loader/Raw/RawLoader.def"

llvm::Error rawError(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>("binary: " + Message,
                                             llvm::inconvertibleErrorCode());
}

} // namespace

std::optional<std::pair<Arch, InstructionMode>>
parseRawProcessor(llvm::StringRef Name) {
#define NEVERD_RAW_PROCESSOR(Spelling, TheArch, TheMode)                       \
  if (Name == Spelling)                                                        \
    return std::make_pair(Arch::TheArch, InstructionMode::TheMode);
#include "neverd/loader/Raw/RawLoader.def"
  return std::nullopt;
}

llvm::StringRef getRawProcessorName(Arch TheArch, InstructionMode Mode) {
#define NEVERD_RAW_PROCESSOR(Spelling, A, M)                                   \
  if (TheArch == Arch::A && Mode == InstructionMode::M)                        \
    return Spelling;
#include "neverd/loader/Raw/RawLoader.def"
  return {};
}

std::optional<BinaryFormat> parseRawPlatform(llvm::StringRef Name) {
#define NEVERD_RAW_PLATFORM(Spelling, Format, Text)                            \
  if (Name == Spelling)                                                        \
    return BinaryFormat::Format;
#include "neverd/loader/Raw/RawLoader.def"
  return std::nullopt;
}

llvm::StringRef getRawPlatformName(BinaryFormat Format) {
#define NEVERD_RAW_PLATFORM(Spelling, F, Text)                                 \
  if (Format == BinaryFormat::F)                                               \
    return Spelling;
#include "neverd/loader/Raw/RawLoader.def"
  return {};
}

llvm::StringRef getRawPlatformText(BinaryFormat Format) {
#define NEVERD_RAW_PLATFORM(Spelling, F, Text)                                 \
  if (Format == BinaryFormat::F)                                               \
    return Text;
#include "neverd/loader/Raw/RawLoader.def"
  return {};
}

llvm::Expected<BinaryImage> RawLoader::load(const std::filesystem::path &Path) {
  if (getRawProcessorName(Options.TheArch, Options.Mode).empty())
    return rawError("no processor to read the file as");
  BinaryImage Img;
  auto Buffer = readFileInto(Path, Img, BinaryFormat::Raw, true, Progress);
  if (!Buffer)
    return Buffer.takeError();
  // The bytes the options name, all of them, at the address they name.
  const uint64_t FileSize = Img.Raw.size();
  if (Options.Offset >= FileSize)
    return rawError(llvm::formatv("file offset {0:x} is not before the end of "
                                  "the file ({1} bytes)",
                                  Options.Offset, FileSize));
  const uint64_t Available = FileSize - Options.Offset;
  if (Options.Size > Available)
    return rawError(llvm::formatv("{0} bytes from offset {1:x} pass the end of "
                                  "the file, which holds {2} there",
                                  Options.Size, Options.Offset, Available));
  const uint64_t Size = Options.Size ? Options.Size : Available;
  const bool Wide =
      Options.TheArch == Arch::X64 || Options.TheArch == Arch::AArch64;
  if (Options.Base + Size < Options.Base)
    return rawError("the bytes do not fit above the base address");
  // A 32-bit processor's addresses end at 4 GiB.
  if (!Wide && Options.Base + Size > (uint64_t(1) << 32))
    return rawError("the bytes do not fit below 4 GiB, where a 32-bit "
                    "processor's addresses end");
  const va_t Entry = Options.Entry.value_or(Options.Base);
  if (Entry < Options.Base || Entry - Options.Base >= Size)
    return rawError(
        llvm::formatv("entry {0:x} is outside the loaded bytes", Entry));
  Img.Arch = Options.TheArch;
  Img.Mode = Options.Mode;
  Img.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
  Img.Base = Options.Base;
  Img.Entry = Entry;
  // Unknown until detection reads the platform from the code.
  Img.ConventionFormat = Options.Platform.value_or(BinaryFormat::Unknown);
  Img.IsRelocatable = false;

  const llvm::ArrayRef<uint8_t> Bytes(Img.Raw.data() + Options.Offset, Size);
  const auto Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Segment Code;
  Code.Name = kSegmentName.str();
  Code.VA = Options.Base;
  Code.Size = Size;
  Code.FileOff = Options.Offset;
  Code.FileSz = Size;
  Code.Flags = Flags;
  copyImageBytes(Bytes, Code.Data, Progress);
  Img.Segments.push_back(std::move(Code));
  Section CodeSection;
  CodeSection.Name = kSegmentName.str();
  CodeSection.SegmentName = kSegmentName.str();
  CodeSection.VA = Options.Base;
  CodeSection.Size = Size;
  CodeSection.FileOff = Options.Offset;
  CodeSection.FileSz = Size;
  CodeSection.Flags = Flags;
  copyImageBytes(Bytes, CodeSection.Data, Progress);
  Img.Sections.push_back(std::move(CodeSection));
  Img.addSymbol(kEntryName, Entry, 0, /*IsFunc=*/true);
  runPostLoadDiscovery(Img, "binary: loaded " + pathToUTF8(Path.filename()),
                       Progress);
  return Img;
}

} // namespace neverd
