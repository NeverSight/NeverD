//===- DebugInfoDiscovery.cpp - Locate and load debug info ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Chooses which debug format applies to an image, finds the file that carries
/// it, and publishes the resulting names into the image's symbol table.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/DebugInfoDiscovery.h"

#include "neverd/Common.h"
#include "neverd/debug/DWARFLoader.h"
#include "neverd/debug/LLDMapLoader.h"
#include "neverd/debug/MSVCMapLoader.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <span>
#include <system_error>

#define DEBUG_TYPE "neverd-debug-discovery"

namespace neverd {

namespace {

bool isReadableFile(const std::filesystem::path &P) {
  if (P.empty())
    return false;
  std::error_code EC;
  return std::filesystem::is_regular_file(P, EC);
}

void addCandidate(std::vector<std::filesystem::path> &Out,
                  std::filesystem::path Candidate) {
  if (Candidate.empty())
    return;
  if (std::find(Out.begin(), Out.end(), Candidate) == Out.end())
    Out.push_back(std::move(Candidate));
}

/// Base name of a path a linker recorded inside the image.  Such a path is
/// spelled for the machine that produced the binary, so it may use Windows
/// separators no matter which host NeverD runs on.
llvm::StringRef recordedFileName(llvm::StringRef Recorded) {
  size_t Pos = Recorded.find_last_of("/\\");
  return Pos == llvm::StringRef::npos ? Recorded : Recorded.substr(Pos + 1);
}

/// PDB files a PE image points at or conventionally sits beside.
std::vector<std::filesystem::path>
pdbCandidates(const std::filesystem::path &BinaryPath, const BinaryImage &Img) {
  std::vector<std::filesystem::path> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();

  // The CodeView debug directory records the path the linker wrote the PDB to.
  // It resolves on the machine that produced the binary; anywhere else only
  // the file name survives the trip, which is why that name is also retried
  // next to the image.
  llvm::StringRef Recorded(Img.DynInfo.PDBPath);
  if (!Recorded.empty() && !Recorded.starts_with(kBuildIdPrefix)) {
    addCandidate(Out, std::filesystem::path(Recorded.str()));
    addCandidate(Out, Dir / recordedFileName(Recorded).str());
  }

  addCandidate(Out, Dir / (BinaryPath.stem().string() + ".pdb"));
  return Out;
}

/// Companion files that hold the DWARF stripped out of an ELF image, in the
/// layouts objcopy --only-keep-debug and the distro debug packages produce.
std::vector<std::filesystem::path>
splitDwarfCandidates(const std::filesystem::path &BinaryPath) {
  std::vector<std::filesystem::path> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();
  const std::string Name = BinaryPath.filename().string();

  addCandidate(Out, Dir / (Name + ".debug"));
  addCandidate(Out, Dir / (BinaryPath.stem().string() + ".debug"));
  addCandidate(Out, Dir / ".debug" / Name);
  return Out;
}

std::vector<std::filesystem::path>
mapCandidates(const std::filesystem::path &BinaryPath) {
  std::vector<std::filesystem::path> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();

  addCandidate(Out, Dir / (BinaryPath.stem().string() + ".map"));
  addCandidate(Out, Dir / (BinaryPath.filename().string() + ".map"));
  return Out;
}

DebugInfoResult loadPDB(const std::filesystem::path &P,
                        const BinaryImage &Img) {
  DebugInfoResult R;
  auto CtxOr = PDBDebugContext::load(P, Img);
  if (!CtxOr) {
    R.Error = llvm::toString(CtxOr.takeError());
    return R;
  }
  auto Ctx = std::move(*CtxOr);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::PDB;
    R.Path = P;
  } else {
    R.Error = "no function symbols in " + P.string();
  }
  return R;
}

DebugInfoResult loadDWARF(const std::filesystem::path &P, BinaryFormat Format,
                          DWARFLoadTrust Trust,
                          std::span<const uint8_t> ExpectedImageBytes = {}) {
  DebugInfoResult R;
  auto Ctx = DWARFDebugContext::load(P, Format, Trust, ExpectedImageBytes);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::DWARF;
    R.Path = P;
  }
  return R;
}

DebugInfoResult loadMap(const std::filesystem::path &P,
                        const BinaryImage &Img) {
  DebugInfoResult R;

  // A COFF /MAP states addresses as segment:offset, so it only resolves
  // against an image base, and link.exe appends line-number tables the other
  // dialects have no equivalent for.  MSVCMapDebugContext is the variant that
  // reads both.  The remaining four dialects carry whole addresses and go
  // through the auto-detecting loader, which also covers a PE linked with
  // lld's /lldmap.
  if (Img.Format == BinaryFormat::COFF) {
    auto MSVCCtx = MSVCMapDebugContext::load(P, Img.Base);
    if (MSVCCtx && MSVCCtx->hasInfo()) {
      R.Context = std::move(MSVCCtx);
      R.Kind = DebugInfoKind::Map;
      R.Path = P;
      return R;
    }
  }

  auto Ctx = LLDMapDebugContext::load(P, Img.isCOFF() ? Img.Base : 0);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::Map;
    R.Path = P;
  }
  return R;
}

} // anonymous namespace

const char *debugInfoKindName(DebugInfoKind Kind) {
  switch (Kind) {
  case DebugInfoKind::None:
    return "none";
  case DebugInfoKind::DWARF:
    return "dwarf";
  case DebugInfoKind::PDB:
    return "pdb";
  case DebugInfoKind::Map:
    return "map";
  }
  return "none";
}

DebugInfoResult loadDebugInfo(const std::filesystem::path &BinaryPath,
                              const BinaryImage &Img,
                              const DebugInfoRequest &Req) {
  DebugInfoResult Result;
  if (!Req.Enabled)
    return Result;

  if (!Req.PDBPath.empty()) {
    if (!isReadableFile(Req.PDBPath)) {
      Result.Error = "PDB file not found: " + Req.PDBPath.string();
      return Result;
    }
    Result = loadPDB(Req.PDBPath, Img);
    if (!Result && Result.Error.empty())
      Result.Error = "no function symbols in " + Req.PDBPath.string();
    return Result;
  }

  if (!Req.MapPath.empty()) {
    if (!isReadableFile(Req.MapPath)) {
      Result.Error = "MAP file not found: " + Req.MapPath.string();
      return Result;
    }
    Result = loadMap(Req.MapPath, Img);
    if (!Result)
      Result.Error = "no function symbols in " + Req.MapPath.string();
    return Result;
  }

  // EVM and SBF describe themselves through their own metadata — an ABI JSON,
  // an Anchor IDL — and no producer emits DWARF, PDB, or a linker MAP for
  // either.  Probing for those files would only cost stat calls.
  if (Img.Arch == Arch::EVM || Img.Arch == Arch::SBF)
    return Result;

  switch (Img.Format) {
  case BinaryFormat::COFF:
    for (const std::filesystem::path &C : pdbCandidates(BinaryPath, Img)) {
      if (!isReadableFile(C))
        continue;
      DebugInfoResult Attempt = loadPDB(C, Img);
      if (Attempt)
        return Attempt;
      LLVM_DEBUG(llvm::dbgs()
                 << "debug-discovery: rejected PDB candidate " << C.string()
                 << (Attempt.Error.empty() ? "" : ": ") << Attempt.Error
                 << "\n");
    }
    break;

  case BinaryFormat::ELF:
  case BinaryFormat::MachO:
    // Covers in-image .debug_info and, for Mach-O, an adjacent .dSYM bundle.
    Result =
        loadDWARF(BinaryPath, Img.Format, DWARFLoadTrust::InImage, Img.Raw);
    if (Result)
      return Result;
    for (const std::filesystem::path &C : splitDwarfCandidates(BinaryPath)) {
      if (!isReadableFile(C))
        continue;
      // Split ELF/Mach-O candidates remain useful for names and lines, but
      // cannot publish object extents until build-id/debuglink identity is
      // authenticated.  dSYM discovery from the primary Mach-O is handled by
      // DWARFDebugContext and separately verifies LC_UUID.
      Result =
          loadDWARF(C, Img.Format, DWARFLoadTrust::UnauthenticatedCompanion);
      if (Result)
        return Result;
    }
    break;

  default:
    break;
  }

  // A MAP carries names and addresses but no types, source lines, or build
  // identity, so it is the last resort: it exists for a stripped binary whose
  // build kept nothing else.
  for (const std::filesystem::path &C : mapCandidates(BinaryPath)) {
    if (!isReadableFile(C))
      continue;
    Result = loadMap(C, Img);
    if (Result)
      return Result;
  }

  return Result;
}

unsigned applyDebugSymbols(BinaryImage &Img, const DebugContext &Dbg) {
  const std::vector<FunctionSym> DbgFuncs = Dbg.allFunctions();
  if (Dbg.hasAuthenticatedObjectExtents()) {
    for (const DataObjectSym &Object : Dbg.allDataObjects()) {
      if (Object.Addr == InvalidVA || Object.Size == 0 ||
          Object.Size > InvalidVA - Object.Addr)
        continue;
      Img.ExactDataObjects.push_back(ExactDataObjectExtent{
          Object.Addr, Object.Size, ExactDataObjectEvidence::AuthenticatedDebug,
          Object.IsBuffer ? ExactDataObjectPrecision::TypedBuffer
                          : ExactDataObjectPrecision::TypedNonBuffer});
    }
  }

  if (DbgFuncs.empty())
    return 0;

  // Index the function symbols once rather than scanning the table per debug
  // entry; both sides run to tens of thousands of entries on a real image.
  std::map<va_t, size_t> ByAddr;
  for (size_t I = 0; I < Img.Symbols.size(); ++I) {
    const Symbol &S = Img.Symbols[I];
    if (!S.IsFunc)
      continue;
    auto [It, Inserted] = ByAddr.try_emplace(S.Addr, I);
    // One address can carry both a stated name and a placeholder, when a
    // discovery pass minted `sub_` for code the symbol table also describes.
    // The stated one is what decides whether debug info may speak here.
    if (!Inserted && isSynthesizedFuncName(Img.Symbols[It->second].Name) &&
        !isSynthesizedFuncName(S.Name))
      It->second = I;
  }

  unsigned Applied = 0;
  for (const FunctionSym &FS : DbgFuncs) {
    if (FS.Name.empty() || FS.Addr == 0)
      continue;

    auto It = ByAddr.find(FS.Addr);
    if (It == ByAddr.end()) {
      Symbol New;
      New.Name = FS.Name;
      New.Addr = FS.Addr;
      New.Size = FS.Size;
      New.IsFunc = true;
      Img.Symbols.push_back(std::move(New));
      ByAddr.emplace(FS.Addr, Img.Symbols.size() - 1);
      ++Applied;
      continue;
    }

    Symbol &Existing = Img.Symbols[It->second];
    if (!isSynthesizedFuncName(Existing.Name))
      continue;
    Existing.Name = FS.Name;
    if (Existing.Size == 0)
      Existing.Size = FS.Size;
    ++Applied;
  }

  LLVM_DEBUG(llvm::dbgs() << "debug-discovery: published " << Applied << " of "
                          << DbgFuncs.size() << " debug function symbols\n");
  return Applied;
}

} // namespace neverd
