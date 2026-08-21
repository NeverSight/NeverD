//===- SinkScanner.cpp - Locate catalog call sites in lifted MedIR --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SinkScanner.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"

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

struct ResolvedIdentity {
  std::string Name;
  NameSource Source = NameSource::Symbol;
};

const MedOp *opFor(const MedFunc &F, int BlockId, int OpIdx) {
  for (const MedBlock &B : F.Blocks)
    if (B.Id == BlockId)
      return (OpIdx >= 0 && OpIdx < static_cast<int>(B.Ops.size()))
                 ? &B.Ops[OpIdx]
                 : nullptr;
  return nullptr;
}

std::string importNameAt(const BinaryImage &Img, va_t Addr) {
  if (Addr == 0)
    return {};
  if (const Import *Imp = Img.findImportAt(Addr); Imp && !Imp->Name.empty())
    return Imp->Name;
  if (auto It = Img.ImportPtrSlots.find(Addr);
      It != Img.ImportPtrSlots.end() && !It->second.empty())
    return It->second;
  if (auto It = Img.DyldBindSlots.find(Addr);
      It != Img.DyldBindSlots.end() && !It->second.Name.empty())
    return It->second.Name;
  return {};
}

ResolvedIdentity resolveIdentity(const AnalysisInput &In, va_t CalleeAddr,
                                 llvm::StringRef StatedName, bool IsIndirect) {
  if (In.Renames && CalleeAddr) {
    if (auto It = In.Renames->find(CalleeAddr); It != In.Renames->end())
      return {It->second, NameSource::Rename};
  }

  if (In.Img) {
    if (std::string Name = importNameAt(*In.Img, CalleeAddr); !Name.empty())
      return {std::move(Name), NameSource::Import};

    if (IsIndirect || CalleeAddr == 0) {
      const std::string Norm = SinkCatalog::normalize(StatedName);
      if (!Norm.empty())
        for (const Import &Imp : In.Img->Imports)
          if (SinkCatalog::normalize(Imp.Name) == Norm)
            return {Imp.Name, NameSource::Import};
    }
  }

  if (In.Dbg && CalleeAddr) {
    if (auto Fn = In.Dbg->resolveFunction(CalleeAddr);
        Fn && !Fn->Name.empty() && !isSynthesizedFuncName(Fn->Name)) {
      switch (In.DebugKind) {
      case DebugInfoKind::PDB:
        return {Fn->Name, NameSource::Pdb};
      case DebugInfoKind::DWARF:
        return {Fn->Name, NameSource::Dwarf};
      case DebugInfoKind::Map:
        return {Fn->Name, NameSource::Map};
      case DebugInfoKind::None:
        break;
      }
    }
  }

  if (In.Img && CalleeAddr) {
    if (const Export *Exp = In.Img->findExportAt(CalleeAddr);
        Exp && !Exp->Name.empty() && !isSynthesizedFuncName(Exp->Name))
      return {Exp->Name, NameSource::Export};
    if (const Symbol *Sym = In.Img->findSymbolAt(CalleeAddr);
        Sym && !Sym->Name.empty() && !isSynthesizedFuncName(Sym->Name))
      return {Sym->Name, NameSource::Symbol};
  }

  if (In.SignatureNames && CalleeAddr) {
    if (auto It = In.SignatureNames->find(CalleeAddr);
        It != In.SignatureNames->end() && !It->second.empty())
      return {It->second, NameSource::Sig};
  }

  return {StatedName.str(), isSynthesizedFuncName(StatedName)
                                ? NameSource::Synthetic
                                : NameSource::Symbol};
}

} // namespace

std::string neverd::safety::resolveCallName(const AnalysisInput &In,
                                            const MedCallInfo &Call) {
  return resolveIdentity(In, Call.TargetAddr, Call.TargetName, Call.IsIndirect)
      .Name;
}

NameSource neverd::safety::classifyNameSource(const AnalysisInput &In,
                                              va_t CalleeAddr,
                                              llvm::StringRef StatedName,
                                              bool IsIndirect) {
  return resolveIdentity(In, CalleeAddr, StatedName, IsIndirect).Source;
}

std::vector<SinkSite> neverd::safety::scanSinks(const AnalysisInput &In,
                                                const SinkCatalog &Cat) {
  std::vector<SinkSite> Sites;
  if (!In.MedFuncs)
    return Sites;

  for (const MedFunc &F : *In.MedFuncs) {
    for (size_t I = 0; I < F.CallInfos.size(); ++I) {
      const MedCallInfo &CI = F.CallInfos[I];
      ResolvedIdentity Identity =
          resolveIdentity(In, CI.TargetAddr, CI.TargetName, CI.IsIndirect);
      const SinkEntry *Entry = Cat.matchSink(Identity.Name);
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
      Site.StatedName = std::move(Identity.Name);
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
      case SinkKind::StackAlloc:
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

      Site.Source = Identity.Source;
      Sites.push_back(std::move(Site));
    }
  }
  return Sites;
}
