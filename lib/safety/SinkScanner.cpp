//===- SinkScanner.cpp - Locate catalog call sites in lifted MedIR --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SinkScanner.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/debug/DebugContext.h"

using namespace neverd;
using namespace neverd::safety;

const MedFunc *AnalysisInput::findMedFunc(va_t Entry) const {
  if (!MedFuncs)
    return nullptr;
  for (const MedFunc &F : *MedFuncs)
    if (F.Entry == Entry)
      return &F;
  return nullptr;
}

const LowFunc *AnalysisInput::findLowFunc(va_t Entry) const {
  if (!LowFuncs)
    return nullptr;
  for (const LowFunc &F : *LowFuncs)
    if (F.Entry == Entry)
      return &F;
  return nullptr;
}

namespace {

const MedOp *opFor(const MedFunc &F, int BlockId, int OpIdx) {
  for (const MedBlock &B : F.Blocks)
    if (B.Id == BlockId)
      return (OpIdx >= 0 && OpIdx < static_cast<int>(B.Ops.size()))
                 ? &B.Ops[OpIdx]
                 : nullptr;
  return nullptr;
}

bool isImportAddress(const BinaryImage &Img, va_t Addr) {
  if (Addr == 0)
    return false;
  if (Img.findImportAt(Addr))
    return true;
  if (Img.ImportPtrSlots.count(Addr))
    return true;
  if (Img.DyldBindSlots.count(Addr))
    return true;
  return false;
}

} // namespace

NameSource neverd::safety::classifyNameSource(const AnalysisInput &In,
                                              va_t CalleeAddr,
                                              llvm::StringRef StatedName,
                                              bool IsIndirect) {
  if (In.Renames && CalleeAddr && In.Renames->count(CalleeAddr))
    return NameSource::Rename;

  if (In.Img) {
    if (CalleeAddr && isImportAddress(*In.Img, CalleeAddr))
      return NameSource::Import;
    if (IsIndirect || CalleeAddr == 0) {
      const std::string Norm = SinkCatalog::normalize(StatedName);
      if (!Norm.empty())
        for (const Import &Imp : In.Img->Imports)
          if (SinkCatalog::normalize(Imp.Name) == Norm)
            return NameSource::Import;
    }
  }

  if (In.Dbg && CalleeAddr) {
    if (auto Fn = In.Dbg->resolveFunction(CalleeAddr);
        Fn && !Fn->Name.empty() && !isSynthesizedFuncName(Fn->Name)) {
      switch (In.DebugKind) {
      case DebugInfoKind::PDB:
        return NameSource::Pdb;
      case DebugInfoKind::DWARF:
        return NameSource::Dwarf;
      case DebugInfoKind::Map:
        return NameSource::Map;
      case DebugInfoKind::None:
        break;
      }
    }
  }

  if (In.Img && CalleeAddr) {
    if (In.Img->findExportAt(CalleeAddr))
      return NameSource::Export;
    if (In.Img->findSymbolAt(CalleeAddr))
      return NameSource::Symbol;
  }

  if (In.SignatureNamed && CalleeAddr && In.SignatureNamed->count(CalleeAddr))
    return NameSource::Sig;

  if (isSynthesizedFuncName(StatedName))
    return NameSource::Synthetic;

  return NameSource::Symbol;
}

std::vector<SinkSite> neverd::safety::scanSinks(const AnalysisInput &In,
                                                const SinkCatalog &Cat) {
  std::vector<SinkSite> Sites;
  if (!In.MedFuncs)
    return Sites;

  for (const MedFunc &F : *In.MedFuncs) {
    for (size_t I = 0; I < F.CallInfos.size(); ++I) {
      const MedCallInfo &CI = F.CallInfos[I];
      const SinkEntry *Entry = Cat.matchSink(CI.TargetName);
      if (!Entry)
        continue;

      SinkSite Site;
      Site.FuncEntry = F.Entry;
      Site.FuncName = F.Name;
      Site.BlockId = CI.BlockId;
      Site.OpIdx = CI.OpIdx;
      Site.CallInfoIndex = I;
      if (const MedOp *Op = opFor(F, CI.BlockId, CI.OpIdx))
        Site.CallVA = Op->Addr;
      Site.StatedName = CI.TargetName;
      Site.Sink = Entry->Name;
      Site.Class = Entry->Class;
      Site.Kind = Entry->Kind;
      Site.IsIndirect = CI.IsIndirect;

      switch (Entry->Kind) {
      case SinkKind::Copy:
      case SinkKind::Format:
        Site.ArgIndex = Entry->decidingArg();
        break;
      case SinkKind::Alloc:
      case SinkKind::Realloc:
        Site.ArgIndex = Entry->LenArg;
        break;
      case SinkKind::Free:
        Site.ArgIndex = Entry->HandleArg;
        break;
      default:
        Site.ArgIndex = -1;
        break;
      }

      Site.Source = classifyNameSource(In, CI.TargetAddr, CI.TargetName,
                                       CI.IsIndirect);
      Sites.push_back(std::move(Site));
    }
  }
  return Sites;
}
