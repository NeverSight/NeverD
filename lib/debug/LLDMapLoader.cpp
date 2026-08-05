//===- LLDMapLoader.cpp - LLD linker MAP file loader ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Format detection and public API for linker MAP file loading.
/// Supports five formats:
///
/// 1. ELF-style  (ld.lld --Map=)     — ELFMapParser.cpp
/// 2. COFF /MAP  (lld-link /MAP:)    — MapDebugContextBase::parseCOFFMapContent
/// 3. COFF /lldmap (lld-link /lldmap:) — COFFLLDMapParser.cpp
/// 4. MachO-style (ld64.lld -map)    — MachOMapParser.cpp
/// 5. GNU ld     (ld --Map=)         — GNUldMapParser.cpp
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/LLDMapLoader.h"

#include "MapParsers.h"

#include "neverd/debug/MapDebugContextBase.h"

#define DEBUG_TYPE "neverd-lld-map-loader"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

namespace {

//===----------------------------------------------------------------------===//
// Format detection
//===----------------------------------------------------------------------===//

enum class MapFormat { ELF, COFFStandard, COFFLLDMap, MachO, GNUld };

MapFormat detectFormat(llvm::StringRef Content) {
  if (Content.starts_with("# Path:") || Content.ltrim().starts_with("# Path:"))
    return MapFormat::MachO;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    if (isGNUldMapHeader(Line))
      return MapFormat::GNUld;

    if (isELFMapHeader(Line))
      return MapFormat::ELF;

    if (isLLDMapHeader(Line))
      return MapFormat::COFFLLDMap;

    if (MapDebugContextBase::isCOFFMapHeader(Line))
      return MapFormat::COFFStandard;
  }

  if (Content.contains("VMA") && Content.contains("LMA"))
    return MapFormat::ELF;

  return MapFormat::COFFStandard;
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

std::unique_ptr<LLDMapDebugContext>
LLDMapDebugContext::load(const std::filesystem::path &MapPath) {
  auto Ctx = std::unique_ptr<LLDMapDebugContext>(new LLDMapDebugContext());

  auto BufOr = llvm::MemoryBuffer::getFile(MapPath.string());
  if (!BufOr) {
    llvm::WithColor::warning()
        << "lld-map: cannot open " << MapPath.string() << "\n";
    return Ctx;
  }

  llvm::StringRef Content = (*BufOr)->getBuffer();
  MapFormat Fmt = detectFormat(Content);

  [[maybe_unused]] const char *FmtName = "";
  switch (Fmt) {
  case MapFormat::ELF:
    parseELFMap(Content, Ctx->Functions);
    FmtName = "ELF";
    break;
  case MapFormat::COFFStandard:
    MapDebugContextBase::parseCOFFMapContent(Content, Ctx->Functions);
    FmtName = "COFF /MAP";
    break;
  case MapFormat::COFFLLDMap:
    parseCOFFLLDMap(Content, Ctx->Functions);
    FmtName = "COFF /lldmap";
    break;
  case MapFormat::MachO:
    parseMachOMap(Content, Ctx->Functions);
    FmtName = "MachO";
    break;
  case MapFormat::GNUld:
    parseGNUldMap(Content, Ctx->Functions);
    FmtName = "GNU ld";
    break;
  }

  Ctx->inferFunctionSizes();
  Ctx->Loaded = !Ctx->Functions.empty();
  LLVM_DEBUG(llvm::dbgs() << "lld-map: loaded " << Ctx->Functions.size()
                          << " function symbols from "
                          << MapPath.filename().string() << " (" << FmtName
                          << " style)\n");
  return Ctx;
}

} // namespace neverd
