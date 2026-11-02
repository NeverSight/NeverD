//===- SourcePatch.cpp - Explicit source replacement installation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/SourcePatch.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/PESignaturePolicy.h"
#include "neverd/backend/codegen/ELF/ELFSourceSymbols.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSigner.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/SourcePatchPreparation.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/MachO/DyldCacheLoad.h"
#include "neverd/loader/MachO/MachOARM32Mode.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {
namespace {

llvm::Error patchError(const llvm::Twine &Detail) {
  return llvm::make_error<llvm::StringError>("source patch: " + Detail,
                                             llvm::inconvertibleErrorCode());
}

bool hasAddressInitializer(const llvm::Constant &Value) {
  if (Value.isNullValue())
    return false;
  if (Value.getType()->isPointerTy())
    return true;
  for (const llvm::Use &Operand : Value.operands())
    if (const auto *Constant = llvm::dyn_cast<llvm::Constant>(Operand.get()))
      if (hasAddressInitializer(*Constant))
        return true;
  return false;
}

llvm::Error writeBytes(const std::filesystem::path &Path,
                       llvm::ArrayRef<uint8_t> Bytes) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path.string(), EC);
  if (EC)
    return patchError("cannot stage input: " + EC.message());
  OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  OS.close();
  if (OS.has_error())
    return patchError("cannot finish staging input");
  return llvm::Error::success();
}

llvm::Error validateTarget(const llvm::Module &Module,
                           const BinaryImage &Image) {
  if (auto Error = validateSourcePatchTarget(Image.Arch, Image.Format))
    return Error;
  auto Target = sourcePatchTargetTriple(Image);
  if (!Target)
    return Target.takeError();
  const llvm::Triple Expected(*Target);
  const llvm::Triple Actual(Module.getTargetTriple());
  if ((Actual.getArch() != Expected.getArch() &&
       !((Actual.isARM() || Actual.isThumb()) &&
         (Expected.isARM() || Expected.isThumb()))) ||
      Actual.getObjectFormat() != Expected.getObjectFormat() ||
      Actual.isOSWindows() != Expected.isOSWindows() ||
      Actual.isOSDarwin() != Expected.isOSDarwin() ||
      (Image.Arch == Arch::ARM &&
       (Actual.getEnvironment() != Expected.getEnvironment() ||
        Actual.isWatchABI() != Expected.isWatchABI())))
    return patchError("compiled target does not match the loaded binary");
  if (Module.getDataLayoutStr().empty() ||
      Module.getDataLayout().getPointerSize() != Image.getPointerSize() ||
      !Module.getDataLayout().isLittleEndian())
    return patchError("compiled data layout does not match the loaded binary");
  return validateReplacementIR(Module, Image.Arch);
}

llvm::Expected<va_t> legacyTarget(llvm::StringRef Name,
                                  const BinaryImage &Image) {
  std::set<va_t> Candidates;
  llvm::StringRef AutoName = Name;
  if (AutoName.consume_front(kAutoFuncPrefix)) {
    va_t Address = 0;
    if (!AutoName.empty() && !AutoName.getAsInteger(16, Address))
      Candidates.insert(Address);
  }
  auto Matches = [&](llvm::StringRef Original) {
    return Original == Name ||
           (Image.Format == BinaryFormat::MachO &&
            Original.consume_front("_") && Original == Name);
  };
  for (const Symbol &S : Image.Symbols)
    if (S.IsFunc && Matches(S.Name))
      Candidates.insert(S.Addr);
  for (const Export &E : Image.Exports)
    if (Matches(E.Name) && Image.hasAuthenticatedFunctionEntryAt(E.Addr))
      Candidates.insert(E.Addr);
  if (Candidates.size() > 1)
    return patchError("ambiguous original function for '" + Name + "'");
  return Candidates.empty() ? InvalidVA : *Candidates.begin();
}

} // namespace

llvm::Expected<std::string> sourcePatchTargetTriple(const BinaryImage &Image) {
  if (Image.MachOSharedCache)
    return patchError(macho_loader::cache_load::kSourceUnsupported);
  if (Image.isArm64e())
    return patchError(kArm64eSourceReplacementUnsupported);
  const char *Default = llvmCodegenTriple(Image.Arch, Image.Format);
  if (!Default)
    return patchError("unsupported native target");
  if (Image.Arch != Arch::ARM)
    return std::string(Default);
  if (Image.Format == BinaryFormat::ELF) {
    if (!Image.ELFMetadata)
      return patchError("ARM ELF requires loader-owned ABI flags");
    const uint32_t Flags = Image.ELFMetadata->Flags;
    const bool Hard = Flags & llvm::ELF::EF_ARM_ABI_FLOAT_HARD;
    const bool Soft = Flags & llvm::ELF::EF_ARM_ABI_FLOAT_SOFT;
    if (Hard && Soft)
      return patchError("conflicting ARM ELF floating-point ABI flags");
    if ((Flags & llvm::ELF::EF_ARM_EABIMASK) != llvm::ELF::EF_ARM_EABI_VER5)
      return patchError("ARM ELF source replacement requires EABI5");
    const bool Thumb =
        Image.instructionModeAt(Image.Entry) == InstructionMode::Thumb;
    return std::string(Thumb ? "thumbv7-unknown-linux-"
                             : "armv7-unknown-linux-") +
           (Hard ? "gnueabihf" : "gnueabi");
  }
  if (Image.Format == BinaryFormat::MachO) {
    auto ModeInfo = macho_arm32::parseModeInfo(Image.Raw);
    if (!ModeInfo)
      return ModeInfo.takeError();
    if (ModeInfo->CPUSubtype == llvm::MachO::CPU_SUBTYPE_ARM_V7K)
      return std::string("thumbv7k-apple-watchos");
    if (ModeInfo->CPUSubtype == llvm::MachO::CPU_SUBTYPE_ARM_V7 ||
        ModeInfo->CPUSubtype == llvm::MachO::CPU_SUBTYPE_ARM_V7S)
      return std::string("thumbv7-apple-ios5.0");
    return patchError("ARM Mach-O source ABI requires ARMv7, ARMv7s or ARMv7k");
  }
  return std::string(Default);
}

llvm::Error validateSourcePatchTarget(Arch Architecture, BinaryFormat Format) {
  if (!llvmCodegenTriple(Architecture, Format))
    return patchError("unsupported native target");
  return llvm::Error::success();
}

llvm::Expected<std::vector<SourcePatchMapping>>
bindSourcePatch(llvm::Module &Module, const BinaryImage &Image,
                llvm::ArrayRef<SourcePatchMapping> Requested) {
  if (auto Error = validateTarget(Module, Image))
    return std::move(Error);

  std::vector<SourcePatchMapping> Mappings(Requested.begin(), Requested.end());
  if (Mappings.empty()) {
    for (llvm::Function &Function : Module) {
      if (Function.isDeclaration() || Function.hasLocalLinkage())
        continue;
      auto Address = legacyTarget(Function.getName(), Image);
      if (!Address)
        return Address.takeError();
      if (*Address != InvalidVA)
        Mappings.push_back({Function.getName().str(), *Address});
    }
  }
  if (Mappings.empty())
    return patchError("no replacement targets; specify an entry and address");

  std::set<va_t> Addresses;
  std::set<std::string> Names;
  for (SourcePatchMapping &Mapping : Mappings) {
    if (Mapping.Symbol.empty()) {
      for (llvm::Function &Function : Module) {
        if (Function.isDeclaration() || Function.hasLocalLinkage())
          continue;
        if (!Mapping.Symbol.empty())
          return patchError(
              "multiple replacement definitions; select an entry");
        Mapping.Symbol = Function.getName().str();
      }
    }
    const llvm::Function *Function = Module.getFunction(Mapping.Symbol);
    if (!Function || Function->isDeclaration())
      return patchError("replacement definition not found: " + Mapping.Symbol);
    if (!Names.insert(Mapping.Symbol).second ||
        !Addresses.insert(Mapping.OriginalVA).second)
      return patchError("duplicate replacement symbol or target address");
    const auto Mode = Image.instructionModeAt(Mapping.OriginalVA);
    if (!Mode ||
        normalizeCodeAddress(Mapping.OriginalVA, Image.Arch, *Mode) !=
            Mapping.OriginalVA ||
        !Image.hasAuthenticatedFunctionEntryAt(Mapping.OriginalVA))
      return patchError("target is not an authenticated function entry: " +
                        Mapping.Symbol);
  }

  // Unselected helpers cannot smuggle additional replacement targets through
  // incoming IR metadata. No metadata is changed until all bindings validate.
  for (llvm::Function &Function : Module) {
    auto Existing = rewrite_source::getOriginalVA(Function);
    if (!Existing)
      return Existing.takeError();
    if (!*Existing)
      continue;
    const auto It = std::find_if(Mappings.begin(), Mappings.end(),
                                 [&](const SourcePatchMapping &Mapping) {
                                   return Mapping.Symbol == Function.getName();
                                 });
    if (It == Mappings.end() || It->OriginalVA != **Existing)
      return patchError("incoming source identity conflicts with request");
  }
  if (Image.Format == BinaryFormat::MachO && Image.Arch == Arch::ARM) {
    auto Target = sourcePatchTargetTriple(Image);
    if (!Target)
      return Target.takeError();
    if (llvm::Triple(*Target).isWatchABI())
      for (auto &F : Module)
        if (!F.isDeclaration())
          F.addFnAttr("frame-pointer", "all");
  }
  for (const SourcePatchMapping &Mapping : Mappings)
    rewrite_source::setOriginalVA(*Module.getFunction(Mapping.Symbol),
                                  Mapping.OriginalVA);
  return Mappings;
}

llvm::Error validateInstalledSourcePatchSection(const BinaryImage &Final,
                                                const CompiledImage &Image,
                                                const CompiledSection &S) {
  if (!S.IsAllocated || !S.Size)
    return llvm::Error::success();
  auto Fail = [&] {
    return llvm::createStringError(
        llvm::errc::not_supported,
        "source patch: final section bytes, extent, or permissions changed: " +
            S.Name);
  };
  const auto *Owner = Final.getSegmentFor(S.VA);
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  const bool Write =
      S.Kind == Kind::WritableData || S.Kind == Kind::UninitializedData;
  if (!Owner || !Owner->isReadable() ||
      !rangeInBounds(S.VA - Owner->VA, S.Size, Owner->Size) ||
      Owner->isWritable() != Write || (Write && Owner->isExecutable()) ||
      (S.Kind == Kind::Code && !Owner->isExecutable()))
    return Fail();
  if (S.IsInImage ? !rangeInBounds(S.Offset, S.Size, Image.Bytes.size())
                  : S.ExternalBytes.size() != S.Size)
    return Fail();
  const uint8_t *Expected =
      S.IsInImage ? Image.Bytes.data() + S.Offset : S.ExternalBytes.data();
  const auto *Actual = Final.readVA(S.VA, S.Size);
  if (!Actual || !std::equal(Expected, Expected + S.Size, Actual))
    return Fail();
  return llvm::Error::success();
}

llvm::Expected<PatchResult> publishSourcePatch(
    const BinaryImage &Image, llvm::ArrayRef<SourcePatchMapping> Mappings,
    const std::filesystem::path &OutputPath,
    llvm::function_ref<llvm::Expected<PatchResult>(
        const std::filesystem::path &, const std::filesystem::path &)>
        Install,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const SourcePatchPreparation *Preparation,
    llvm::function_ref<llvm::Error(const BinaryImage &)> VerifyCandidate) {
  if (Mappings.empty() || Image.Raw.empty())
    return patchError(
        "bound targets and a complete input snapshot are required");
  if (OutputPath.empty())
    return patchError("output path is empty");
  std::error_code EC;
  auto Status = std::filesystem::symlink_status(OutputPath, EC);
  if (EC && EC != std::errc::no_such_file_or_directory)
    return patchError("cannot inspect output: " + EC.message());
  if (Status.type() != std::filesystem::file_type::not_found)
    return patchError("output already exists; choose a new path");

  if (auto Error = validateELFSourceFeatures(Image))
    return std::move(Error);
  std::optional<macho_signature::Profile> Signature;
  if (Image.Format == BinaryFormat::MachO) {
    auto Before = macho_signature::inspect(Image.Raw);
    if (!Before)
      return Before.takeError();
    const bool Unsigned = std::all_of(
        Before->Slices.begin(), Before->Slices.end(), [](const auto &Slice) {
          return Slice.SignatureKind == macho_signature::Kind::Unsigned;
        });
    if (!Unsigned && !macho_signature::canTransactionallyAdHocResign(*Before))
      return patchError(
          "Mach-O signature requires an explicit signing workflow");
    if (!Unsigned)
      Signature = std::move(*Before);
  } else if (Image.Format == BinaryFormat::COFF) {
    auto Before = pe_signature::inspect(Image.Raw);
    if (!Before)
      return Before.takeError();
    if (Before->SignatureKind != pe_signature::Kind::Unsigned)
      return patchError("signed PE requires an explicit signing workflow");
  }

  const auto AbsoluteOutput = std::filesystem::absolute(OutputPath, EC);
  if (EC)
    return patchError("cannot resolve output path: " + EC.message());
  const auto Parent = AbsoluteOutput.parent_path();
  llvm::SmallString<256> Directory;
  if (auto Error = llvm::sys::fs::createUniqueDirectory(
          (Parent / ".neverd-source-patch").string(), Directory))
    return patchError("cannot create staging directory: " + Error.message());
  const std::filesystem::path TempDir(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] {
    std::error_code Ignored;
    std::filesystem::remove_all(TempDir, Ignored);
  });
  const auto Input = TempDir / "input";
  const auto Candidate = TempDir / "candidate";
  if (Preparation)
    if (auto Error = verifySourcePatchPreparation(Image, *Preparation,
                                                  Preparation->Image))
      return Error;
  if (auto Error =
          writeBytes(Input, Preparation ? Preparation->Image.Raw : Image.Raw))
    return std::move(Error);
  auto Installed = Install(Input, Candidate);
  if (!Installed)
    return Installed.takeError();
  PatchResult Result = std::move(*Installed);
  if (!Result.Success)
    return patchError(
        "format installation failed; candidate was not published");
  if (Result.PatchedFunctions.size() != Mappings.size() ||
      Result.TrampolineCount != Mappings.size())
    return patchError("not every requested replacement was installed");
  for (const SourcePatchMapping &Mapping : Mappings) {
    const auto Count = std::count_if(
        Result.PatchedFunctions.begin(), Result.PatchedFunctions.end(),
        [&](const PatchedFunctionEntry &Installed) {
          return Installed.OriginalVA == Mapping.OriginalVA &&
                 Installed.SourceFunction == Mapping.Symbol;
        });
    if (Count != 1)
      return patchError("missing exact installation receipt: " +
                        Mapping.Symbol);
  }

  if (Image.isMachO()) {
    std::vector<uint8_t> Raw;
    {
      auto File = llvm::MemoryBuffer::getFile(Candidate.string());
      if (!File)
        return patchError("cannot read native segment candidate");
      const auto Buffer = (*File)->getBuffer();
      Raw.assign(Buffer.bytes_begin(), Buffer.bytes_end());
    }
    // Release any file mapping before truncating the candidate on Windows.
    if (auto E = completeMachOChainedSegmentTable(Raw, Image.Arch))
      return E;
    if (auto E = writeBytes(Candidate, Raw))
      return E;
  }

  if (Signature) {
    if (auto Error = macho_signing::adHocResign(
            Candidate.string(), Signature->Slices.front().Identifier))
      return std::move(Error);
    if (auto Error = macho_signing::verifyStrict(Candidate.string()))
      return std::move(Error);
  }
  auto Reader = Loader::create(Image.Format);
  auto Reloaded = Reader->load(Candidate);
  if (!Reloaded)
    return Reloaded.takeError();
  if (Reloaded->Arch != Image.Arch || Reloaded->Format != Image.Format)
    return patchError("candidate target changed during installation");
  if (Signature) {
    auto After = macho_signature::inspect(Reloaded->Raw);
    if (!After)
      return After.takeError();
    if (auto Error = macho_signature::validateTransactionallyAdHocResigned(
            *Signature, *After))
      return std::move(Error);
  }
  for (const PatchedFunctionEntry &Installed : Result.PatchedFunctions) {
    const auto Mode = Image.instructionModeAt(Installed.OriginalVA);
    const auto Target = getTargetCodegenInfo(Image.Arch, *Mode);
    const uint64_t Size = Target.sourceTrampolineSize();
    const uint64_t Offset = Reloaded->vaToFileOffset(Installed.OriginalVA);
    std::vector<uint8_t> Expected(Size);
    const auto *OwnerSegment = Reloaded->getSegmentFor(Installed.OwnerVA);
    const auto *OwnerFrame =
        Reloaded->ExceptionMetadata.findFunction(Installed.OwnerVA);
    const bool UnsectionedMachOCode =
        Reloaded->isMachO() && !Reloaded->getSectionFor(Installed.OwnerVA) &&
        OwnerSegment && OwnerSegment->isExecutable() &&
        !OwnerSegment->isWritable() && OwnerFrame &&
        OwnerFrame->CodeRange.Begin == Installed.OwnerVA &&
        OwnerFrame->ParseStatus == ExceptionParseStatus::Complete;
    if (!Reloaded->isCodeAddress(Installed.OwnerVA) && !UnsectionedMachOCode)
      return patchError("installed owner is not executable in candidate: 0x" +
                        llvm::utohexstr(Installed.OwnerVA));
    if (!rangeInBounds(Offset, Size, Reloaded->Raw.size()))
      return patchError("candidate entry is outside file-backed storage");
    if (!Target.writeSourceTrampoline(Expected, 0, Installed.OwnerVA,
                                      Installed.OriginalVA, Size))
      return patchError("installed entry has an out-of-range destination");
    if (!std::equal(Expected.begin(), Expected.end(),
                    Reloaded->Raw.begin() + Offset))
      return patchError(
          "candidate entry does not match its installation receipt");
  }

  if (Preparation) {
    if (auto Error =
            verifySourcePatchPreparation(Image, *Preparation, *Reloaded))
      return Error;
    for (const auto &Callback : Preparation->Callbacks)
      Result.OriginalCallbacks.push_back(
          {Callback.Symbol, Callback.Prefix.OriginalVA,
           Callback.Prefix.CallbackVA, Callback.Prefix.CopiedSize});
  }

  if (VerifyCandidate)
    if (auto Error = VerifyCandidate(*Reloaded))
      return Error;

  // Same-filesystem hard-link publication is atomic and refuses an existing
  // destination, including a dangling symlink. Cleanup unlinks only staging.
  Result.OutputPath = OutputPath.string();
  if (PrepareReceipt)
    if (auto Error = PrepareReceipt(Result))
      return std::move(Error);
  if (auto Error = llvm::sys::fs::create_hard_link(Candidate.string(),
                                                   OutputPath.string()))
    return patchError("cannot publish output exclusively: " + Error.message());
  return Result;
}

llvm::Expected<PatchResult> installSourcePatch(
    llvm::Module &Module, const BinaryImage &Image,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    const std::filesystem::path &OutputPath, llvm::StringRef TextSection,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const SourcePatchPreparation *Preparation) {
  // Each admitted format must install persistent state in writable storage.
  // Address initializers additionally need load-time relocation registration;
  // until that contract is installed, never freeze an ASLR-sensitive pointer.
  for (const llvm::GlobalVariable &Global : Module.globals()) {
    if (Global.isThreadLocal())
      return patchError("TLS storage is not supported by this source profile");
    if ((Global.getName() == "llvm.global_ctors" ||
         Global.getName() == "llvm.global_dtors")) {
      if (Global.hasInitializer() && !Global.getInitializer()->isNullValue())
        return patchError(
            "global initialization requires runtime registration");
      continue;
    }
    if (!Global.hasInitializer() || Global.getName() == "llvm.used" ||
        Global.getName() == "llvm.compiler.used")
      continue;
    const bool PESource = Image.Format == BinaryFormat::COFF;
    if (!Global.isConstant() && Image.Format != BinaryFormat::ELF &&
        Image.Format != BinaryFormat::MachO && !PESource)
      return patchError(
          "writable patch storage is not installed for this format: " +
          Global.getName());
    if (hasAddressInitializer(*Global.getInitializer()) &&
        Image.Format != BinaryFormat::ELF && !PESource)
      return patchError(
          "address initializer requires load-time relocation registration: " +
          Global.getName());
  }

  if (Image.isMachO() && Preparation && !Preparation->Imports.empty()) {
    if (auto E = validateTarget(Module, Image))
      return E;
    Codegen Generator;
    // Native indirect-symbol directives must survive emission. MC rewrite's
    // raw section stream is insufficient for Mach-O's non-lazy pointer tables.
    auto Object = Generator.compile(Module, Image.Arch, Image.Format,
                                    Module.getTargetTriple().str());
    if (!Object.Success || Object.ObjectData.empty())
      return patchError("native dependency object emission failed");
    return installObjectPatch(llvm::StringRef(reinterpret_cast<const char *>(
                                                  Object.ObjectData.data()),
                                              Object.ObjectData.size()),
                              Image, Mappings, Preparation->Bindings,
                              OutputPath, PrepareReceipt, nullptr, Preparation);
  }
  return publishSourcePatch(
      Image, Mappings, OutputPath,
      [&](const std::filesystem::path &Input,
          const std::filesystem::path &Candidate)
          -> llvm::Expected<PatchResult> {
        auto Patcher = BinaryPatcher::create(Image.Format);
        if (!Patcher)
          return patchError("unsupported binary format");
        BinaryImage ReferenceImage = Preparation ? Preparation->Image : Image;
        if (ReferenceImage.isELF() ||
            ReferenceImage.Format == BinaryFormat::COFF)
          scanImportThunks(ReferenceImage);
        Patcher->setImageContext(&ReferenceImage);
        Patcher->setInputKind(RewriteInputKind::ReplacementIR);
        Patcher->setTextSectionOverride(TextSection);
        PatchResult Result =
            Patcher->patch(Input, Candidate, Module, Image.Arch);
        return Result;
      },
      PrepareReceipt, Preparation);
}

} // namespace neverd
