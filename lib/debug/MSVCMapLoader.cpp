//===- MSVCMapLoader.cpp - MSVC linker MAP file loader ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses MSVC linker MAP files (link.exe /MAP) to extract function
/// symbol addresses.  The format has these sections:
///
///   - Header: module name, timestamp, preferred load address
///   - Section table: Start(seg:off) Length Name Class
///   - Public symbols: seg:off name Rva+Base [flags] Lib:Object
///   - Entry point
///   - Static symbols (same format as public symbols)
///
/// See lld/COFF/MapFile.cpp for the reference implementation of the same
/// format produced by lld-link /MAP.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/MSVCMapLoader.h"

#include "neverd/debug/MapDebugContextBase.h"

#define DEBUG_TYPE "neverd-msvc-map-loader"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

std::unique_ptr<MSVCMapDebugContext>
MSVCMapDebugContext::load(const std::filesystem::path &MapPath,
                          uint64_t ImageBase) {
  auto Ctx = std::unique_ptr<MSVCMapDebugContext>(new MSVCMapDebugContext());

  auto BufOr = llvm::MemoryBuffer::getFile(MapPath.string());
  if (!BufOr) {
    llvm::WithColor::warning()
        << "msvc-map: cannot open " << MapPath.string() << "\n";
    return Ctx;
  }

  llvm::StringRef Content = (*BufOr)->getBuffer();
  MapDebugContextBase::parseCOFFMapContent(Content, Ctx->Functions, ImageBase);
  MapDebugContextBase::parseCOFFMapLineNumbers(Content, Ctx->SourceLocations,
                                               ImageBase);

  Ctx->inferFunctionSizes();
  Ctx->Loaded = !Ctx->Functions.empty();
  LLVM_DEBUG(llvm::dbgs() << "msvc-map: loaded " << Ctx->Functions.size()
                          << " function symbols, "
                          << Ctx->SourceLocations.size()
                          << " source locations from "
                          << MapPath.filename().string() << "\n");
  return Ctx;
}

} // namespace neverd
