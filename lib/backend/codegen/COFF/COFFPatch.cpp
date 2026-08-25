//===- COFFPatch.cpp - COFF/PE binary patching -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE binary patching implementation.  Handles both PE32 and PE32+
/// in a single unified code path — the Is64 flag in the parsed layout
/// drives pointer-size and optional-header differences.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFPatch.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFReloc.h"
#include "neverd/object/PELayout.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#define DEBUG_TYPE "neverd-coff-patch"

namespace neverd {

namespace {

constexpr llvm::StringLiteral
    SecurityCheckCookieName("__security_check_cookie");

bool hasCompilerOwnedGSContract(const llvm::Function &Function) {
  const llvm::Attribute GSWriter =
      Function.getFnAttribute(llvm::mc_rewrite::RewriteWinGSHandlerAttribute);
  return Function.hasFnAttribute(llvm::mc_rewrite::RewriteWinCxxFH4Attribute) &&
         GSWriter.isStringAttribute() &&
         GSWriter.getValueAsString() ==
             llvm::mc_rewrite::RewriteWinGSHandlerCxxFH4 &&
         Function.hasFnAttribute(llvm::Attribute::StackProtectReq);
}

bool hasExactSecurityCheckCookieABI(const llvm::Function &Function) {
  const llvm::FunctionType *Type = Function.getFunctionType();
  return Function.isDeclaration() && Function.hasExternalLinkage() &&
         Type->getReturnType()->isVoidTy() && !Type->isVarArg() &&
         Type->getNumParams() == 1 && Type->getParamType(0)->isPointerTy() &&
         Function.getCallingConv() == llvm::CallingConv::X86_FastCall &&
         Function.hasParamAttribute(0, llvm::Attribute::InReg);
}

llvm::Function *createSecurityCheckCookieDeclaration(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                       llvm::PointerType::getUnqual(Context),
                                       /*isVarArg=*/false);
  llvm::Function *Function =
      llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                             SecurityCheckCookieName, Module);
  Function->setCallingConv(llvm::CallingConv::X86_FastCall);
  Function->addParamAttr(0, llvm::Attribute::InReg);
  Function->setDSOLocal(true);
  return Function;
}

} // namespace

bool COFFPatcher::normalizeCompilerOwnedGSSecurityCheck(
    llvm::Module &Module, Arch TargetArch,
    const COFFExceptionPatchPlan &ExceptionPlan,
    const SourceFunctionPreparation &SourcePreparation, std::string &Detail) {
  Detail.clear();

  llvm::SmallPtrSet<const llvm::Function *, 4> GSFunctions;
  for (llvm::Function &Function : Module) {
    if (Function.isDeclaration() ||
        !Function.hasFnAttribute(
            llvm::mc_rewrite::RewriteWinGSHandlerAttribute))
      continue;
    if (TargetArch != Arch::X64 || !hasCompilerOwnedGSContract(Function)) {
      Detail = "a compiler-owned GS function has an invalid target contract";
      return false;
    }
    auto OriginalVA = rewrite_source::getOriginalVA(Function);
    if (!OriginalVA) {
      Detail = llvm::toString(OriginalVA.takeError());
      return false;
    }
    if (!*OriginalVA ||
        !llvm::is_contained(ExceptionPlan.LanguageExceptionFunctionEntries,
                            **OriginalVA)) {
      Detail = "a compiler-owned GS function is outside the validated "
               "exception plan";
      return false;
    }
    GSFunctions.insert(&Function);
  }
  if (GSFunctions.empty())
    return true;

  llvm::GlobalValue *NamedSecurityCheck =
      Module.getNamedValue(SecurityCheckCookieName);
  llvm::Function *SecurityCheck =
      llvm::dyn_cast_or_null<llvm::Function>(NamedSecurityCheck);
  if (NamedSecurityCheck && !SecurityCheck) {
    Detail = "the security-cookie runtime name has a non-function owner";
    return false;
  }
  if (!SecurityCheck) {
    createSecurityCheckCookieDeclaration(Module);
    return true;
  }
  if (!SecurityCheck->isDeclaration()) {
    Detail = "the preserved security-cookie runtime still has a body";
    return false;
  }

  auto OriginalVA = rewrite_source::getOriginalVA(*SecurityCheck);
  if (!OriginalVA) {
    Detail = llvm::toString(OriginalVA.takeError());
    return false;
  }
  if (*OriginalVA) {
    const auto Preserved = SourcePreparation.PreservedOriginalVAs.find(
        SecurityCheckCookieName.str());
    if (Preserved == SourcePreparation.PreservedOriginalVAs.end() ||
        Preserved->second != **OriginalVA) {
      Detail = "the lifted security-cookie runtime was not preserved";
      return false;
    }
  }

  llvm::SmallVector<llvm::CallInst *, 4> SourceFrameChecks;
  bool HasOtherUse = SecurityCheck->isUsedByMetadata();
  for (llvm::User *User : SecurityCheck->users()) {
    auto *Call = llvm::dyn_cast<llvm::CallInst>(User);
    if (!Call || Call->getCalledFunction() != SecurityCheck ||
        !GSFunctions.contains(Call->getFunction()) || !Call->use_empty()) {
      HasOtherUse = true;
      continue;
    }
    SourceFrameChecks.push_back(Call);
  }

  const bool ExactABI = hasExactSecurityCheckCookieABI(*SecurityCheck);
  if (HasOtherUse && !ExactABI) {
    Detail = "an incompatible security-cookie declaration has a non-GS use";
    return false;
  }

  // The lifted call checks the source machine frame.  The final-image backend
  // owns a different frame and StackProtectReq emits its fresh cookie check;
  // retaining the old call would both duplicate the check and preserve the
  // lifter's guessed runtime ABI.
  for (llvm::CallInst *Call : SourceFrameChecks)
    Call->eraseFromParent();

  if (ExactABI)
    return true;
  assert(SecurityCheck->use_empty() &&
         "all incompatible security-cookie uses were prevalidated");
  SecurityCheck->eraseFromParent();
  createSecurityCheckCookieDeclaration(Module);
  return true;
}

bool COFFPatcher::parseLayout(const std::vector<uint8_t> &Data,
                              PatchLayout &Layout) {
  auto PE = locatePEHeaders(const_cast<uint8_t *>(Data.data()), Data.size());
  if (!PE.valid()) {
    llvm::WithColor::error() << "coff_patch: not a valid PE file\n";
    return false;
  }

  Layout.PeOffset = PE.PeOffset;
  Layout.Is64 = PE.Is64;
  Layout.NumSections = PE.NumSections;
  Layout.OptionalHdrSize = PE.FileHeader->SizeOfOptionalHeader;
  Layout.SectionTableOff = static_cast<uint32_t>(PE.SectionTable - Data.data());

  Layout.ImageBase = getPEImageBase(PE);
  Layout.SectionAlignment = getPESectionAlignment(PE);
  Layout.FileAlignment = getPEFileAlignment(PE);
  Layout.SizeOfImage = getPESizeOfImage(PE);
  Layout.SizeOfHeaders = getPESizeOfHeaders(PE);
  Layout.EntryPointRva = getPEAddressOfEntryPoint(PE);

  // Locate the original code section so trampolines can be written over the
  // functions being replaced.  Resolution order:
  //   1. user-forced name ("--text-section .vmp0" for a packed/renamed PE),
  //   2. the canonical ".text",
  //   3. flag-based fallback: the executable section containing the entry
  //      point, else the largest executable section.
  // Step 3 mirrors BinaryImage::getTextSection() so section-mode patching keeps
  // working on a binary whose code section was renamed by a packer/protector
  // (VMProtect ".vmp0", UPX "UPX1", Themida, randomised names) even without an
  // explicit --text-section.  Without any match Layout.Text* stay 0 and
  // installTrampolines() would skip every function, silently producing a binary
  // with no redirection.
  PESectionFields TextSec;
  bool FoundText = false;
  if (!TextSectionOverride.empty())
    FoundText = findPESection(PE, TextSectionOverride, TextSec);
  if (!FoundText)
    FoundText = findPESection(PE, section_names::coff::Text, TextSec);
  if (!FoundText) {
    // A packer can zero a section's VirtualSize; the Windows loader then treats
    // the virtual extent as SizeOfRawData.  Use that effective size for both
    // the entry-containment test and the "largest" comparison, otherwise a
    // zeroed VirtualSize yields TextSize=0 and installTrampolines() silently
    // skips every function — the very failure this fallback exists to prevent.
    auto effSize = [](const PESectionFields &F) -> uint32_t {
      return F.VirtualSize ? F.VirtualSize : F.SizeOfRawData;
    };
    uint32_t EntryRVA = Layout.EntryPointRva;
    PESectionFields EntrySec, BiggestSec;
    bool HasEntry = false, HasBiggest = false;
    forEachPESection(PE, [&](const PESectionFields &F, uint16_t) {
      if (!(F.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
        return;
      if (EntryRVA != 0 && EntryRVA >= F.VirtualAddress &&
          EntryRVA < F.VirtualAddress + effSize(F)) {
        EntrySec = F;
        HasEntry = true;
      }
      if (!HasBiggest || effSize(F) > effSize(BiggestSec)) {
        BiggestSec = F;
        HasBiggest = true;
      }
    });
    if (HasEntry) {
      TextSec = EntrySec;
      FoundText = true;
    } else if (HasBiggest) {
      TextSec = BiggestSec;
      FoundText = true;
    }
  }
  if (FoundText) {
    Layout.TextVA = TextSec.VirtualAddress;
    Layout.TextSize =
        TextSec.VirtualSize ? TextSec.VirtualSize : TextSec.SizeOfRawData;
    Layout.TextFileOff = TextSec.PointerToRawData;
    Layout.TextFileSize = TextSec.SizeOfRawData;
  }

  COFFRelocResolver Resolver;
  if (Resolver.parse(Data, Arch::Unknown)) {
    for (const auto &E : Resolver.entries()) {
      uint64_t RVA = E.Addr - Layout.ImageBase;
      Layout.Imports.push_back({E.Name, RVA});
      Layout.IATMap[E.Name] = RVA;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "coff_patch: parsed " << Layout.NumSections
                          << " sections, " << Layout.Imports.size()
                          << " imports, .text RVA=0x"
                          << llvm::utohexstr(Layout.TextVA) << " size=0x"
                          << llvm::utohexstr(Layout.TextSize) << "\n");
  return true;
}

uint64_t COFFPatcher::plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                           Arch /*TargetArch*/) {
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;
  uint32_t NewSecRva = alignUp(Layout.SizeOfImage, Layout.SectionAlignment);
  return Layout.ImageBase + NewSecRva;
}

uint64_t COFFPatcher::appendExecSegment(std::vector<uint8_t> &Binary,
                                        llvm::ArrayRef<uint8_t> Code,
                                        llvm::StringRef SegName,
                                        Arch /*TargetArch*/) {
  using namespace llvm::object;
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;

  uint32_t NewSecRva = alignUp(Layout.SizeOfImage, Layout.SectionAlignment);
  uint64_t CodeVA = Layout.ImageBase + NewSecRva;

  uint64_t TextSize = Code.size();
  // FileAlignment/SectionAlignment are untrusted PE fields used here as alignUp
  // divisors: a zero value makes alignUp collapse to 0 (the resize(0) + memcpy
  // below would then write out of bounds), and a huge value can push the 32-bit
  // file offsets past 4 GiB and wrap the resize length.  Validate before use.
  if (Layout.FileAlignment == 0 || Layout.SectionAlignment == 0) {
    llvm::WithColor::error() << "coff_patch: invalid PE alignment\n";
    return 0;
  }
  uint64_t NewSecRawOff64 = alignUp(Binary.size(), Layout.FileAlignment);
  uint64_t NewSecRawSize64 = alignUp(TextSize, Layout.FileAlignment);
  if (NewSecRawOff64 + NewSecRawSize64 > 0xFFFFFFFFULL) {
    llvm::WithColor::error() << "coff_patch: section layout exceeds PE limits\n";
    return 0;
  }
  uint32_t NewSecRawOff = static_cast<uint32_t>(NewSecRawOff64);
  uint32_t NewSecRawSize = static_cast<uint32_t>(NewSecRawSize64);
  uint32_t NewSecVsize =
      alignUp(static_cast<uint32_t>(TextSize), Layout.SectionAlignment);

  Binary.resize(NewSecRawOff + NewSecRawSize, 0);
  std::memcpy(Binary.data() + NewSecRawOff, Code.data(), TextSize);

  uint32_t SecTableOff = Layout.SectionTableOff;
  uint32_t NewSecHdrOff =
      SecTableOff + Layout.NumSections * sizeof(coff_section);
  if (NewSecHdrOff + sizeof(coff_section) > NewSecRawOff) {
    llvm::WithColor::error() << "coff_patch: no room for new section header\n";
    return 0;
  }

  coff_section NewSec = {};
  // COFF short section names are at most 8 bytes.
  std::string Name = SegName.empty() ? kNdTextSection.str() : SegName.str();
  std::memcpy(NewSec.Name, Name.data(), std::min<size_t>(Name.size(), 8));
  NewSec.VirtualSize = static_cast<uint32_t>(TextSize);
  NewSec.VirtualAddress = NewSecRva;
  NewSec.SizeOfRawData = NewSecRawSize;
  NewSec.PointerToRawData = NewSecRawOff;
  NewSec.Characteristics = llvm::COFF::IMAGE_SCN_CNT_CODE |
                           llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                           llvm::COFF::IMAGE_SCN_MEM_READ;
  std::memcpy(Binary.data() + NewSecHdrOff, &NewSec, sizeof(coff_section));

  auto PE2 = locatePEHeaders(Binary.data(), Binary.size());
  if (PE2.valid()) {
    PE2.FileHeader->NumberOfSections =
        static_cast<uint16_t>(Layout.NumSections + 1);
    setPESizeOfImage(PE2, NewSecRva + NewSecVsize);
    clearPEChecksum(PE2);
    clearPEDataDirectory(PE2, llvm::COFF::CERTIFICATE_TABLE);
  }

  return CodeVA;
}

PatchResult COFFPatcher::patch(const std::filesystem::path &InputPath,
                               const std::filesystem::path &OutputPath,
                               llvm::Module &Mod, Arch TargetArch) {
  if (!archCOFFPatchSupported(TargetArch)) {
    llvm::WithColor::error()
        << "coff_patch: unsupported arch " << getArchName(TargetArch) << "\n";
    return PatchResult{};
  }

  return readPatchWrite(
      InputPath, OutputPath, /*SetExecPerm=*/false, "coff_patch",
      [&](std::vector<uint8_t> &Binary, PatchResult &Result) -> bool {
        PatchLayout Layout;
        if (!parseLayout(Binary, Layout))
          return false;
        if (TargetArch == Arch::ARM &&
            CachedMode != InstructionMode::Thumb) {
          llvm::WithColor::error()
              << "coff_patch: Windows ARM patching requires Thumb image "
                 "context\n";
          return false;
        }
        if (!CachedImage) {
          llvm::WithColor::error()
              << "coff_patch: parsed image context is required for safe "
                 "exception-table rewriting\n";
          return false;
        }

        auto CompileMod = llvm::CloneModule(Mod);
        SourceFunctionPreparation SourcePreparation;
        std::string SourceDetail;
        if (!prepareSourceFunctionsForPatch(*CompileMod, CachedImage,
                                            SourcePreparation, SourceDetail)) {
          llvm::WithColor::error()
              << "coff_patch: source identity preparation failed: "
              << SourceDetail << "\n";
          return false;
        }
        if (SourcePreparation.isExactNoOp()) {
          Result.Success = true;
          return true;
        }
        auto EHPlanOrErr =
            planCOFFExceptionPatch(*CompileMod, *CachedImage, TargetArch);
        if (!EHPlanOrErr) {
          llvm::WithColor::error()
              << llvm::toString(EHPlanOrErr.takeError()) << "\n";
          return false;
        }
        if (!normalizeCompilerOwnedGSSecurityCheck(
                *CompileMod, TargetArch, *EHPlanOrErr, SourcePreparation,
                SourceDetail)) {
          llvm::WithColor::error()
              << "coff_patch: GS runtime preparation failed: " << SourceDetail
              << "\n";
          return false;
        }

        uint64_t CodeVA = plannedExecSegmentVA(Binary, TargetArch);
        if (CodeVA == 0) {
          llvm::WithColor::error() << "coff_patch: cannot plan exec segment\n";
          return false;
        }

        InstructionMode ResolveMode = CachedMode;
        auto SerializeResolvedCode = [&](uint64_t VA, bool IsCode) {
          return IsCode ? serializeCodePointer(VA, TargetArch, ResolveMode)
                        : VA;
        };
        auto Resolve = [&](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
          std::string Name = Sym.str();
          const std::string PreservedKey = resolveSourceFunctionAlias(
              Name, SourcePreparation.PreservedOriginalVAs, BinaryFormat::COFF,
              TargetArch);
          if (!PreservedKey.empty())
            return SerializeResolvedCode(
                SourcePreparation.PreservedOriginalVAs.at(PreservedKey), true);
          if (CachedImage)
            if (auto Personality =
                    findCOFFExceptionPersonalityVA(*CachedImage, Sym))
              return SerializeResolvedCode(*Personality, true);
          std::string Key = resolveSymbolAlias(Name, Layout.IATMap);
          auto It = Layout.IATMap.find(Key);
          if (It != Layout.IATMap.end())
            return SerializeResolvedCode(Layout.ImageBase + It->second, false);
          if (CachedExports) {
            for (auto &E : *CachedExports)
              if (E.Name == Name)
                return CachedImage
                           ? serializeExportAddress(*CachedImage, E.Addr)
                           : E.Addr;
          }
          if (CachedSymbols) {
            for (const auto &S : *CachedSymbols)
              if (S.IsFunc && S.Name == Name)
                return SerializeResolvedCode(S.Addr, true);
          }
          if (auto VA = parseNdDataSymbol(Name)) {
            return CachedImage
                       ? serializeImageDataSymbolAddress(*CachedImage, *VA)
                       : *VA;
          }
          if (auto VA = parseNdCodePtrSymbol(Name))
            return SerializeResolvedCode(*VA, false);
          return std::nullopt;
        };

        for (llvm::Function &F : *CompileMod) {
          if (F.isDeclaration() ||
              !findCOFFExceptionPersonalityVA(*CachedImage, F.getName()))
            continue;
          // A personality routine is part of the preserved runtime, not an
          // ordinary lifted callee.  Re-emitting a locally defined copy would
          // make generated unwind data name the copy and would also assume the
          // lifter recovered its private runtime ABI.  Keep it external so the
          // address model binds every handler reference to the proven original
          // executable address.
          F.deleteBody();
          F.setLinkage(llvm::GlobalValue::ExternalLinkage);
          F.setDSOLocal(true);
        }

        CompiledImage Img =
            compileImageForPatch(*CompileMod, TargetArch, BinaryFormat::COFF,
                                 CodeVA, Resolve, Layout.ImageBase);
        if (!Img.Success || Img.Bytes.empty()) {
          llvm::WithColor::error()
              << "coff_patch: compileImageForPatch failed\n";
          return false;
        }

        if (!Img.Unresolved.empty()) {
          llvm::WithColor::warning() << "coff_patch: " << Img.Unresolved.size()
                                     << " unresolved symbols\n";
        }

        std::vector<va_t> PatchedOriginalEntries;
        std::vector<std::pair<va_t, va_t>> PatchedEntryMappings;
        const std::vector<Export> AuthenticatedExports =
            authenticatedFunctionExports(CachedImage);
        const std::vector<Export> *TrampolineExports =
            CachedImage ? &AuthenticatedExports : CachedExports;
        std::vector<PatchedFunctionEntry> PatchedFunctions;
        const bool HasExactSourcePlan = SourcePreparation.HasExactSources;
        if (HasExactSourcePlan &&
            !validateSourceFunctionPatchPlan(Img, CachedImage, SourceDetail)) {
          llvm::WithColor::error()
              << "coff_patch: source identity validation failed: "
              << SourceDetail << "\n";
          return false;
        }
        const SourceTrampolinePlan TrampolinePlan =
            HasExactSourcePlan ? makeSourceTrampolinePlan(Img, CachedImage)
                               : SourceTrampolinePlan{};
        if (HasExactSourcePlan && TrampolinePlan.PreservedCount != 0) {
          llvm::WithColor::error()
              << "coff_patch: an unsafe source definition escaped the "
                 "preservation prepass\n";
          return false;
        }

        size_t TrampCount = 0;
        if (HasExactSourcePlan) {
          if (!TrampolinePlan.OriginalVAs.empty())
            TrampCount = installTrampolines(
                Binary, Img.SymbolAddrs, Layout.TextVA, Layout.TextSize,
                Layout.TextFileOff, Layout.ImageBase, TargetArch, CachedMode,
                CachedSymbols, CachedCodeRanges, TrampolineExports,
                &PatchedOriginalEntries, &PatchedEntryMappings,
                &PatchedFunctions, TrampolinePlan.Owners,
                TrampolinePlan.OriginalVAs);
          if (!validatePatchedSourceTrampolineClosure(
                  TrampolinePlan, PatchedFunctions, TrampCount, SourceDetail)) {
            llvm::WithColor::error()
                << "coff_patch: source trampoline closure failed: "
                << SourceDetail << "\n";
            return false;
          }
        } else {
          TrampCount = installTrampolines(
              Binary, Img.SymbolAddrs, Layout.TextVA, Layout.TextSize,
              Layout.TextFileOff, Layout.ImageBase, TargetArch, CachedMode,
              CachedSymbols, CachedCodeRanges, TrampolineExports,
              &PatchedOriginalEntries, &PatchedEntryMappings);
        }

        auto EHUpdateOrErr = prepareCOFFExceptionDirectory(
            Binary, *CachedImage, Img, PatchedOriginalEntries,
            PatchedEntryMappings, CodeVA, TargetArch, CompileMod.get());
        if (!EHUpdateOrErr) {
          llvm::WithColor::error()
              << llvm::toString(EHUpdateOrErr.takeError()) << "\n";
          return false;
        }
        auto GuardUpdateOrErr = prepareCOFFGuardTables(
            Binary, *CachedImage, Img, PatchedEntryMappings, CodeVA, TargetArch,
            !EHPlanOrErr->LanguageExceptionFunctionEntries.empty());
        if (!GuardUpdateOrErr) {
          llvm::WithColor::error()
              << llvm::toString(GuardUpdateOrErr.takeError()) << "\n";
          return false;
        }

        uint64_t TextSize = Img.Bytes.size();
        uint64_t Placed =
            appendExecSegment(Binary, Img.Bytes, kNdTextSection, TargetArch);
        if (Placed == 0 || Placed != CodeVA) {
          llvm::WithColor::error()
              << "coff_patch: appended section VA does not match the "
                 "compiled address model\n";
          return false;
        }
        if (llvm::Error Err =
                applyCOFFExceptionDirectoryUpdate(Binary, *EHUpdateOrErr)) {
          llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
          return false;
        }
        if (llvm::Error Err = applyCOFFGuardTableUpdate(Binary, *CachedImage,
                                                        *GuardUpdateOrErr)) {
          llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
          return false;
        }
        if (llvm::Error Err = validatePatchedCOFFImage(
                Binary, TargetArch,
                EHUpdateOrErr->Apply && EHUpdateOrErr->Size != 0,
                *EHUpdateOrErr)) {
          llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
          return false;
        }

        Result.Success = true;
        Result.CodeSize = TextSize;
        Result.TrampolineCount = TrampCount;
        return true;
      });
}

} // namespace neverd
