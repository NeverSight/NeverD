//===- COFFTrampolineRegions.cpp - Authenticated PE entry storage --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// An exact source identity selects its executable owner, including an earlier
/// generated section. Each write must have unique virtual and physical storage
/// and match the bytes used for source analysis.
//===----------------------------------------------------------------------===//

#include "COFFTrampolineRegions.h"

#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/Support/Errc.h"

#include <algorithm>

namespace neverd {
llvm::Expected<std::vector<TextLayout>> collectCOFFSourceTrampolineRegions(
    llvm::ArrayRef<uint8_t> Binary, const BinaryImage &Image,
    const std::map<std::string, uint64_t> &OriginalVAs) {
  auto Reject = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "coff source entry: " + Detail);
  };
  auto PE =
      locatePEHeaders(const_cast<uint8_t *>(Binary.data()), Binary.size());
  if (!PE.valid() || Image.Format != BinaryFormat::COFF ||
      getPEImageBase(PE) != Image.Base)
    return Reject("input image identity changed");
  std::vector<PESectionFields> Fields;
  std::vector<coff_loader::detail::RawBackedSectionRange> Sections;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    Fields.push_back(Section);
    Sections.push_back(
        {Section.VirtualAddress,
         getPESectionContentSize(Section.VirtualSize, Section.SizeOfRawData),
         Section.PointerToRawData, Section.SizeOfRawData});
  });
  std::vector<TextLayout> Regions;
  for (const auto &[Name, Entry] : OriginalVAs) {
    const auto Mode = Image.instructionModeAt(Entry);
    const auto Width =
        Mode ? getTargetCodegenInfo(Image.Arch, *Mode).trampolineSize() : 0;
    if (!Width || !Image.hasAuthenticatedFunctionEntryAt(Entry) ||
        Entry < Image.Base || Entry - Image.Base > UINT32_MAX)
      return Reject("source entry has no checked instruction-mode contract");
    const uint32_t RVA = uint32_t(Entry - Image.Base);
    auto Raw = coff_loader::detail::resolveUniqueRawBackedFileOffset(
        Sections, Binary.size(), RVA, Width);
    if (!Raw)
      return Reject(llvm::toString(Raw.takeError()));
    if (*Raw < getPESizeOfHeaders(PE))
      return Reject("source entry aliases PE headers");
    unsigned Owners = 0;
    for (const auto &Section : Fields) {
      if (!Section.SizeOfRawData ||
          *Raw >= uint64_t(Section.PointerToRawData) + Section.SizeOfRawData ||
          Section.PointerToRawData >= *Raw + Width)
        continue;
      if (++Owners != 1 ||
          !(Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) ||
          RVA < Section.VirtualAddress ||
          uint64_t(Section.PointerToRawData) + RVA - Section.VirtualAddress !=
              *Raw)
        return Reject(
            "source entry has ambiguous or non-executable raw storage");
    }
    const auto *Source = Image.readVA(Entry, Width);
    if (Owners != 1 || !Source ||
        !std::equal(Source, Source + Width, Binary.begin() + *Raw))
      return Reject("source entry bytes changed after analysis");
    Regions.push_back({*Raw, RVA, Width});
  }
  return Regions;
}
} // namespace neverd
