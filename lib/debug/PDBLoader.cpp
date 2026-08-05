//===- PDBLoader.cpp - PDB debug info loader -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PDB debug information loading implementation.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/PDBLoader.h"

#define DEBUG_TYPE "neverd-pdb-loader"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace neverd {

struct PDBDebugContext::Impl {
  std::map<va_t, FunctionSym> Functions;
  uint64_t ImageBase = 0;
  bool Loaded = false;
};

PDBDebugContext::~PDBDebugContext() = default;

std::unique_ptr<PDBDebugContext>
PDBDebugContext::load(const std::filesystem::path &PdbPath,
                      uint64_t ImageBase) {

  auto Ctx = std::unique_ptr<PDBDebugContext>(new PDBDebugContext());
  Ctx->PImpl = std::make_unique<Impl>();
  Ctx->PImpl->ImageBase = ImageBase;

  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  auto Err =
      llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                llvm::StringRef(PdbPath.string()), Session);
  if (Err) {
    llvm::consumeError(std::move(Err));
    llvm::WithColor::error()
        << "pdb: failed to open " << PdbPath.string() << "\n";
    return Ctx;
  }

  auto *Native = static_cast<llvm::pdb::NativeSession *>(Session.get());
  auto &PDB = Native->getPDBFile();

  auto DbiOr = PDB.getPDBDbiStream();
  if (!DbiOr) {
    llvm::consumeError(DbiOr.takeError());
    llvm::WithColor::warning() << "pdb: no DBI stream\n";
    return Ctx;
  }
  auto &DBI = *DbiOr;

  auto SecHeaders = DBI.getSectionHeaders();

  auto ResolveVA = [&](uint16_t Seg, uint32_t Off) -> va_t {
    if (Seg == 0 || Seg > SecHeaders.size())
      return 0;
    return ImageBase + SecHeaders[Seg - 1].VirtualAddress + Off;
  };

  auto PubOr = PDB.getPDBPublicsStream();
  if (PubOr) {
    auto &Publics = *PubOr;
    auto SymOr = PDB.getPDBSymbolStream();
    if (SymOr) {
      auto &Syms = *SymOr;
      auto Records = Syms.getSymbolArray();

      for (uint32_t Off : Publics.getPublicsTable()) {
        auto SymRef = Records.at(Off);
        llvm::codeview::CVSymbol CVS = *SymRef;
        if (CVS.kind() != llvm::codeview::SymbolKind::S_PUB32)
          continue;

        llvm::codeview::PublicSym32 PubRec(
            llvm::codeview::SymbolRecordKind::PublicSym32);
        if (auto EC = llvm::codeview::SymbolDeserializer::deserializeAs<
                llvm::codeview::PublicSym32>(CVS, PubRec)) {
          llvm::consumeError(std::move(EC));
          continue;
        }

        bool IsFunc =
            (static_cast<uint32_t>(PubRec.Flags) &
             static_cast<uint32_t>(llvm::codeview::PublicSymFlags::Function)) !=
            0;
        if (!IsFunc)
          continue;

        va_t VA = ResolveVA(PubRec.Segment, PubRec.Offset);
        if (VA != 0) {
          FunctionSym FS;
          FS.Name = std::string(PubRec.Name);
          FS.Addr = VA;
          Ctx->PImpl->Functions[VA] = std::move(FS);
        }
      }
    } else {
      llvm::consumeError(SymOr.takeError());
    }
  } else {
    llvm::consumeError(PubOr.takeError());
  }

  Ctx->PImpl->Loaded = !Ctx->PImpl->Functions.empty();
  LLVM_DEBUG(llvm::dbgs() << "pdb: loaded " << Ctx->PImpl->Functions.size()
                          << " function symbols from "
                          << PdbPath.filename().string() << "\n");
  return Ctx;
}

std::optional<FunctionSym> PDBDebugContext::resolveFunction(va_t Addr) const {
  if (!PImpl)
    return std::nullopt;
  auto It = PImpl->Functions.find(Addr);
  if (It != PImpl->Functions.end())
    return It->second;
  auto LB = PImpl->Functions.lower_bound(Addr);
  if (LB != PImpl->Functions.begin()) {
    --LB;
    if (LB->second.contains(Addr))
      return LB->second;
  }
  return std::nullopt;
}

std::optional<VariableSym> PDBDebugContext::resolveVariable(va_t,
                                                            int64_t) const {
  return std::nullopt;
}

std::optional<TypeSym> PDBDebugContext::resolveType(uint64_t) const {
  return std::nullopt;
}

std::optional<SourceLoc> PDBDebugContext::sourceLocation(va_t) const {
  return std::nullopt;
}

std::vector<FunctionSym> PDBDebugContext::allFunctions() const {
  std::vector<FunctionSym> Result;
  if (!PImpl)
    return Result;
  Result.reserve(PImpl->Functions.size());
  for (auto &[_, FS] : PImpl->Functions)
    Result.push_back(FS);
  return Result;
}

bool PDBDebugContext::hasInfo() const { return PImpl && PImpl->Loaded; }

} // namespace neverd
