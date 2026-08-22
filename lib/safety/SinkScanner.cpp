//===- SinkScanner.cpp - Locate catalog call sites in lifted MedIR --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SinkScanner.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"

#include <array>
#include <optional>

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
  const va_t CanonicalAddr =
      In.Img ? normalizeCodeAddress(CalleeAddr, In.Img->Arch, In.Img->Mode)
             : CalleeAddr;
  const std::array<va_t, 2> IdentityAddrs = {CalleeAddr, CanonicalAddr};
  // Address zero is a valid entry in relocatable images, but an unresolved
  // indirect call also uses zero as its sentinel.  A direct call whose only
  // stated identity is the synthesized address name is the occurrence that
  // can safely consult address-keyed identity sources.  A named placeholder
  // relocation and every unresolved indirect call stay name-only.
  const bool HasAddressIdentity =
      CalleeAddr != 0 || (!IsIndirect && isSynthesizedFuncName(StatedName));
  const size_t NumIdentityAddrs =
      HasAddressIdentity ? (CanonicalAddr == CalleeAddr ? 1 : 2) : 0;
  std::optional<ResolvedIdentity> StatedIdentity;

  if (In.Renames) {
    for (size_t I = 0; I < NumIdentityAddrs; ++I)
      if (auto It = In.Renames->find(IdentityAddrs[I]); It != In.Renames->end())
        return {It->second, NameSource::Rename};
  }

  if (In.Img) {
    for (size_t I = 0; I < NumIdentityAddrs; ++I)
      if (std::string Name = importNameAt(*In.Img, IdentityAddrs[I]);
          !Name.empty())
        return {std::move(Name), NameSource::Import};

    if (CalleeAddr == 0) {
      const std::string Norm = SinkCatalog::normalize(StatedName);
      if (!Norm.empty())
        for (const Import &Imp : In.Img->Imports)
          if (SinkCatalog::normalize(Imp.Name) == Norm)
            return {Imp.Name, NameSource::Import};
    }

    for (size_t I = 0; I < NumIdentityAddrs; ++I)
      if (const Export *Exp = In.Img->findExportAt(IdentityAddrs[I]);
          Exp && !Exp->Name.empty() && !isSynthesizedFuncName(Exp->Name))
        return {Exp->Name, NameSource::Export};
    for (size_t I = 0; I < NumIdentityAddrs; ++I)
      if (const Symbol *Sym = In.Img->findSymbolAt(IdentityAddrs[I]);
          Sym && !Sym->Name.empty() && !isSynthesizedFuncName(Sym->Name)) {
        StatedIdentity = ResolvedIdentity{Sym->Name, NameSource::Symbol};
        break;
      }
  }

  if (!StatedName.empty() && !isSynthesizedFuncName(StatedName))
    if (!StatedIdentity)
      StatedIdentity = ResolvedIdentity{StatedName.str(), NameSource::Symbol};

  if (In.Dbg) {
    for (size_t I = 0; I < NumIdentityAddrs; ++I) {
      if (auto Fn = In.Dbg->resolveFunction(IdentityAddrs[I]);
          Fn && !Fn->Name.empty() && !isSynthesizedFuncName(Fn->Name)) {
        if (StatedIdentity && StatedIdentity->Name != Fn->Name)
          return *StatedIdentity;
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
  }

  if (StatedIdentity)
    return *StatedIdentity;

  if (In.SignatureNames) {
    for (size_t I = 0; I < NumIdentityAddrs; ++I)
      if (auto It = In.SignatureNames->find(IdentityAddrs[I]);
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
