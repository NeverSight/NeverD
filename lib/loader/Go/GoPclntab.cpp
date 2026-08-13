//===- GoPclntab.cpp - Go pcHeader location and decoding ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeDetail.h"

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <optional>

namespace neverd::go_loader::detail {

const char *getMagicVersionName(uint32_t Magic) {
  switch (Magic) {
  case Go12Magic:
    return "go1.2";
  case Go116Magic:
    return "go1.16";
  case Go118Magic:
    return "go1.18";
  case Go120Magic:
    return "go1.20";
  default:
    return "unknown";
  }
}

namespace {

bool isKnownMagic(uint32_t Magic) {
  return Magic == Go12Magic || Magic == Go116Magic || Magic == Go118Magic ||
         Magic == Go120Magic;
}

/// Check the invariants `runtime.moduledataverify1` checks, which is what lets
/// a scan tell a real header from four bytes that happen to match the magic.
bool isPlausibleHeaderAt(const ImageReader &R, const BinaryImage &Img,
                         va_t VA) {
  std::optional<uint32_t> Magic = R.u32(VA);
  if (!Magic || !isKnownMagic(*Magic))
    return false;
  std::optional<uint8_t> Pad1 = R.u8(VA + 4);
  std::optional<uint8_t> Pad2 = R.u8(VA + 5);
  std::optional<uint8_t> MinLC = R.u8(VA + 6);
  std::optional<uint8_t> PtrSize = R.u8(VA + 7);
  if (!Pad1 || !Pad2 || !MinLC || !PtrSize)
    return false;
  if (*Pad1 != 0 || *Pad2 != 0)
    return false;
  // `sys.PCQuantum`: 1 on x86, 4 on the fixed-width RISC targets, 2 on Thumb.
  if (*MinLC != 1 && *MinLC != 2 && *MinLC != 4)
    return false;
  return *PtrSize == (Img.is64Bit() ? 8 : 4);
}

/// The Go 1.2 table is a different structure rather than a shorter one: after
/// the function count there is no offset block at all, and the sub-tables are
/// reached by walking.  Everything an offset in this table names -- a symbol
/// name, a pc-value table, a `_func` record, a file name -- is measured from
/// the start of the header, so the four bases below are all the header itself
/// and only the file table has to be found by walking to it.
std::optional<PcHeader> decodeGo12PcHeader(const ImageReader &R, PcHeader &H) {
  const va_t VA = H.VA;
  H.FuncNameTab = VA;
  H.PcTab = VA;
  H.FuncRecordBase = VA;

  std::optional<uint64_t> FuncCount = R.word(VA + 8);
  if (!FuncCount || *FuncCount > MaxFunctionCount)
    return std::nullopt;
  H.FuncCount = *FuncCount;
  H.FuncTab = VA + 8 + R.pointerSize();

  // `functab` is `[nfunc+1]{entry, funcoff}` of pointer-sized words, but the
  // trailing element is only half a pair: its `entry` closes the last real
  // function and its `funcoff` slot holds the file table's offset instead.
  const uint64_t FuncTabWords = 2 * H.FuncCount + 1;
  if (FuncTabWords > (InvalidVA - H.FuncTab) / R.pointerSize())
    return std::nullopt;
  const va_t FileTabSlot = H.FuncTab + FuncTabWords * R.pointerSize();
  std::optional<uint32_t> FileTabOffset = R.u32(FileTabSlot);
  if (!FileTabOffset || *FileTabOffset == 0 || *FileTabOffset > InvalidVA - VA)
    return std::nullopt;
  H.FileTab = VA + *FileTabOffset;
  std::optional<uint32_t> FileCount = R.u32(H.FileTab);
  if (!FileCount)
    return std::nullopt;
  H.FileCount = *FileCount;
  return H;
}

std::optional<PcHeader> decodePcHeader(const ImageReader &R, va_t VA) {
  PcHeader H;
  H.VA = VA;
  std::optional<uint32_t> Magic = R.u32(VA);
  std::optional<uint8_t> MinLC = R.u8(VA + 6);
  std::optional<uint8_t> PtrSize = R.u8(VA + 7);
  if (!Magic || !MinLC || !PtrSize)
    return std::nullopt;
  H.Magic = *Magic;
  H.MinLC = *MinLC;
  H.PtrSize = *PtrSize;

  if (H.Magic == Go12Magic)
    return decodeGo12PcHeader(R, H);

  // Fields after the fixed eight-byte prefix, in pointer-sized words.
  const va_t Fields = VA + 8;
  auto field = [&](unsigned Index) { return R.wordAt(Fields, Index); };

  std::optional<uint64_t> FuncCount = field(0);
  std::optional<uint64_t> FileCount = field(1);
  if (!FuncCount || !FileCount || *FuncCount > MaxFunctionCount)
    return std::nullopt;
  H.FuncCount = *FuncCount;
  H.FileCount = *FileCount;

  // Go 1.18 inserted `textStart` ahead of the offset block.
  const unsigned OffsetBase = H.Magic == Go116Magic ? 2 : 3;
  if (H.Magic != Go116Magic) {
    if (std::optional<uint64_t> TextStart = field(2))
      H.TextStart = static_cast<va_t>(*TextStart);
  }

  auto resolve = [&](unsigned Index, va_t &Out) {
    std::optional<uint64_t> Offset = field(OffsetBase + Index);
    if (!Offset || *Offset > InvalidVA - VA)
      return false;
    Out = VA + static_cast<va_t>(*Offset);
    return R.isMapped(Out);
  };
  if (!resolve(0, H.FuncNameTab) || !resolve(1, H.CuTab) ||
      !resolve(2, H.FileTab) || !resolve(3, H.PcTab) || !resolve(4, H.FuncTab))
    return std::nullopt;
  H.FuncRecordBase = H.FuncTab;
  return H;
}

} // namespace

/// Find the `pcHeader`.  ELF and Mach-O linkers give the table its own
/// section, but the PE linker folds it into `.rdata`, so a scan of the mapped
/// data is the only way to find it there.  The scan is cheap because the
/// header is pointer aligned and the magic is checked before anything else.
std::optional<PcHeader> findPcHeader(const ImageReader &R,
                                     const BinaryImage &Img) {
  static constexpr const char *SectionNames[] = {
      section_names::elf::GoPclnTab,
      section_names::macho::GoPclnTab,
      ".data.rel.ro.gopclntab",
  };
  for (const char *Name : SectionNames)
    if (const Section *Sec = Img.getSectionByName(Name))
      if (Sec->VA != 0 && isPlausibleHeaderAt(R, Img, Sec->VA))
        if (std::optional<PcHeader> H = decodePcHeader(R, Sec->VA))
          return H;

  // Executable segments are searched too.  Mach-O puts read-only data in
  // `__TEXT`, so on that format the table lives in a segment an
  // ELF-shaped assumption would skip.
  const unsigned Step = R.pointerSize();
  for (const Segment &Seg : Img.Segments) {
    if (Seg.Data.empty())
      continue;
    const uint64_t Limit = std::min<uint64_t>(Seg.Size, Seg.Data.size());
    if (Limit < 8)
      continue;
    for (uint64_t Off = 0; Off + 8 <= Limit; Off += Step) {
      if (!isKnownMagic(readLE<uint32_t>(Seg.Data.data() + Off)))
        continue;
      const va_t VA = Seg.VA + Off;
      if (!isPlausibleHeaderAt(R, Img, VA))
        continue;
      if (std::optional<PcHeader> H = decodePcHeader(R, VA))
        return H;
    }
  }
  return std::nullopt;
}

} // namespace neverd::go_loader::detail
