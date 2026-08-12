// Scratch harness: dumps decoded exception metadata for a real binary.
//
// Not a test.  It is skipped unless NEVERD_SCRATCH_BINARY names an image, and
// exists so a decoder can be pointed at a binary the corpus does not pin while
// it is being developed.
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <map>
#include <string>

using namespace neverd;

namespace {

bool wants(const char *Name) { return std::getenv(Name) != nullptr; }

unsigned limitOr(const char *Name, unsigned Default) {
  if (const char *V = std::getenv(Name))
    return static_cast<unsigned>(std::strtoul(V, nullptr, 0));
  return Default;
}

va_t addrOr(const char *Name, va_t Default) {
  if (const char *V = std::getenv(Name))
    return static_cast<va_t>(std::strtoull(V, nullptr, 0));
  return Default;
}

void dumpSections(const BinaryImage &Img) {
  for (const Segment &S : Img.Segments)
    llvm::errs() << "seg " << S.Name << " va=0x" << llvm::utohexstr(S.VA)
                 << " size=0x" << llvm::utohexstr(S.Size) << " data=0x"
                 << llvm::utohexstr(S.Data.size()) << "\n";
  for (const Section &S : Img.Sections)
    llvm::errs() << "sec " << S.Name << " va=0x" << llvm::utohexstr(S.VA)
                 << " size=0x" << llvm::utohexstr(S.Size) << "\n";
}

/// Function symbols in an address window, which is how to tell code the loader
/// published from code only the tables know about.
void dumpFuncSymbols(const BinaryImage &Img, va_t Low, va_t High) {
  std::vector<const Symbol *> Sorted;
  for (const Symbol &S : Img.Symbols)
    if (S.IsFunc && S.Addr >= Low && S.Addr < High)
      Sorted.push_back(&S);
  llvm::sort(Sorted, [](const Symbol *A, const Symbol *B) {
    return A->Addr < B->Addr;
  });
  llvm::errs() << "func symbols in [0x" << llvm::utohexstr(Low) << ",0x"
               << llvm::utohexstr(High) << ") = " << Sorted.size() << "\n";
  for (const Symbol *S : Sorted)
    llvm::errs() << "  sym 0x" << llvm::utohexstr(S->Addr) << " size=0x"
                 << llvm::utohexstr(S->Size) << " '" << S->Name << "'\n";
}

void dumpGoModule(const ExceptionInfo &EH) {
  if (!EH.GoModule)
    return;
  const GoModuleInfo &M = *EH.GoModule;
  llvm::errs() << "go-module version=" << M.PclnTabVersion << " magic=0x"
               << llvm::utohexstr(M.PclnTabMagic) << " pcheader=0x"
               << llvm::utohexstr(M.PcHeaderVA) << " moduledata=0x"
               << llvm::utohexstr(M.ModuleDataVA) << "\n"
               << "  text=0x" << llvm::utohexstr(M.TextBase) << " gofunc=0x"
               << llvm::utohexstr(M.GoFuncBase) << " funcnametab=0x"
               << llvm::utohexstr(M.FuncNameTabVA) << " pctab=0x"
               << llvm::utohexstr(M.PcTabVA) << " functab=0x"
               << llvm::utohexstr(M.FuncTabVA) << "\n"
               << "  funcs=" << M.FunctionCount
               << " minlc=" << unsigned(M.MinLC)
               << " ptrsize=" << unsigned(M.PtrSize)
               << " multitext=" << M.HasMultipleTextSections
               << " defer-layout="
               << getGoOpenCodedDeferLayoutName(M.OpenCodedDeferLayout)
               << "\n";
}

void dumpGoFunction(const ExceptionFunction &F) {
  const GoFunctionEH &G = *F.Go;
  llvm::errs() << "--- go 0x" << llvm::utohexstr(F.CodeRange.Begin) << "-0x"
               << llvm::utohexstr(F.CodeRange.End) << " '" << G.Name << "' "
               << getExceptionParseStatusName(F.ParseStatus) << "\n";
  llvm::errs() << "    funcid=" << unsigned(G.FuncID) << " flags=0x"
               << llvm::utohexstr(G.FuncFlags)
               << " framesize=" << (G.FrameSize ? *G.FrameSize : -1)
               << " deferreturn="
               << (G.DeferReturnOffset ? int64_t(*G.DeferReturnOffset) : -1)
               << " open-coded=" << G.UsesOpenCodedDefers << "\n";
  if (G.OpenCodedDeferInfo) {
    llvm::errs() << "    open-coded deferbits=-"
                 << G.OpenCodedDeferInfo->DeferBitsOffset << " slots=-"
                 << G.OpenCodedDeferInfo->SlotsOffset << " ("
                 << G.OpenCodedDefers.size() << " slots, "
                 << getGoOpenCodedDeferLayoutName(G.OpenCodedDeferInfo->Layout)
                 << (G.OpenCodedDeferInfo->SlotCountIsExact ? ", exact" : "")
                 << ")";
    for (const GoOpenCodedDefer &D : G.OpenCodedDefers)
      llvm::errs() << " " << D.ClosureOffset;
    llvm::errs() << "\n";
  }
  for (const GoDeferSite &D : G.Defers)
    llvm::errs() << "    defer@0x" << llvm::utohexstr(D.CallVA) << " "
                 << getGoDeferKindName(D.Kind) << "\n";
  for (const GoRecoverSite &Rc : G.Recovers)
    llvm::errs() << "    recover@0x" << llvm::utohexstr(Rc.CallVA)
                 << " deferred-frame=" << Rc.InDeferredFrame << "\n";
  for (const GoPanicSite &P : G.Panics)
    llvm::errs() << "    panic@0x" << llvm::utohexstr(P.CallVA) << " "
                 << P.RuntimeName << (P.IsImplicitCheck ? " (implicit)" : "")
                 << "\n";
  for (const std::string &D : F.Diagnostics)
    llvm::errs() << "    diag: " << D << "\n";
}

void dumpRustRuntime(const ExceptionInfo &EH) {
  if (!EH.RustRuntime)
    return;
  const RustRuntimeInfo &R = *EH.RustRuntime;
  llvm::errs() << "rust-runtime strategy="
               << getRustPanicStrategyName(R.Strategy)
               << " msvc=" << R.UsesMSVCUnwinding << " panic-type=0x"
               << llvm::utohexstr(R.PanicTypeDescriptorVA) << "\n"
               << "  cleanup=" << R.CleanupFrames
               << " catch_unwind=" << R.CatchUnwindFrames
               << " nounwind-guard=" << R.NoUnwindGuardFrames
               << " panic-sites=" << R.PanicSites << "\n";
}

void dumpRustFunction(const ExceptionFunction &F) {
  const RustFunctionEH &R = *F.Rust;
  llvm::errs() << "--- rust 0x" << llvm::utohexstr(F.CodeRange.Begin) << "-0x"
               << llvm::utohexstr(F.CodeRange.End)
               << " msvc=" << R.UsesMSVCTables << "\n";
  for (const RustLandingPad &Pad : R.LandingPads)
    llvm::errs() << "    pad@0x" << llvm::utohexstr(Pad.PadVA) << " "
                 << getRustLandingPadKindName(Pad.Kind) << " guards [0x"
                 << llvm::utohexstr(Pad.GuardedRange.Begin) << ",0x"
                 << llvm::utohexstr(Pad.GuardedRange.End) << ")\n";
  for (const RustPanicSite &P : R.Panics)
    llvm::errs() << "    panic@0x" << llvm::utohexstr(P.CallVA) << " "
                 << getRustPanicKindName(P.Kind) << " " << P.TargetName << "\n";
}

void dumpItanium(const ExceptionFunction &F) {
  const ItaniumEHInfo &I = *F.Itanium;
  llvm::errs() << "--- itanium 0x" << llvm::utohexstr(F.CodeRange.Begin)
               << "-0x" << llvm::utohexstr(F.CodeRange.End) << " personality='"
               << F.PersonalityName << "' ("
               << getExceptionPersonalityName(F.Personality) << ") "
               << getExceptionParseStatusName(F.ParseStatus) << "\n";
  llvm::errs() << "    lsda=0x" << llvm::utohexstr(I.LSDAVA) << " lpbase=0x"
               << llvm::utohexstr(I.LandingPadBase) << " cs-enc=0x"
               << llvm::utohexstr(I.CallSiteEncoding) << " tt-enc=0x"
               << llvm::utohexstr(I.TypeTableEncoding) << " tt=0x"
               << llvm::utohexstr(I.TypeTableVA)
               << " cleanup-only=" << I.isCleanupOnly() << "\n";
  for (const ItaniumCallSite &S : I.CallSites)
    llvm::errs() << "    site [0x" << llvm::utohexstr(S.GuardedRange.Begin)
                 << ",0x" << llvm::utohexstr(S.GuardedRange.End) << ") lp=0x"
                 << llvm::utohexstr(S.LandingPadVA) << " action="
                 << (S.FirstActionOffset ? int64_t(*S.FirstActionOffset) : -1)
                 << "\n";
  for (const ItaniumAction &A : I.Actions)
    llvm::errs() << "    action@" << A.TableOffset << " filter=" << A.TypeFilter
                 << " next="
                 << (A.NextActionOffset ? int64_t(*A.NextActionOffset) : -1)
                 << "\n";
  for (const ItaniumTypeEntry &T : I.TypeTable)
    llvm::errs() << "    type[" << T.Index << "] = 0x"
                 << llvm::utohexstr(T.TypeInfoVA) << " slot=0x"
                 << llvm::utohexstr(T.TypeInfoSlotVA) << " '" << T.TypeName
                 << "'" << (T.IsCatchAll ? " (catch-all)" : "") << "\n";
  for (const ItaniumExceptionSpec &S : I.ExceptionSpecs) {
    llvm::errs() << "    spec[" << S.Index << "] =";
    for (uint64_t Idx : S.TypeIndices)
      llvm::errs() << " " << Idx;
    llvm::errs() << "\n";
  }
  for (const std::string &D : F.Diagnostics)
    llvm::errs() << "    diag: " << D << "\n";
}

void dumpRegistration(const ExceptionFunction &F) {
  const RegistrationChainInfo &R = *F.Registration;
  llvm::errs() << "--- registration 0x" << llvm::utohexstr(F.CodeRange.Begin)
               << "-0x" << llvm::utohexstr(F.CodeRange.End) << " ["
               << getExceptionEncodingName(F.Encoding) << "] personality='"
               << F.PersonalityName << "' ("
               << getExceptionPersonalityName(F.Personality) << ") "
               << getExceptionParseStatusName(F.ParseStatus) << "\n";
  llvm::errs() << "    handler=0x" << llvm::utohexstr(R.HandlerVA)
               << " scopetable=0x" << llvm::utohexstr(R.ScopeTableVA)
               << " magic=0x" << llvm::utohexstr(R.ScopeTableMagic)
               << " install=0x" << llvm::utohexstr(R.ChainInstallVA)
               << " trylevel="
               << (R.TryLevelOffset ? int64_t(*R.TryLevelOffset) : 0x7fffffff)
               << " cookies=" << R.HasSecurityCookies << "\n";
  for (size_t I = 0; I < R.Scopes.size(); ++I)
    llvm::errs() << "    scope[" << I
                 << "] enclosing=" << R.Scopes[I].EnclosingLevel << " filter=0x"
                 << llvm::utohexstr(R.Scopes[I].FilterVA) << " handler=0x"
                 << llvm::utohexstr(R.Scopes[I].HandlerVA)
                 << (R.Scopes[I].IsFinally ? " (finally)" : "") << "\n";
  for (const RegistrationTryLevelStore &S : R.TryLevelStores)
    llvm::errs() << "    trylevel store 0x" << llvm::utohexstr(S.StoreVA)
                 << " := " << S.Level << "\n";
  if (F.Cxx) {
    llvm::errs() << "    cxx magic=0x" << llvm::utohexstr(F.Cxx->Magic)
                 << " maxstate=" << F.Cxx->MaxState
                 << " tryblocks=" << F.Cxx->TryBlocks.size()
                 << " valid-graph=" << F.Cxx->hasValidStateGraph() << "\n";
    for (const CxxUnwindAction &U : F.Cxx->UnwindMap)
      llvm::errs() << "      unwind to=" << U.ToState << " action=0x"
                   << llvm::utohexstr(U.ActionVA) << "\n";
    for (const CxxTryBlock &T : F.Cxx->TryBlocks) {
      llvm::errs() << "      try low=" << T.TryLow << " high=" << T.TryHigh
                   << " catchhigh=" << T.CatchHigh << "\n";
      for (const CxxCatchHandler &H : T.Handlers)
        llvm::errs() << "        catch adj=0x" << llvm::utohexstr(H.Adjectives)
                     << " type=0x" << llvm::utohexstr(H.TypeDescriptorVA)
                     << " handler=0x" << llvm::utohexstr(H.HandlerVA) << "\n";
    }
  }
  for (const std::string &D : F.Diagnostics)
    llvm::errs() << "    diag: " << D << "\n";
}

/// What a named symbol resolved to, and what the exception tables say about
/// the frame covering it.  A corpus contract is written against source-level
/// probe names, so "the manifest names X but nothing classified it" is the
/// question that comes up first, and answering it from the census alone means
/// guessing which of the printed frames X was supposed to be.
void dumpProbes(const BinaryImage &Img, llvm::StringRef Names) {
  const ExceptionInfo &EH = Img.ExceptionMetadata;
  while (!Names.empty()) {
    auto [Name, Rest] = Names.split(',');
    Names = Rest;
    if (Name.empty())
      continue;
    const std::string Underscored = ("_" + Name).str();
    va_t Addr = 0;
    for (llvm::StringRef Candidate : {Name, llvm::StringRef(Underscored)}) {
      if (const Symbol *S = Img.findSymbol(Candidate); S && S->Addr != 0)
        Addr = S->Addr;
      else if (const Export *E = Img.findExport(Candidate); E && E->Addr != 0)
        Addr = E->Addr;
      if (Addr != 0)
        break;
    }
    if (Addr == 0) {
      llvm::errs() << "probe '" << Name << "' = <no symbol>\n";
      continue;
    }
    const ExceptionFunction *F = EH.findFunction(Addr);
    llvm::errs() << "probe '" << Name << "' = 0x" << llvm::utohexstr(Addr);
    if (!F) {
      llvm::errs() << " <no covering frame>\n";
      continue;
    }
    llvm::errs() << " frame [0x" << llvm::utohexstr(F->CodeRange.Begin) << ",0x"
                 << llvm::utohexstr(F->CodeRange.End) << ") ["
                 << getExceptionEncodingName(F->Encoding) << "] "
                 << getExceptionParseStatusName(F->ParseStatus)
                 << " personality='" << F->PersonalityName << "' itanium="
                 << F->Itanium.has_value() << " rust=" << F->Rust.has_value();
    if (F->Rust)
      llvm::errs() << " pads=" << F->Rust->LandingPads.size()
                   << " panics=" << F->Rust->Panics.size();
    if (F->Itanium)
      llvm::errs() << " lsda=0x" << llvm::utohexstr(F->Itanium->LSDAVA)
                   << " sites=" << F->Itanium->CallSites.size();
    llvm::errs() << "\n";
  }
}

} // namespace

TEST(Scratch, DumpRealBinary) {
  const char *Path = std::getenv("NEVERD_SCRATCH_BINARY");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_SCRATCH_BINARY";
  auto L = Loader::create(std::filesystem::path(Path));
  ASSERT_NE(L, nullptr);
  auto ImgOr = L->load(std::filesystem::path(Path));
  ASSERT_TRUE(static_cast<bool>(ImgOr)) << toString(ImgOr.takeError());
  BinaryImage &Img = *ImgOr;

  if (wants("NEVERD_SCRATCH_SEGMENTS"))
    dumpSections(Img);
  if (wants("NEVERD_SCRATCH_SYMBOLS"))
    dumpFuncSymbols(Img, addrOr("NEVERD_SCRATCH_SYM_LOW", 0),
                    addrOr("NEVERD_SCRATCH_SYM_HIGH", InvalidVA));

  LanguageRuntimeInfo RT = detectLanguageRuntime(Img);
  llvm::errs() << "runtime=" << getSourceLanguageRuntimeName(RT.Runtime)
               << " mixed=" << RT.IsMixed << " version=" << RT.Version << "\n";
  for (const std::string &E : RT.Evidence)
    llvm::errs() << "  evidence: " << E << "\n";
  for (SourceLanguageRuntime S : RT.SecondaryRuntimes)
    llvm::errs() << "  secondary: " << getSourceLanguageRuntimeName(S) << "\n";

  const ExceptionInfo &EH = Img.ExceptionMetadata;
  llvm::errs() << "functions=" << EH.Functions.size()
               << " cies=" << EH.CIEs.size()
               << " status=" << getExceptionParseStatusName(EH.ParseStatus)
               << " models=";
  for (ExceptionModel M : EH.Models)
    llvm::errs() << getExceptionModelName(M) << " ";
  llvm::errs() << "\n";
  for (const DwarfCIE &C : EH.CIEs)
    llvm::errs() << "cie@0x" << llvm::utohexstr(C.SectionOffset) << " aug='"
                 << C.Augmentation << "' p-enc=0x"
                 << llvm::utohexstr(C.PersonalityEncoding) << " p=0x"
                 << llvm::utohexstr(C.PersonalityVA) << " p-slot=0x"
                 << llvm::utohexstr(C.PersonalitySlotVA) << " lsda-enc=0x"
                 << llvm::utohexstr(C.LSDAPointerEncoding) << " fde-enc=0x"
                 << llvm::utohexstr(C.FDEPointerEncoding) << " name='"
                 << resolveRoutineName(Img, C.PersonalityVA,
                                       C.PersonalitySlotVA)
                 << "'\n";
  if (wants("NEVERD_SCRATCH_CXX_TYPES")) {
    unsigned Shown = 0;
    for (const ExceptionFunction &F : EH.Functions) {
      bool AnyTyped = false;
      if (F.Cxx)
        for (const CxxTryBlock &T : F.Cxx->TryBlocks)
          for (const CxxCatchHandler &C : T.Handlers)
            AnyTyped |= C.TypeDescriptorVA != 0;
      if (!F.Cxx || !AnyTyped ||
          Shown >= limitOr("NEVERD_SCRATCH_LIMIT", 4))
        continue;
      ++Shown;
      llvm::errs() << "--- cxx 0x" << llvm::utohexstr(F.CodeRange.Begin)
                   << " tries=" << F.Cxx->TryBlocks.size()
                   << " unwind=" << F.Cxx->UnwindMap.size() << "\n";
      for (const CxxTryBlock &T : F.Cxx->TryBlocks)
        for (const CxxCatchHandler &C : T.Handlers)
          llvm::errs() << "    catch handler=0x"
                       << llvm::utohexstr(C.HandlerVA) << " typedesc=0x"
                       << llvm::utohexstr(C.TypeDescriptorVA) << "\n";
    }
  }

  if (wants("NEVERD_SCRATCH_MODULEDATA")) {
    if (const std::optional<GoModuleInfo> &M = EH.GoModule) {
      llvm::errs() << "moduledata va=0x" << llvm::utohexstr(M->ModuleDataVA)
                   << " text=0x" << llvm::utohexstr(M->TextBase) << " gofunc=0x"
                   << llvm::utohexstr(M->GoFuncBase) << "\n";
      const unsigned Words = limitOr("NEVERD_SCRATCH_LIMIT", 48);
      const size_t Size = Img.is64Bit() ? 8 : 4;
      for (unsigned I = 0; I < Words; ++I) {
        const va_t At = M->ModuleDataVA + I * Size;
        const uint8_t *P = Img.readVA(At, Size);
        if (!P)
          break;
        const uint64_t W = Img.is64Bit() ? readLE<uint64_t>(P)
                                         : uint64_t(readLE<uint32_t>(P));
        const Segment *S = Img.getSegmentFor(static_cast<va_t>(W));
        llvm::errs() << "  w[" << I << "] = 0x" << llvm::utohexstr(W) << " ("
                     << (S ? S->Name : std::string("-")) << ")\n";
      }
    } else {
      llvm::errs() << "moduledata: none\n";
    }
  }

  llvm::errs() << "relocations=" << Img.Relocations.size() << "\n";
  for (const DwarfCIE &C : EH.CIEs)
    for (const RelocationEntry &R : Img.Relocations)
      if (C.PersonalitySlotVA != 0 && R.Address == C.PersonalitySlotVA)
        llvm::errs() << "  reloc at p-slot 0x" << llvm::utohexstr(R.Address)
                     << " type=" << R.Type << " addend=0x"
                     << llvm::utohexstr(static_cast<uint64_t>(R.Addend))
                     << " explicit=" << R.HasExplicitAddend << " sym='"
                     << R.SymbolName << "'\n";
  for (const std::string &D : EH.Diagnostics)
    llvm::errs() << "  image-diag: " << D << "\n";

  if (const char *Probes = std::getenv("NEVERD_SCRATCH_PROBE"))
    dumpProbes(Img, Probes);

  dumpGoModule(EH);
  dumpRustRuntime(EH);

  // Per-encoding and per-status census: the shape of a whole image says more
  // about whether a decoder is right than any single record does.
  std::map<std::string, size_t> ByEncoding;
  std::map<std::string, size_t> ByPersonality;
  size_t Partial = 0, Malformed = 0, WithLSDA = 0, WithCxx = 0, WithSEH = 0,
         WithGo = 0, WithReg = 0, WithCompact = 0, WithDwarf = 0;
  for (const ExceptionFunction &F : EH.Functions) {
    ++ByEncoding[getExceptionEncodingName(F.Encoding)];
    if (F.Personality != ExceptionPersonality::None)
      ++ByPersonality[getExceptionPersonalityName(F.Personality)];
    Partial += F.ParseStatus == ExceptionParseStatus::Partial;
    Malformed += F.ParseStatus == ExceptionParseStatus::Malformed;
    WithLSDA += F.Itanium.has_value();
    WithCxx += F.Cxx.has_value();
    WithSEH += F.SEH.has_value();
    WithGo += F.Go.has_value();
    WithReg += F.Registration.has_value();
    WithCompact += F.Compact.has_value();
    WithDwarf += F.Dwarf.has_value();
  }
  llvm::errs() << "partial=" << Partial << " malformed=" << Malformed
               << " lsda=" << WithLSDA << " cxx=" << WithCxx
               << " seh=" << WithSEH << " go=" << WithGo << " reg=" << WithReg
               << " compact=" << WithCompact << " dwarf=" << WithDwarf << "\n";
  for (const auto &[Name, Count] : ByEncoding)
    llvm::errs() << "  encoding " << Name << " = " << Count << "\n";
  for (const auto &[Name, Count] : ByPersonality)
    llvm::errs() << "  personality " << Name << " = " << Count << "\n";

  if (wants("NEVERD_SCRATCH_RUST")) {
    unsigned RustShown = 0;
    const va_t Only = addrOr("NEVERD_SCRATCH_ADDR", 0);
    for (const ExceptionFunction &F : EH.Functions) {
      if (!F.Rust || (Only != 0 && !F.CodeRange.contains(Only)))
        continue;
      if (RustShown++ < limitOr("NEVERD_SCRATCH_LIMIT", 4)) {
        dumpRustFunction(F);
        if (F.Itanium)
          dumpItanium(F);
      }
    }
  }

  size_t Reported = 0;
  for (const ExceptionFunction &F : EH.Functions)
    if (F.ParseStatus != ExceptionParseStatus::Complete && Reported++ < 20) {
      llvm::errs() << "!!! " << getExceptionParseStatusName(F.ParseStatus)
                   << " 0x" << llvm::utohexstr(F.CodeRange.Begin) << " ["
                   << getExceptionEncodingName(F.Encoding)
                   << "] personality=0x" << llvm::utohexstr(F.PersonalityVA)
                   << " '" << F.PersonalityName << "' sym='"
                   << (Img.findSymbolAt(F.PersonalityVA)
                           ? Img.findSymbolAt(F.PersonalityVA)->Name
                           : std::string())
                   << "'\n";
      for (const std::string &D : F.Diagnostics)
        llvm::errs() << "      diag: " << D << "\n";
    }

  const unsigned Limit = limitOr("NEVERD_SCRATCH_LIMIT", 4);
  unsigned Shown = 0;
  for (const ExceptionFunction &F : EH.Functions) {
    if (Shown >= Limit)
      break;
    if (F.Itanium && !F.Itanium->TypeTable.empty()) {
      dumpItanium(F);
      ++Shown;
    } else if (F.Registration) {
      dumpRegistration(F);
      ++Shown;
    } else if (F.Go && (F.Go->UsesOpenCodedDefers || !F.Go->Recovers.empty())) {
      dumpGoFunction(F);
      ++Shown;
    }
  }
}
