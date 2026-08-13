//===- GoModuleData.cpp - Go moduledata location and decoding -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeDetail.h"

#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::go_loader::detail {
namespace {

/// Confirm a candidate funcdata base by resolving real funcdata through it.
/// Open-coded defer records are what make this decisive: each one has to
/// describe a frame this image actually builds, and a base that is off by even
/// a byte turns the pointer-aligned slot offset into an unaligned one.
bool validatesAsFuncDataBase(const ImageReader &R, const FuncLayout &L,
                             const PcHeader &H,
                             const std::vector<RawFunc> &Funcs, va_t Base) {
  if (!R.isMapped(Base))
    return false;
  unsigned Confirmed = 0;
  unsigned Fallback = 0;
  for (const RawFunc &F : Funcs) {
    if (std::optional<va_t> RecordVA =
            getFuncDataAddress(R, L, F, L.OpenCodedDeferInfoIndex, Base)) {
      if (!readsUnderAnyLayout(R, *RecordVA, R.pointerSize(),
                               decodeMaxFrameSize(R, H.PcTab, F.PcSP, H.MinLC)))
        return false;
      ++Confirmed;
    } else if (Confirmed == 0 && Fallback < FuncDataBaseSampleTarget) {
      // Only reached for an image whose functions all defer nothing.  An
      // argument pointer map is a far weaker witness — it merely has to start
      // with a believable bitmap count — but it is better than accepting a
      // base no record was resolved through at all.
      if (std::optional<va_t> MapVA =
              getFuncDataAddress(R, L, F, FuncDataArgsPointerMaps, Base)) {
        std::optional<int32_t> BitmapCount = R.i32(*MapVA);
        if (!BitmapCount || *BitmapCount < 0 || *BitmapCount > (1 << 20))
          return false;
        ++Fallback;
      }
    }
    if (Confirmed >= FuncDataBaseSampleTarget)
      break;
  }
  return Confirmed > 0 || Fallback >= FuncDataBaseSampleTarget;
}

/// Word index of `moduledata.text`.  Everything ahead of it — the header
/// pointer, six slices, the find-func table, and the pc bounds — has been at
/// these positions since Go 1.16.
constexpr unsigned ModuleDataTextWordIndex = 22;
constexpr unsigned ModuleDataMinPCWordIndex = 20;

/// Read the three words at \p Index as a `[]textsect` and confirm every entry
/// describes a real text section.  Used as the structural landmark that fixes
/// where the surrounding fields are, so it is deliberately strict.
bool readTextSectionMap(const ImageReader &R, va_t ModuleVA, unsigned Index,
                        std::vector<TextSection> &Out) {
  std::optional<uint64_t> DataVA = R.wordAt(ModuleVA, Index);
  std::optional<uint64_t> Length = R.wordAt(ModuleVA, Index + 1);
  std::optional<uint64_t> Capacity = R.wordAt(ModuleVA, Index + 2);
  if (!DataVA || !Length || !Capacity)
    return false;
  if (*Length == 0 || *Length != *Capacity || *Length > MaxTextSections)
    return false;
  const uint64_t Stride = uint64_t(3) * R.pointerSize();
  if (*Length > (InvalidVA - *DataVA) / Stride)
    return false;
  std::vector<TextSection> Sections;
  for (uint64_t I = 0; I < *Length; ++I) {
    const va_t Entry = static_cast<va_t>(*DataVA + I * Stride);
    std::optional<uint64_t> VirtualOffset = R.wordAt(Entry, 0);
    std::optional<uint64_t> End = R.wordAt(Entry, 1);
    std::optional<uint64_t> BaseVA = R.wordAt(Entry, 2);
    if (!VirtualOffset || !End || !BaseVA)
      return false;
    if (*VirtualOffset >= *End)
      return false;
    if (!R.isMappedCode(static_cast<va_t>(*BaseVA)))
      return false;
    Sections.push_back({*VirtualOffset, *End, static_cast<va_t>(*BaseVA)});
  }
  Out = std::move(Sections);
  return true;
}

} // namespace

/// Locate `runtime.firstmoduledata` by the one field whose value is already
/// known: it opens with a pointer to the `pcHeader`, immediately followed by
/// the `funcnametab` slice whose base the header also names.  Two independent
/// matches at fixed relative positions is enough to identify the structure
/// without depending on a symbol that a stripped image does not have.
std::optional<va_t> findModuleData(const ImageReader &R, const BinaryImage &Img,
                                   const PcHeader &H) {
  const unsigned PtrSize = R.pointerSize();
  for (const Segment &Seg : Img.Segments) {
    if (Seg.Data.empty())
      continue;
    const uint64_t Limit = std::min<uint64_t>(Seg.Size, Seg.Data.size());
    if (Limit < 2 * PtrSize)
      continue;
    for (uint64_t Off = 0; Off + 2 * PtrSize <= Limit; Off += PtrSize) {
      if (readPtr(Seg.Data.data() + Off, PtrSize == 8) != H.VA)
        continue;
      const va_t Candidate = Seg.VA + Off;
      std::optional<uint64_t> FuncNameTab = R.wordAt(Candidate, 1);
      if (FuncNameTab && *FuncNameTab == H.FuncNameTab)
        return Candidate;
    }
  }
  return std::nullopt;
}

ModuleData decodeModuleData(const ImageReader &R, const BinaryImage &Img,
                            const FuncLayout &L, const PcHeader &H, va_t VA,
                            const std::vector<RawFunc> &Funcs) {
  ModuleData MD;
  MD.VA = VA;

  // `text` is confirmed against `minpc`, which the runtime itself requires to
  // equal the entry of the first function in the table.
  std::optional<uint64_t> MinPC = R.wordAt(VA, ModuleDataMinPCWordIndex);
  std::optional<uint64_t> Text = R.wordAt(VA, ModuleDataTextWordIndex);
  std::optional<uint64_t> EText = R.wordAt(VA, ModuleDataTextWordIndex + 1);
  if (Text && MinPC && !Funcs.empty() && L.EntryIsOffset &&
      *Text + Funcs.front().EntryOffset == *MinPC) {
    MD.TextBase = static_cast<va_t>(*Text);
    if (EText)
      MD.ETextVA = static_cast<va_t>(*EText);
  } else if (Text && R.isMappedCode(static_cast<va_t>(*Text))) {
    MD.TextBase = static_cast<va_t>(*Text);
    if (EText)
      MD.ETextVA = static_cast<va_t>(*EText);
  }

  // An image that kept its symbol table names the base outright.
  for (const Symbol &Sym : Img.Symbols) {
    if (Sym.Name != "go:func.*" && Sym.Name != "go.func.*")
      continue;
    if (validatesAsFuncDataBase(R, L, H, Funcs, Sym.Addr)) {
      MD.GoFuncBase = Sym.Addr;
      break;
    }
  }

  // The funcdata base is one of a run of segment bounds whose length has grown
  // in most releases, so its word index is not fixed.  What is fixed is what
  // comes immediately after it: the `textsectmap` slice, separated from it by
  // at most one word.  A slice is far more recognizable than a bare address —
  // its length and capacity are equal, and each element it points at is a text
  // section whose base is executable — so the search anchors on the slice and
  // then confirms the one or two candidates in front of it against real
  // funcdata.
  for (unsigned Index = ModuleDataTextWordIndex + 2;
       Index < ModuleDataTextWordIndex + MaxModuleDataSearchWords; ++Index) {
    std::vector<TextSection> Sections;
    if (!readTextSectionMap(R, VA, Index, Sections))
      continue;
    if (MD.GoFuncBase != 0) {
      MD.TextSections = std::move(Sections);
      return MD;
    }
    for (unsigned Back = 1; Back <= 2 && Index >= Back; ++Back) {
      std::optional<uint64_t> Candidate = R.wordAt(VA, Index - Back);
      if (!Candidate)
        continue;
      if (!validatesAsFuncDataBase(R, L, H, Funcs,
                                   static_cast<va_t>(*Candidate)))
        continue;
      MD.GoFuncBase = static_cast<va_t>(*Candidate);
      MD.TextSections = std::move(Sections);
      return MD;
    }
  }
  return MD;
}

/// `runtime.moduledata.textAddr`.  Returns nothing when the offset does not
/// land in the address space, rather than the wrapped address that would look
/// like a plausible entry.
std::optional<va_t> resolveTextAddress(const ModuleData &MD, va_t TextBase,
                                       uint64_t Offset) {
  if (MD.TextSections.size() > 1) {
    for (size_t I = 0; I < MD.TextSections.size(); ++I) {
      const TextSection &S = MD.TextSections[I];
      const bool Last = I + 1 == MD.TextSections.size();
      if ((Offset >= S.VirtualOffset && Offset < S.End) ||
          (Last && Offset == S.End)) {
        const uint64_t Delta = Offset - S.VirtualOffset;
        if (Delta > InvalidVA - S.BaseVA)
          return std::nullopt;
        return static_cast<va_t>(S.BaseVA + Delta);
      }
    }
  }
  if (Offset > InvalidVA - TextBase)
    return std::nullopt;
  return static_cast<va_t>(TextBase + Offset);
}

} // namespace neverd::go_loader::detail
