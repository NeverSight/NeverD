//===- COFFRegistrationPatch.cpp - Checked PE32 EH installation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"

#include "../X86/CompiledSEHStateWrites.h"
#include "COFFRegistrationCxxIRProof.h"
#include "COFFRegistrationIRProof.h"
#include "COFFRegistrationSEHStateWrites.h"
#include "COFFRegistrationTableProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"

#include <map>
#include <set>

namespace neverd {
namespace {

llvm::Error reject(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "coff registration patch: " + Detail);
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
llvm::Expected<size_t> rawOffset(const PEHeaderPtrs &PE,
                                 llvm::ArrayRef<uint8_t> Bytes, uint32_t RVA,
                                 uint32_t Size) {
  std::vector<coff_loader::detail::RawBackedSectionRange> Sections;
  forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
    Sections.push_back({S.VirtualAddress,
                        getPESectionContentSize(S.VirtualSize, S.SizeOfRawData),
                        S.PointerToRawData, S.SizeOfRawData});
  });
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      Sections, Bytes.size(), RVA, Size);
  if (!Offset)
    return Offset.takeError();
  return static_cast<size_t>(*Offset);
}

using coff_registration::absolutePointer;
using coff_registration::exactPointerFixup;
using coff_registration::ownerVA;
using coff_registration::sectionAt;

using coff_registration::validateSymbolicImagePointers;

std::optional<va_t> retiredSharedEntryPointer(const BinaryImage &Image,
                                              const ExceptionFunction &Source,
                                              va_t Entry, va_t Field) {
  if (Entry > UINT32_MAX - 7 || Field != Entry + 3)
    return std::nullopt;
  if (Source.Cxx) {
    const auto Call = coff_loader::getCheckedX86CxxPrologueCall(Image, Source);
    if (Call && Call->Caller.Begin == Entry && Call->CallVA == Entry + 7)
      return Call->HandlerVA;
  } else {
    const auto Call = coff_loader::getCheckedX86SEH4PrologueCall(Image, Source);
    if (Call && Call->Caller.Begin == Entry && Call->CallVA == Entry + 7)
      return Call->ScopeTableVA;
  }
  return std::nullopt;
}
#endif
} // namespace

std::optional<va_t> findCOFFRegistrationRuntimeVA(const BinaryImage &Image,
                                                  llvm::StringRef Symbol) {
  const bool CookieSymbol = Symbol == "___security_cookie";
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      Image.Base > UINT32_MAX ||
      (!CookieSymbol && Symbol != "@__security_check_cookie@4") ||
      !Image.DynInfo.SecurityCookieRVA)
    return std::nullopt;
  const va_t Cookie = Image.Base + Image.DynInfo.SecurityCookieRVA;
  if (Cookie > UINT32_MAX - 3 || !Image.readVA(Cookie, 4))
    return std::nullopt;
  std::optional<va_t> Result;
  for (const auto &EH : Image.ExceptionMetadata.Functions)
    if (EH.Personality == ExceptionPersonality::ExceptHandler4 &&
        EH.ParseStatus == ExceptionParseStatus::Complete && EH.Registration)
      if (auto Check = coff_loader::getCheckedX86EH4CookieCheck(
              Image, EH.PersonalityVA)) {
        if (!CookieSymbol &&
            !coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, *Check))
          return std::nullopt;
        const va_t Address = CookieSymbol ? Cookie : *Check;
        if (Result && Result != Address)
          return std::nullopt;
        Result = Address;
      }
  return Result;
}

llvm::Expected<COFFRegistrationPatchUpdate> prepareCOFFRegistrationPatch(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, const llvm::Module &RewriteModule,
    const COFFGuardTableUpdate *GuardUpdate) {
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  COFFRegistrationPatchUpdate Update;
  std::map<va_t, va_t> Mappings;
  std::map<va_t, const ExceptionFunction *> Sources;
  for (const auto &[Original, Generated] : PatchedEntryMappings) {
    if (Original < Image.Base || Original > uint64_t(UINT32_MAX) - 4 ||
        !Image.isCodeAddress(Original) || Generated < NewSectionVA ||
        Generated > UINT32_MAX)
      return reject("patched entry mapping lies outside its executable image");
    if (!Mappings.emplace(Original, Generated).second)
      return reject("patched entry mapping is duplicated");
    for (const auto &EH : Image.ExceptionMetadata.Functions)
      if (EH.CodeRange.Begin == Original) {
        if (!EH.Registration || !Sources.emplace(Original, &EH).second)
          return reject(
              "patched x86 exception function has no unique registration");
      }
  }
  if (Sources.empty())
    return Update;
  auto PE = locatePEHeaders(const_cast<uint8_t *>(OriginalBinary.data()),
                            OriginalBinary.size());
  if (!PE.valid() || PE.Is64 ||
      PE.FileHeader->Machine != llvm::COFF::IMAGE_FILE_MACHINE_I386 ||
      getPEImageBase(PE) != Image.Base || Image.Arch != Arch::X86 ||
      Image.Format != BinaryFormat::COFF || Compiled.BaseVA != NewSectionVA ||
      NewSectionVA < Image.Base || NewSectionVA > UINT32_MAX ||
      Compiled.Bytes.size() > UINT32_MAX ||
      Compiled.Bytes.size() > uint64_t(UINT32_MAX) + 1 - NewSectionVA ||
      !Compiled.Success || !Compiled.Unresolved.empty())
    return reject("generated x86 image does not match its PE32 placement");
  const auto *Optional = getPE32OptionalHeader(PE);
  Update.ImageBase = Image.Base;
  Update.DllCharacteristics = Optional->DLLCharacteristics;
  Update.FileCharacteristics = PE.FileHeader->Characteristics;
  if (Update.DllCharacteristics & llvm::COFF::IMAGE_DLL_CHARACTERISTICS_NO_SEH)
    return reject(
        "input forbids SEH while containing a live registration contract");
  const bool Fixed =
      Update.FileCharacteristics & llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED;
  if (Fixed && (Update.DllCharacteristics &
                llvm::COFF::IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE))
    return reject("input combines stripped relocations with dynamic base");
  if (!Fixed)
    if (llvm::Error Error = validateSymbolicImagePointers(
            RewriteModule, Image.Base, getPESizeOfImage(PE)))
      return std::move(Error);
  std::set<uint32_t> RequiredHandlers;
  std::set<uint32_t> OriginalHandlers;
  std::set<uint32_t> GeneratedHandlers;
  std::set<va_t> RequiredAbsoluteFields;
  std::set<std::string> SourceNames;
  for (const auto &[Original, EH] : Sources) {
    const llvm::Function *Function = nullptr;
    for (const auto &[Name, VA] : Compiled.SourceFunctionOriginalVAs)
      if (VA == Original) {
        if (Function || !RewriteModule.getFunction(Name))
          return reject("source entry has no unique generated IR definition");
        Function = RewriteModule.getFunction(Name);
      }
    if (!Function || Function->isDeclaration())
      return reject("patched registration source has no generated definition");
    auto Owner = ownerVA(*Function, Compiled);
    if (!Owner)
      return Owner.takeError();
    if (*Owner != Mappings.at(Original))
      return reject(
          "entry trampoline names a different compiler function owner");
    if (EH->Cxx) {
      if (llvm::Error Error =
              validateCOFFRegistrationCxxIR(*Function, *EH, Image))
        return std::move(Error);
      auto Proof =
          coff_registration::getCheckedCxxControlIRProof(*Function, *EH, Image);
      if (!Proof)
        return Proof.takeError();
      size_t CalleeWork = 0;
      for (const auto &[Call, Checked] : Proof->Calls) {
        if (Checked.Contract.CodeRanges.empty())
          return reject("preserved C++ callee has no complete code extent");
        for (const auto &[Entry, Generated] : Mappings)
          for (const auto &Range : Checked.Contract.CodeRanges) {
            if (++CalleeWork > limits::kMaxRegistrationEHStateWork)
              return reject("C++ preserved code proof exceeds its work budget");
            if (!Range.isValid() || Range.End > uint64_t(UINT32_MAX) + 1 ||
                ExceptionAddressRange{Entry, Entry + 5}.overlaps(Range))
              return reject("entry trampoline overwrites preserved C++ callee "
                            "code without a native replacement contract");
          }
      }
      auto Runtime =
          coff_registration::getCheckedCxxRuntimeIR(*Function, *EH, Image);
      if (!Runtime)
        return Runtime.takeError();
      for (const auto &[Entry, Generated] : Mappings)
        for (const auto &Range : Runtime->CodeRanges)
          if (ExceptionAddressRange{Entry, Entry + 5}.overlaps(Range))
            return reject("entry trampoline overwrites preserved C++ CRT "
                          "dispatch code");
      auto Receipt = getCheckedCOFFRegistrationCxxHandlerReceipt(
          *Function, *EH, Image, Compiled);
      if (!Receipt)
        return Receipt.takeError();
      Update.GeneratedCxxGraphs.push_back(Receipt->Tables.GeneratedCxxGraph);
      if (Receipt->CodeRange.Begin < NewSectionVA ||
          Receipt->CodeRange.Begin < Image.Base ||
          Receipt->CodeRange.Begin > UINT32_MAX)
        return reject(
            "generated C++ registration handler is outside its image");
      const uint32_t RVA = Receipt->CodeRange.Begin - Image.Base;
      GeneratedHandlers.insert(RVA);
      RequiredHandlers.insert(RVA);
      RequiredAbsoluteFields.insert(Receipt->AbsolutePointerFields.begin(),
                                    Receipt->AbsolutePointerFields.end());
    } else {
      if (llvm::Error Error = validateCOFFRegistrationIR(*Function, *EH, Image))
        return std::move(Error);
      if (llvm::Error Error =
              validateCOFFRegistrationSemanticRows(*Function, *EH, Compiled))
        return std::move(Error);
      if (llvm::Error Error =
              validateCOFFRegistrationSEHStateWrites(*Function, *EH, Compiled))
        return std::move(Error);
      RequiredHandlers.insert(uint32_t(EH->PersonalityVA - Image.Base));
    }
    SourceNames.insert(Function->getName().str());
    if (EH->PersonalityVA < Image.Base || EH->PersonalityVA > UINT32_MAX ||
        !Image.isCodeAddress(EH->PersonalityVA) ||
        !Image.readVA(EH->PersonalityVA, 1))
      return reject(
          "source registration personality is not executable image code");
    OriginalHandlers.insert(uint32_t(EH->PersonalityVA - Image.Base));
    if (EH->Personality == ExceptionPersonality::ExceptHandler4) {
      const uint64_t Cookie = Image.Base + Image.DynInfo.SecurityCookieRVA;
      if (!Image.DynInfo.SecurityCookieRVA || Cookie > UINT32_MAX ||
          !Image.readVA(Cookie, 4))
        return reject(
            "EH4 source has no authenticated security cookie storage");
      bool Referenced = false;
      for (const auto &Section : Compiled.Sections)
        for (const auto &Fixup : Section.FixupReferences)
          Referenced |= absolutePointer(Fixup) && Fixup.ResolvedValue == Cookie;
      if (!Referenced)
        return reject("generated EH4 frame omits the input security cookie");
    }
  }
  for (const auto &Row : Compiled.WinEHSemanticRecords)
    if (!SourceNames.count(Row.SourceFunction))
      return reject(
          "generated scope row belongs to an uninstalled source owner");
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  if (!validateCompiledX86SEHStateWrites(Compiled))
    return reject("physical SEH state-write receipts have no valid closure");
  for (const auto &Frame : Compiled.WinX86SEHFrames)
    if (!SourceNames.count(Frame.SourceFunction))
      return reject(
          "physical SEH frame belongs to an uninstalled source owner");
  for (const auto &Write : Compiled.WinX86SEHStateWrites)
    if (!SourceNames.count(Write.ParentFrame.SourceFunction))
      return reject("callback state write belongs to an uninstalled parent");
#endif

  auto Prepared = Compiled.Bytes;
  auto Append = [&](llvm::ArrayRef<uint8_t> Bytes) -> llvm::Expected<uint32_t> {
    const uint64_t Aligned = llvm::alignTo(Prepared.size(), uint64_t(4));
    if (Aligned > UINT32_MAX || Bytes.size() > UINT32_MAX - Aligned ||
        Aligned + Bytes.size() > uint64_t(UINT32_MAX) + 1 - NewSectionVA)
      return reject("registration metadata exceeds the PE32 address domain");
    Prepared.resize(Aligned, 0);
    const uint32_t RVA = uint32_t(NewSectionVA - Image.Base + Aligned);
    Prepared.insert(Prepared.end(), Bytes.begin(), Bytes.end());
    return RVA;
  };
  std::set<uint32_t> Relocations;
  const auto *RelocDirectory =
      getPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE);
  const bool HasRelocations = RelocDirectory && RelocDirectory->Size &&
                              RelocDirectory->RelativeVirtualAddress;
  if (!RelocDirectory ||
      bool(RelocDirectory->Size) !=
          bool(RelocDirectory->RelativeVirtualAddress) ||
      (Fixed && HasRelocations) || (!Fixed && !HasRelocations))
    return reject("input relocation contract is missing or contradictory");
  if (HasRelocations) {
    auto Offset =
        rawOffset(PE, OriginalBinary, RelocDirectory->RelativeVirtualAddress,
                  RelocDirectory->Size);
    if (!Offset)
      return Offset.takeError();
    const auto Bytes = OriginalBinary.slice(*Offset, RelocDirectory->Size);
    size_t Cursor = 0;
    size_t Count = 0;
    while (Cursor < Bytes.size()) {
      if (!rangeInBounds(Cursor, 8, Bytes.size()))
        return reject("input relocation block header is truncated");
      const uint32_t Page = readLE<uint32_t>(Bytes.data() + Cursor);
      const uint32_t Size = readLE<uint32_t>(Bytes.data() + Cursor + 4);
      if ((Page & 0xfff) || Size < 8 || (Size & 3) ||
          !rangeInBounds(Cursor, Size, Bytes.size()))
        return reject("input relocation block has an invalid extent");
      for (size_t I = 8; I < Size; I += 2) {
        if (++Count > limits::kMaxRegistrationEHStateWork)
          return reject("input relocation directory exceeds the work budget");
        const uint16_t Entry = readLE<uint16_t>(Bytes.data() + Cursor + I);
        const unsigned Type = Entry >> 12;
        if (!Type)
          continue;
        if (Type != llvm::COFF::IMAGE_REL_BASED_HIGHLOW ||
            uint64_t(Page) + (Entry & 0xfff) > UINT32_MAX)
          return reject("input has an unsupported PE32 relocation kind");
        const uint32_t RVA = Page + (Entry & 0xfff);
        auto Field = rawOffset(PE, OriginalBinary, RVA, 4);
        if (!Field)
          return Field.takeError();
        bool Removed = false;
        for (const auto &[Original, Generated] : Mappings) {
          const uint64_t Begin = Original - Image.Base;
          if (uint64_t(RVA) < Begin + 5 && Begin < uint64_t(RVA) + 4) {
            if (RVA < Begin || uint64_t(RVA) + 4 > Begin + 5) {
              // Checked narrow shared entries retire the second instruction:
              // MOV EAX, handler (C++) or PUSH table (SEH4). The jump replaces
              // its opcode and first two operand bytes, so its entire field
              // loses relocation ownership. Keeping it would rebase the new
              // jump. No adjacent or unbound field has this authority.
              const auto Source = Sources.find(Original);
              const auto Retired =
                  Source != Sources.end()
                      ? retiredSharedEntryPointer(Image, *Source->second,
                                                  Original, Image.Base + RVA)
                      : std::nullopt;
              if (!Retired ||
                  readLE<uint32_t>(OriginalBinary.data() + *Field) != *Retired)
                return reject("entry trampoline partially overwrites a "
                              "relocation field");
            }
            Removed = true;
          }
        }
        if (!Removed && !Relocations.insert(RVA).second)
          return reject("input relocation directory contains duplicate fields");
      }
      Cursor += Size;
    }
  }
  std::map<va_t, const llvm::Function *> EntryOwners;
  if (Compiled.SourceFunctionOriginalVAs.size() >
      limits::kMaxRegistrationEHStateWork)
    return reject("patched entry owner set exceeds its work budget");
  for (const auto &[Name, Original] : Compiled.SourceFunctionOriginalVAs) {
    if (!Mappings.count(Original))
      continue;
    const auto *Function = RewriteModule.getFunction(Name);
    if (!Function || Function->isDeclaration() ||
        !EntryOwners.emplace(Original, Function).second)
      return reject("patched entry has no unique compiled source definition");
  }
  for (const auto &[Original, Generated] : Mappings) {
    const auto Owner = EntryOwners.find(Original);
    if (Owner == EntryOwners.end())
      return reject("patched entry has no exact compiler source owner");
    auto Address = ownerVA(*Owner->second, Compiled);
    if (!Address)
      return Address.takeError();
    if (*Address != Generated)
      return reject("patched entry target differs from its compiler owner");
    auto Offset = rawOffset(PE, OriginalBinary, Original - Image.Base, 5);
    if (!Offset)
      return Offset.takeError();
    Update.PatchedEntryRVAs.emplace_back(Original - Image.Base,
                                         Generated - Image.Base);
    const auto Source = Sources.find(Original);
    Update.EntryEncodings.push_back(
        Source == Sources.end()
            ? std::nullopt
            : std::optional<ExceptionEncoding>(Source->second->Encoding));
  }
  std::set<va_t> AbsoluteFields;
  for (const auto &Section : Compiled.Sections) {
    if (Section.Name == ".sxdata") {
      if (Section.IsAllocated || Section.IsInImage || Section.Size % 4 ||
          Section.ExternalBytes.size() != Section.Size ||
          Section.SymbolIndexReferences.size() != Section.Size / 4)
        return reject("SafeSEH linker metadata is not physically closed");
      std::set<uint64_t> Offsets;
      for (const auto &Reference : Section.SymbolIndexReferences) {
        if (Reference.Offset % 4 ||
            !rangeInBounds(Reference.Offset, 4, Section.Size) ||
            !Offsets.insert(Reference.Offset).second ||
            Reference.Symbol.empty() || Reference.TargetVA < Image.Base ||
            Reference.TargetVA > UINT32_MAX ||
            !RequiredHandlers.count(uint32_t(Reference.TargetVA - Image.Base)))
          return reject(
              "SafeSEH linker row names an unproven registration handler");
      }
      continue;
    }
    if (!Section.Size) {
      if (!Section.FixupReferences.empty() ||
          !Section.SymbolIndexReferences.empty())
        return reject("empty generated section carries address fixups");
      continue;
    }
    if (!Section.IsAllocated)
      continue;
    if (!Section.IsInImage || Section.VA < NewSectionVA ||
        Section.Offset != Section.VA - NewSectionVA ||
        !rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()) ||
        (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code &&
         Section.Kind != llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData))
      return reject("generated registration section has incompatible placement "
                    "or permissions");
    for (const auto &Fixup : Section.FixupReferences) {
      if (Fixup.IsPCRel || !Fixup.SubtractSymbol.empty())
        continue;
      if (!absolutePointer(Fixup) ||
          !rangeInBounds(Fixup.Offset, 4, Section.Size) ||
          Fixup.ResolvedValue > UINT32_MAX ||
          readLE<uint32_t>(Compiled.Bytes.data() + Section.Offset +
                           Fixup.Offset) != Fixup.ResolvedValue)
        return reject(
            "generated absolute address has no PE32 HIGHLOW contract");
      const va_t Field = Section.VA + Fixup.Offset;
      auto Next = AbsoluteFields.lower_bound(Field);
      if ((Next != AbsoluteFields.end() && *Next < Field + 4) ||
          (Next != AbsoluteFields.begin() && *std::prev(Next) + 4 > Field))
        return reject("generated absolute fixup fields overlap");
      AbsoluteFields.insert(Next, Field);
      if (!Fixed)
        if (!Relocations
                 .insert(uint32_t(Section.VA + Fixup.Offset - Image.Base))
                 .second)
          return reject("generated absolute fixup fields overlap");
    }
  }
  for (va_t Field : RequiredAbsoluteFields)
    if (!AbsoluteFields.count(Field))
      return reject("C++ dispatch pointer has no complete HIGHLOW closure");

  const auto *LoadConfig =
      getPEDataDirectory(PE, llvm::COFF::LOAD_CONFIG_TABLE);
  if (LoadConfig &&
      (bool(LoadConfig->Size) != bool(LoadConfig->RelativeVirtualAddress)))
    return reject("input load configuration has a contradictory extent");
  if (LoadConfig && LoadConfig->Size) {
    Update.LoadConfigRVA = LoadConfig->RelativeVirtualAddress;
    Update.LoadConfigSize = LoadConfig->Size;
    auto ConfigOffset =
        rawOffset(PE, OriginalBinary, LoadConfig->RelativeVirtualAddress,
                  LoadConfig->Size);
    if (!ConfigOffset)
      return ConfigOffset.takeError();
    if (LoadConfig->Size < 4 ||
        readLE<uint32_t>(OriginalBinary.data() + *ConfigOffset) < 4)
      return reject("input load configuration is truncated");
    Update.LoadConfigDeclaredSize =
        readLE<uint32_t>(OriginalBinary.data() + *ConfigOffset);
    // MSVC keeps the directory's compatibility size at 64 while the structure
    // declares its actual append-only ABI. The loader already owns that rule;
    // require its complete extent and a unique raw-backed section here.
    if (Image.DynInfo.LoadConfigRVA != Update.LoadConfigRVA ||
        Image.DynInfo.LoadConfigSize != Update.LoadConfigDeclaredSize)
      return reject("input load configuration differs from its loaded extent");
    ConfigOffset = rawOffset(PE, OriginalBinary, Update.LoadConfigRVA,
                             Update.LoadConfigDeclaredSize);
    if (!ConfigOffset)
      return ConfigOffset.takeError();
    const auto Config =
        OriginalBinary.slice(*ConfigOffset, Update.LoadConfigDeclaredSize);
    using LC = llvm::object::coff_load_configuration32;
    if (GuardUpdate) {
      const std::tuple<bool, uint64_t, size_t> GuardFields[] = {
          {GuardUpdate->ApplyCF, GuardUpdate->CFFunctionCount,
           offsetof(LC, GuardCFFunctionTable)},
          {GuardUpdate->ApplyEHCont, GuardUpdate->EHContinuationCount,
           offsetof(LC, GuardEHContinuationTable)}};
      for (const auto &[Apply, Count, Offset] : GuardFields) {
        if (!Apply)
          continue;
        if (Offset + 4 > Update.LoadConfigDeclaredSize ||
            uint64_t(Update.LoadConfigRVA) + Offset > UINT32_MAX)
          return reject(
              "guard pointer has no complete PE32 load configuration field");
        if (!Fixed) {
          const uint32_t Field = Update.LoadConfigRVA + Offset;
          if (Count)
            Relocations.insert(Field);
          else
            Relocations.erase(Field);
        }
      }
    }
    constexpr size_t TableOffset = offsetof(LC, SEHandlerTable);
    constexpr size_t CountOffset = offsetof(LC, SEHandlerCount);
    if (readLE<uint32_t>(Config.data()) >= CountOffset + 4) {
      const uint32_t Table = readLE<uint32_t>(Config.data() + TableOffset);
      const uint32_t Count = readLE<uint32_t>(Config.data() + CountOffset);
      if (bool(Table) != bool(Count) ||
          Count > limits::kMaxRegistrationEHStateWork)
        return reject("input SafeSEH table has a contradictory extent");
      Update.LoadConfigBytes.assign(Config.begin() + TableOffset,
                                    Config.begin() + CountOffset + 4);
      if (Count) {
        if (Table < Image.Base)
          return reject("input SafeSEH table is outside the image");
        auto Offset =
            rawOffset(PE, OriginalBinary, Table - Image.Base, Count * 4);
        if (!Offset)
          return Offset.takeError();
        for (uint32_t I = 0; I < Count; ++I) {
          const uint32_t RVA =
              readLE<uint32_t>(OriginalBinary.data() + *Offset + I * 4);
          if ((I && RVA <= Update.SafeSEHHandlers.back()) ||
              !Image.isCodeAddress(Image.Base + RVA) ||
              !Image.readVA(Image.Base + RVA, 1))
            return reject(
                "input SafeSEH handlers are not ordered executable RVAs");
          Update.SafeSEHHandlers.push_back(RVA);
        }
        for (uint32_t Handler : OriginalHandlers)
          if (!llvm::is_contained(Update.SafeSEHHandlers, Handler))
            return reject(
                "registered source personality is absent from SafeSEH");
        for (uint32_t Handler : GeneratedHandlers)
          Update.SafeSEHHandlers.push_back(Handler);
        llvm::sort(Update.SafeSEHHandlers);
        Update.SafeSEHHandlers.erase(std::unique(Update.SafeSEHHandlers.begin(),
                                                 Update.SafeSEHHandlers.end()),
                                     Update.SafeSEHHandlers.end());
        if (Update.SafeSEHHandlers.size() > limits::kMaxRegistrationEHStateWork)
          return reject("merged SafeSEH handlers exceed the work budget");
        std::vector<uint8_t> TableBytes(Update.SafeSEHHandlers.size() * 4);
        for (size_t I = 0; I < Update.SafeSEHHandlers.size(); ++I)
          writeLE<uint32_t>(TableBytes.data() + I * 4,
                            Update.SafeSEHHandlers[I]);
        auto RVA = Append(TableBytes);
        if (!RVA)
          return RVA.takeError();
        writeLE<uint32_t>(Update.LoadConfigBytes.data(), Image.Base + *RVA);
        writeLE<uint32_t>(Update.LoadConfigBytes.data() + 4,
                          Update.SafeSEHHandlers.size());
        if (!Fixed)
          Relocations.insert(Update.LoadConfigRVA + TableOffset);
      }
    }
  }
  if (!Fixed) {
    std::vector<uint8_t> RelocBytes;
    auto It = Relocations.begin();
    while (It != Relocations.end()) {
      const uint32_t Page = *It & ~uint32_t(0xfff);
      const size_t Begin = RelocBytes.size();
      RelocBytes.resize(Begin + 8);
      writeLE<uint32_t>(RelocBytes.data() + Begin, Page);
      while (It != Relocations.end() && (*It & ~uint32_t(0xfff)) == Page) {
        const size_t Offset = RelocBytes.size();
        RelocBytes.resize(Offset + 2);
        writeLE<uint16_t>(RelocBytes.data() + Offset,
                          uint16_t(llvm::COFF::IMAGE_REL_BASED_HIGHLOW << 12) |
                              uint16_t(*It & 0xfff));
        ++It;
      }
      RelocBytes.resize(llvm::alignTo(RelocBytes.size(), size_t(4)), 0);
      writeLE<uint32_t>(RelocBytes.data() + Begin + 4,
                        RelocBytes.size() - Begin);
    }
    auto RVA = Append(RelocBytes);
    if (!RVA)
      return RVA.takeError();
    Update.RelocationRVA = *RVA;
    Update.RelocationSize = RelocBytes.size();
  }
  Update.Apply = true;
  Update.SectionRVA = NewSectionVA - Image.Base;
  Update.SectionSize = Prepared.size();
  Update.SectionSHA256 = llvm::SHA256::hash(Prepared);
  Compiled.Bytes.swap(Prepared);
  return Update;
#endif
}

llvm::Error
applyCOFFRegistrationPatch(std::vector<uint8_t> &Binary,
                           const COFFRegistrationPatchUpdate &Update) {
  if (!Update.Apply)
    return llvm::Error::success();
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (!PE.valid() || PE.Is64 ||
      !getPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE))
    return reject(
        "final PE32 cannot install the registration relocation contract");
  if (!Update.LoadConfigBytes.empty()) {
    if (Update.LoadConfigBytes.size() != 8)
      return reject("SafeSEH update has an invalid field extent");
    auto Offset = rawOffset(
        PE, Binary,
        Update.LoadConfigRVA +
            offsetof(llvm::object::coff_load_configuration32, SEHandlerTable),
        8);
    if (!Offset)
      return Offset.takeError();
    std::copy(Update.LoadConfigBytes.begin(), Update.LoadConfigBytes.end(),
              Binary.begin() + *Offset);
  }
  setPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE,
                     Update.RelocationRVA, Update.RelocationSize);
  clearPEChecksum(PE);
  return llvm::Error::success();
#endif
}

llvm::Error
validateCOFFRegistrationPatch(llvm::ArrayRef<uint8_t> Binary,
                              const COFFRegistrationPatchUpdate &Update) {
  if (!Update.Apply)
    return llvm::Error::success();
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return reject("LLVM does not provide indexed x86 SEH output receipts");
#else
  auto PE =
      locatePEHeaders(const_cast<uint8_t *>(Binary.data()), Binary.size());
  if (!PE.valid() || PE.Is64 ||
      PE.FileHeader->Machine != llvm::COFF::IMAGE_FILE_MACHINE_I386 ||
      getPEImageBase(PE) != Update.ImageBase ||
      getPE32OptionalHeader(PE)->DLLCharacteristics !=
          Update.DllCharacteristics ||
      PE.FileHeader->Characteristics != Update.FileCharacteristics)
    return reject("final registration image changed its PE32 safety flags");
  auto Offset = rawOffset(PE, Binary, Update.SectionRVA, Update.SectionSize);
  if (!Offset)
    return Offset.takeError();
  if (llvm::SHA256::hash(Binary.slice(*Offset, Update.SectionSize)) !=
      Update.SectionSHA256)
    return reject("installed registration image differs from the validated "
                  "compiler bytes");
  bool Executable = false;
  forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
    if (Update.SectionRVA >= S.VirtualAddress &&
        rangeInBounds(uint64_t(Update.SectionRVA) - S.VirtualAddress,
                      Update.SectionSize,
                      getPESectionContentSize(S.VirtualSize, S.SizeOfRawData)))
      Executable = (S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) &&
                   (S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ) &&
                   !(S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE);
  });
  if (!Executable)
    return reject(
        "installed registration section has incompatible memory permissions");
  if (Update.PatchedEntryRVAs.empty() ||
      Update.EntryEncodings.size() != Update.PatchedEntryRVAs.size() ||
      Update.PatchedEntryRVAs.size() > limits::kMaxRegistrationEHStateWork)
    return reject("installed registration has no bounded entry receipt");
  uint64_t PreviousEnd = 0;
  for (const auto &[Original, Generated] : Update.PatchedEntryRVAs) {
    if (Original < PreviousEnd ||
        uint64_t(Original) + Update.ImageBase + 5 > uint64_t(UINT32_MAX) + 1 ||
        Generated < Update.SectionRVA ||
        uint64_t(Generated) - Update.SectionRVA >= Update.SectionSize)
      return reject("installed registration entry receipt has invalid extents");
    PreviousEnd = uint64_t(Original) + 5;
    auto Entry = rawOffset(PE, Binary, Original, 5);
    if (!Entry)
      return Entry.takeError();
    size_t Owners = 0;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      if (Original >= Section.VirtualAddress &&
          rangeInBounds(uint64_t(Original) - Section.VirtualAddress, 5,
                        getPESectionContentSize(Section.VirtualSize,
                                                Section.SizeOfRawData)) &&
          (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) &&
          (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ))
        ++Owners;
    });
    const auto *Bytes = Binary.data() + *Entry;
    if (Owners != 1 || Bytes[0] != 0xe9 ||
        uint32_t(Original + 5 + readLE<uint32_t>(Bytes + 1)) != Generated)
      return reject("installed registration entry differs from its exact "
                    "compiler trampoline");
  }
  size_t CxxOwners = 0;
  for (const auto &Encoding : Update.EntryEncodings) {
    if (!Encoding)
      continue;
    if (*Encoding == ExceptionEncoding::X86CxxFuncInfo)
      ++CxxOwners;
    else if (*Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
             *Encoding != ExceptionEncoding::X86ScopeTableEH4)
      return reject("installed registration has an unsupported entry encoding");
  }
  if (CxxOwners != Update.GeneratedCxxGraphs.size())
    return reject("installed C++ graph set differs from its entry encodings");
  if (CxxOwners) {
    BinaryImage Reparsed;
    Reparsed.Base = Update.ImageBase;
    Reparsed.Arch = Arch::X86;
    Reparsed.Bits = Bitness::Bits32;
    Reparsed.Format = BinaryFormat::COFF;
    bool InvalidSections = false;
    std::vector<std::pair<uint64_t, uint64_t>> VirtualRanges, RawRanges;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      const uint64_t Extent = std::max(uint32_t(Section.VirtualSize),
                                       uint32_t(Section.SizeOfRawData));
      if (!Extent)
        return;
      const uint64_t VA = uint64_t(Update.ImageBase) + Section.VirtualAddress;
      if (VA > UINT32_MAX || Extent > uint64_t(UINT32_MAX) + 1 - VA ||
          !rangeInBounds(Section.PointerToRawData, Section.SizeOfRawData,
                         Binary.size())) {
        InvalidSections = true;
        return;
      }
      VirtualRanges.emplace_back(VA, VA + Extent);
      if (Section.SizeOfRawData)
        RawRanges.emplace_back(Section.PointerToRawData,
                               uint64_t(Section.PointerToRawData) +
                                   Section.SizeOfRawData);
      Segment Mapped;
      Mapped.VA = VA;
      Mapped.Size = Extent;
      Mapped.Flags = coffFlagsToNd(Section.Characteristics);
      Mapped.Data.assign(Binary.begin() + Section.PointerToRawData,
                         Binary.begin() + Section.PointerToRawData +
                             Section.SizeOfRawData);
      Reparsed.Segments.push_back(std::move(Mapped));
    });
    auto Overlaps = [](auto &Ranges) {
      llvm::sort(Ranges);
      for (size_t I = 1; I < Ranges.size(); ++I)
        if (Ranges[I].first < Ranges[I - 1].second)
          return true;
      return false;
    };
    if (InvalidSections || Overlaps(VirtualRanges) || Overlaps(RawRanges))
      return reject("installed C++ graph has conflicting PE section storage");
    std::set<va_t> FuncInfos;
    size_t Work = CxxOwners;
    for (const auto &Expected : Update.GeneratedCxxGraphs) {
      const auto Charge = [&](size_t Amount) {
        if (Amount > limits::kMaxRegistrationEHStateWork - Work)
          return false;
        Work += Amount;
        return true;
      };
      if (!Charge(Expected.UnwindMap.size() + Expected.TryBlocks.size() +
                  Expected.IPMap.size() + Expected.ExceptionSpecTypes.size() +
                  1))
        return reject(
            "installed C++ graph reanalysis exhausted its work budget");
      for (const auto &Try : Expected.TryBlocks)
        if (!Charge(Try.Handlers.size()))
          return reject(
              "installed C++ catch reanalysis exhausted its work budget");
      if (!FuncInfos.insert(Expected.NativeFuncInfoVA).second)
        return reject("installed C++ graphs reuse one FuncInfo owner");
      auto Decoded = coff_loader::getCheckedX86CxxFuncInfoRecords(
          Reparsed, Expected.NativeFuncInfoVA);
      if (!Decoded || !Decoded->HasDistinctRanges || Decoded->Cxx != Expected)
        return reject(
            "installed C++ FuncInfo differs from its normalized graph");
      for (const auto &Range : Decoded->Ranges)
        if (Range.Begin < uint64_t(Update.ImageBase) + Update.SectionRVA ||
            Range.End > uint64_t(Update.ImageBase) + Update.SectionRVA +
                            Update.SectionSize)
          return reject("installed C++ record leaves its generated section");
    }
  }
  const auto *Reloc = getPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE);
  if (!Reloc || Reloc->RelativeVirtualAddress != Update.RelocationRVA ||
      Reloc->Size != Update.RelocationSize)
    return reject(
        "installed relocation directory differs from the prepared contract");
  const auto *Config = getPEDataDirectory(PE, llvm::COFF::LOAD_CONFIG_TABLE);
  if ((Config ? uint32_t(Config->RelativeVirtualAddress) : 0) !=
          Update.LoadConfigRVA ||
      (Config ? uint32_t(Config->Size) : 0) != Update.LoadConfigSize)
    return reject("installed load configuration changed its directory extent");
  if (Update.LoadConfigRVA) {
    auto Header = rawOffset(PE, Binary, Update.LoadConfigRVA, 4);
    if (!Header)
      return Header.takeError();
    if (readLE<uint32_t>(Binary.data() + *Header) !=
        Update.LoadConfigDeclaredSize)
      return reject("installed load configuration changed its declared extent");
  }
  if (!Update.LoadConfigBytes.empty()) {
    if (!Config ||
        Update.LoadConfigDeclaredSize <
            offsetof(llvm::object::coff_load_configuration32, SEHandlerCount) +
                4)
      return reject("installed SafeSEH load configuration changed");
    auto Fields = rawOffset(
        PE, Binary,
        Update.LoadConfigRVA +
            offsetof(llvm::object::coff_load_configuration32, SEHandlerTable),
        8);
    if (!Fields)
      return Fields.takeError();
    if (Update.LoadConfigBytes.size() != 8 ||
        Binary.slice(*Fields, 8) !=
            llvm::ArrayRef<uint8_t>(Update.LoadConfigBytes))
      return reject(
          "installed SafeSEH fields differ from the prepared contract");
  }
  return llvm::Error::success();
#endif
}

} // namespace neverd
