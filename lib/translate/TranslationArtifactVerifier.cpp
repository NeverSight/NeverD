//===- TranslationArtifactVerifier.cpp - Audit generated objects ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/TranslationArtifactVerifier.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace neverd::translate {

char TranslationArtifactVerificationError::ID;

TranslationArtifactVerificationError::TranslationArtifactVerificationError(
    TranslationArtifactViolation Reason, std::string ItemName,
    std::string Detail)
    : Reason(Reason), ItemName(std::move(ItemName)), Detail(std::move(Detail)) {
}

void TranslationArtifactVerificationError::log(llvm::raw_ostream &OS) const {
  OS << "translation artifact verification";
  if (!ItemName.empty())
    OS << " for '" << ItemName << "'";
  OS << ": ";
  switch (Reason) {
  case TranslationArtifactViolation::InvalidPolicy:
    OS << "translation-artifact policy is invalid";
    break;
  case TranslationArtifactViolation::MalformedObject:
    OS << "object cannot be parsed or inspected completely";
    break;
  case TranslationArtifactViolation::UnsupportedObjectFormat:
    OS << "object format is outside the translation contract";
    break;
  case TranslationArtifactViolation::ObjectFormatMismatch:
    OS << "object format does not match the host triple";
    break;
  case TranslationArtifactViolation::HostArchitectureMismatch:
    OS << "object architecture does not match the host triple";
    break;
  case TranslationArtifactViolation::UnsupportedArtifactKind:
    OS << "artifact is not a relocatable object";
    break;
  case TranslationArtifactViolation::ExecutableWritableSection:
    OS << "section is executable and writable";
    break;
  case TranslationArtifactViolation::ExceptionUnwindMetadata:
    OS << "exception or unwind metadata is forbidden";
    break;
  case TranslationArtifactViolation::StaticInitializer:
    OS << "static initializer or terminator is forbidden";
    break;
  case TranslationArtifactViolation::ThreadLocalStorage:
    OS << "thread-local storage is forbidden";
    break;
  case TranslationArtifactViolation::IndirectSymbol:
    OS << "indirect symbol or IFUNC is forbidden";
    break;
  case TranslationArtifactViolation::DynamicSymbolNotAllowed:
    OS << "dynamic symbols are forbidden";
    break;
  case TranslationArtifactViolation::ExternalSymbolNotAllowed:
    OS << "undefined symbol is outside the exact runtime allowlist";
    break;
  case TranslationArtifactViolation::RelocationTargetNotAllowed:
    OS << "relocation target is neither object-defined nor allowlisted";
    break;
  case TranslationArtifactViolation::RelocationTypeNotAllowed:
    OS << "relocation type is not positively recognized";
    break;
  case TranslationArtifactViolation::DynamicRelocation:
    OS << "dynamic relocation is forbidden";
    break;
  case TranslationArtifactViolation::UnsupportedSection:
    OS << "section is outside the closed artifact contract";
    break;
  case TranslationArtifactViolation::PreemptibleDefinition:
    OS << "weak or preemptible definition is forbidden";
    break;
  case TranslationArtifactViolation::UnsupportedLoadCommand:
    OS << "Mach-O load command is outside the closed artifact contract";
    break;
  case TranslationArtifactViolation::RequiredBlockMissing:
    OS << "required translated-block definition is missing";
    break;
  case TranslationArtifactViolation::InvalidBlockDefinition:
    OS << "translated-block definition violates the v1 manifest";
    break;
  case TranslationArtifactViolation::UnexpectedBlockDefinition:
    OS << "externally resolvable translated block is outside the v1 manifest";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ")";
}

std::error_code
TranslationArtifactVerificationError::convertToErrorCode() const {
  return std::make_error_code(std::errc::executable_format_error);
}

namespace {

llvm::Error failure(TranslationArtifactViolation Reason,
                    llvm::StringRef ItemName = {},
                    llvm::StringRef Detail = {}) {
  return llvm::make_error<TranslationArtifactVerificationError>(
      Reason, ItemName.str(), Detail.str());
}

llvm::Error malformed(llvm::Error Cause, llvm::StringRef ItemName) {
  return failure(TranslationArtifactViolation::MalformedObject, ItemName,
                 llvm::toString(std::move(Cause)));
}

llvm::Triple::ObjectFormatType
objectFormat(const llvm::object::ObjectFile &Object) {
  if (Object.isELF())
    return llvm::Triple::ELF;
  if (Object.isCOFF())
    return llvm::Triple::COFF;
  if (Object.isMachO())
    return llvm::Triple::MachO;
  return llvm::Triple::UnknownObjectFormat;
}

llvm::Triple::ArchType canonicalArchitecture(llvm::Triple::ArchType Arch) {
  switch (Arch) {
  case llvm::Triple::thumb:
    return llvm::Triple::arm;
  default:
    return Arch;
  }
}

bool isSupportedArchitecture(llvm::Triple::ArchType Arch) {
  switch (canonicalArchitecture(Arch)) {
  case llvm::Triple::x86:
  case llvm::Triple::x86_64:
  case llvm::Triple::arm:
  case llvm::Triple::aarch64:
    return true;
  default:
    return false;
  }
}

bool isNamedVariant(llvm::StringRef Name, llvm::StringRef Stem) {
  if (Name == Stem)
    return true;
  if (!Name.starts_with(Stem))
    return false;
  const llvm::StringRef Suffix = Name.drop_front(Stem.size());
  return Suffix.starts_with('.') || Suffix.starts_with('$');
}

llvm::StringRef relocatedSectionName(llvm::StringRef Name) {
  if (Name.starts_with(".rela."))
    return Name.drop_front(5);
  if (Name.starts_with(".rel."))
    return Name.drop_front(4);
  return Name;
}

bool isUnwindSectionName(llvm::StringRef Name) {
  Name = relocatedSectionName(Name);
  return isNamedVariant(Name, ".eh_frame") ||
         isNamedVariant(Name, ".eh_frame_hdr") ||
         isNamedVariant(Name, ".gcc_except_table") ||
         isNamedVariant(Name, ".debug_frame") ||
         isNamedVariant(Name, ".sframe") ||
         isNamedVariant(Name, ".ARM.exidx") ||
         isNamedVariant(Name, ".ARM.extab") || isNamedVariant(Name, ".pdata") ||
         isNamedVariant(Name, ".xdata") || isNamedVariant(Name, ".sxdata") ||
         isNamedVariant(Name, "__eh_frame") ||
         isNamedVariant(Name, "__compact_unwind") ||
         isNamedVariant(Name, "__gcc_except_tab") ||
         isNamedVariant(Name, "__unwind_info");
}

bool isInitializerSectionName(llvm::StringRef Name) {
  Name = relocatedSectionName(Name);
  return isNamedVariant(Name, ".init_array") ||
         isNamedVariant(Name, ".fini_array") ||
         isNamedVariant(Name, ".preinit_array") ||
         isNamedVariant(Name, ".init") || isNamedVariant(Name, ".fini") ||
         isNamedVariant(Name, ".ctors") || isNamedVariant(Name, ".dtors") ||
         Name.starts_with(".CRT$");
}

bool isTLSSectionName(llvm::StringRef Name) {
  Name = relocatedSectionName(Name);
  return isNamedVariant(Name, ".tdata") || isNamedVariant(Name, ".tbss") ||
         isNamedVariant(Name, ".tls") ||
         isNamedVariant(Name, "__thread_data") ||
         isNamedVariant(Name, "__thread_bss") ||
         isNamedVariant(Name, "__thread_vars") ||
         isNamedVariant(Name, "__thread_ptrs") ||
         isNamedVariant(Name, "__thread_init");
}

bool isIndirectSectionName(llvm::StringRef Name) {
  Name = relocatedSectionName(Name);
  return isNamedVariant(Name, ".got") || isNamedVariant(Name, ".got.plt") ||
         isNamedVariant(Name, ".plt") || isNamedVariant(Name, ".iplt") ||
         isNamedVariant(Name, ".igot") || isNamedVariant(Name, ".idata") ||
         isNamedVariant(Name, ".didat") || isNamedVariant(Name, ".edata") ||
         isNamedVariant(Name, ".gfids") || isNamedVariant(Name, ".giats") ||
         isNamedVariant(Name, ".gljmp") || isNamedVariant(Name, "__got") ||
         isNamedVariant(Name, "__auth_got") ||
         isNamedVariant(Name, "__la_symbol_ptr") ||
         isNamedVariant(Name, "__nl_symbol_ptr") ||
         isNamedVariant(Name, "__symbol_stub") ||
         isNamedVariant(Name, "__stub_helper") ||
         isNamedVariant(Name, "__la_resolver") ||
         isNamedVariant(Name, "__stubs") || isNamedVariant(Name, "__interpose");
}

bool isDynamicSectionName(llvm::StringRef Name) {
  return isNamedVariant(Name, ".dynamic") || isNamedVariant(Name, ".dynsym") ||
         isNamedVariant(Name, ".dynstr") || isNamedVariant(Name, ".hash") ||
         isNamedVariant(Name, ".gnu.hash") ||
         Name.starts_with(".gnu.version") || isNamedVariant(Name, ".interp");
}

bool isLinkerDirectiveSectionName(llvm::StringRef Name) {
  return isNamedVariant(Name, ".drectve") ||
         isNamedVariant(Name, ".llvm.linker.options") ||
         isNamedVariant(Name, ".linker-options");
}

llvm::Error verifyELFSection(const llvm::object::SectionRef &Section,
                             llvm::StringRef Name) {
  const llvm::object::ELFSectionRef ELFSection(Section);
  const uint64_t Flags = ELFSection.getFlags();
  const uint32_t Type = ELFSection.getType();
  if ((Flags & llvm::ELF::SHF_EXECINSTR) && (Flags & llvm::ELF::SHF_WRITE))
    return failure(TranslationArtifactViolation::ExecutableWritableSection,
                   Name);
  if (Type == llvm::ELF::SHT_RELR || Type == llvm::ELF::SHT_ANDROID_REL ||
      Type == llvm::ELF::SHT_ANDROID_RELA ||
      Type == llvm::ELF::SHT_ANDROID_RELR || Name.starts_with(".rel.dyn") ||
      Name.starts_with(".rela.dyn"))
    return failure(TranslationArtifactViolation::DynamicRelocation, Name);
  // ObjectFile records a CREL decode problem as side data and exposes a
  // synthesized relocation through the generic iterator. This boundary does
  // not accept a relocation stream whose parse status is not carried by the
  // iterator itself.
  if (Type == llvm::ELF::SHT_CREL)
    return failure(TranslationArtifactViolation::RelocationTypeNotAllowed,
                   Name);
  if (Type == llvm::ELF::SHT_DYNSYM || Type == llvm::ELF::SHT_DYNAMIC ||
      isDynamicSectionName(Name))
    return failure(TranslationArtifactViolation::DynamicSymbolNotAllowed, Name);
  if (Type == llvm::ELF::SHT_GROUP || (Flags & llvm::ELF::SHF_GROUP) ||
      Name.starts_with(".gnu.linkonce."))
    return failure(TranslationArtifactViolation::PreemptibleDefinition, Name,
                   "section participates in linker selection");
  if (isLinkerDirectiveSectionName(Name))
    return failure(TranslationArtifactViolation::UnsupportedSection, Name,
                   "linker directives are forbidden");
  if (isIndirectSectionName(Name))
    return failure(TranslationArtifactViolation::IndirectSymbol, Name);
  if ((Flags & llvm::ELF::SHF_TLS) || isTLSSectionName(Name))
    return failure(TranslationArtifactViolation::ThreadLocalStorage, Name);
  if (Type == llvm::ELF::SHT_INIT_ARRAY || Type == llvm::ELF::SHT_FINI_ARRAY ||
      Type == llvm::ELF::SHT_PREINIT_ARRAY || isInitializerSectionName(Name))
    return failure(TranslationArtifactViolation::StaticInitializer, Name);
  if (Type == llvm::ELF::SHT_ARM_EXIDX ||
      Type == llvm::ELF::SHT_X86_64_UNWIND || isUnwindSectionName(Name))
    return failure(TranslationArtifactViolation::ExceptionUnwindMetadata, Name);
  if ((Flags & llvm::ELF::SHF_ALLOC) && Type != llvm::ELF::SHT_PROGBITS &&
      Type != llvm::ELF::SHT_NOBITS)
    return failure(TranslationArtifactViolation::UnsupportedSection, Name,
                   "unrecognized allocated ELF section type");
  return llvm::Error::success();
}

llvm::Error verifyCOFFSection(const llvm::object::COFFObjectFile &Object,
                              const llvm::object::SectionRef &Section,
                              llvm::StringRef Name) {
  const llvm::object::coff_section *COFFSection =
      Object.getCOFFSection(Section);
  if (!COFFSection)
    return failure(TranslationArtifactViolation::MalformedObject, Name);
  const uint32_t Characteristics = COFFSection->Characteristics;
  const bool IsCode = Characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE;
  const bool IsInitializedData =
      Characteristics & llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
  const bool IsUninitializedData =
      Characteristics & llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA;
  const bool IsLoadable = Characteristics & (llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                                             llvm::COFF::IMAGE_SCN_MEM_READ |
                                             llvm::COFF::IMAGE_SCN_MEM_WRITE);
  const unsigned ContentKindCount = static_cast<unsigned>(IsCode) +
                                    static_cast<unsigned>(IsInitializedData) +
                                    static_cast<unsigned>(IsUninitializedData);
  if (IsLoadable &&
      (ContentKindCount != 1 ||
       static_cast<bool>(Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) !=
           IsCode))
    return failure(TranslationArtifactViolation::UnsupportedSection, Name,
                   "invalid allocated COFF section classification");
  if ((Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) &&
      (Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE))
    return failure(TranslationArtifactViolation::ExecutableWritableSection,
                   Name);
  if (isIndirectSectionName(Name))
    return failure(TranslationArtifactViolation::IndirectSymbol, Name);
  if (isLinkerDirectiveSectionName(Name))
    return failure(TranslationArtifactViolation::UnsupportedSection, Name,
                   "linker directives are forbidden");
  if (Characteristics & llvm::COFF::IMAGE_SCN_LNK_COMDAT)
    return failure(TranslationArtifactViolation::PreemptibleDefinition, Name,
                   "COMDAT section permits definition selection");
  if (isTLSSectionName(Name))
    return failure(TranslationArtifactViolation::ThreadLocalStorage, Name);
  if (isInitializerSectionName(Name))
    return failure(TranslationArtifactViolation::StaticInitializer, Name);
  if (isUnwindSectionName(Name))
    return failure(TranslationArtifactViolation::ExceptionUnwindMetadata, Name);
  return llvm::Error::success();
}

llvm::Error verifyMachOSection(const llvm::object::MachOObjectFile &Object,
                               const llvm::object::SectionRef &Section,
                               llvm::StringRef Name) {
  const llvm::object::DataRefImpl Ref = Section.getRawDataRefImpl();
  const uint32_t Flags = Object.is64Bit() ? Object.getSection64(Ref).flags
                                          : Object.getSection(Ref).flags;
  const uint32_t Type = Flags & llvm::MachO::SECTION_TYPE;
  const bool ContainsInstructions =
      Flags & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
               llvm::MachO::S_ATTR_SOME_INSTRUCTIONS);
  // MH_OBJECT sections do not carry independent VM protections. The final
  // segment name is the only placement contract exposed by ObjectFile, so an
  // executable section outside __TEXT cannot be proven non-writable.
  if (ContainsInstructions &&
      Object.getSectionFinalSegmentName(Ref) != "__TEXT")
    return failure(TranslationArtifactViolation::ExecutableWritableSection,
                   Name);
  switch (Type) {
  case llvm::MachO::S_REGULAR:
  case llvm::MachO::S_ZEROFILL:
  case llvm::MachO::S_CSTRING_LITERALS:
  case llvm::MachO::S_4BYTE_LITERALS:
  case llvm::MachO::S_8BYTE_LITERALS:
  case llvm::MachO::S_GB_ZEROFILL:
  case llvm::MachO::S_16BYTE_LITERALS:
    break;
  case llvm::MachO::S_COALESCED:
    return failure(TranslationArtifactViolation::PreemptibleDefinition, Name,
                   "coalesced section permits definition selection");
  case llvm::MachO::S_MOD_INIT_FUNC_POINTERS:
  case llvm::MachO::S_MOD_TERM_FUNC_POINTERS:
  case llvm::MachO::S_INIT_FUNC_OFFSETS:
    return failure(TranslationArtifactViolation::StaticInitializer, Name);
  case llvm::MachO::S_THREAD_LOCAL_REGULAR:
  case llvm::MachO::S_THREAD_LOCAL_ZEROFILL:
  case llvm::MachO::S_THREAD_LOCAL_VARIABLES:
  case llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS:
  case llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS:
    return failure(TranslationArtifactViolation::ThreadLocalStorage, Name);
  case llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS:
  case llvm::MachO::S_LAZY_SYMBOL_POINTERS:
  case llvm::MachO::S_SYMBOL_STUBS:
  case llvm::MachO::S_LAZY_DYLIB_SYMBOL_POINTERS:
  case llvm::MachO::S_INTERPOSING:
    return failure(TranslationArtifactViolation::IndirectSymbol, Name);
  default:
    return failure(TranslationArtifactViolation::UnsupportedSection, Name,
                   "unrecognized Mach-O section type");
  }
  if (isIndirectSectionName(Name))
    return failure(TranslationArtifactViolation::IndirectSymbol, Name);
  if (isInitializerSectionName(Name))
    return failure(TranslationArtifactViolation::StaticInitializer, Name);
  if (isTLSSectionName(Name))
    return failure(TranslationArtifactViolation::ThreadLocalStorage, Name);
  if (isUnwindSectionName(Name))
    return failure(TranslationArtifactViolation::ExceptionUnwindMetadata, Name);
  return llvm::Error::success();
}

llvm::Error
verifyMachOLoadCommands(const llvm::object::MachOObjectFile &Object) {
  unsigned SegmentCount = 0;
  unsigned SymbolTableCount = 0;
  unsigned DynamicSymbolTableCount = 0;
  unsigned PlatformCount = 0;
  unsigned DataInCodeCount = 0;

  for (const llvm::object::MachOObjectFile::LoadCommandInfo &Command :
       Object.load_commands()) {
    switch (Command.C.cmd) {
    case llvm::MachO::LC_SEGMENT:
      if (Object.is64Bit())
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "LC_SEGMENT", "32-bit segment in a 64-bit object");
      if (++SegmentCount > 1)
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "LC_SEGMENT", "multiple object segments");
      break;
    case llvm::MachO::LC_SEGMENT_64:
      if (!Object.is64Bit())
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "LC_SEGMENT_64", "64-bit segment in a 32-bit object");
      if (++SegmentCount > 1)
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "LC_SEGMENT_64", "multiple object segments");
      break;
    case llvm::MachO::LC_SYMTAB:
      if (++SymbolTableCount > 1)
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "LC_SYMTAB", "duplicate symbol table command");
      break;
    case llvm::MachO::LC_DYSYMTAB:
      if (++DynamicSymbolTableCount > 1)
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "LC_DYSYMTAB", "duplicate dynamic symbol table command");
      break;
    case llvm::MachO::LC_BUILD_VERSION:
    case llvm::MachO::LC_VERSION_MIN_MACOSX:
    case llvm::MachO::LC_VERSION_MIN_IPHONEOS:
    case llvm::MachO::LC_VERSION_MIN_TVOS:
    case llvm::MachO::LC_VERSION_MIN_WATCHOS:
      if (++PlatformCount > 1)
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "platform version", "multiple platform commands");
      break;
    case llvm::MachO::LC_DATA_IN_CODE:
      if (++DataInCodeCount > 1)
        return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                       "LC_DATA_IN_CODE", "duplicate data-in-code command");
      break;
    default:
      return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                     llvm::utostr(Command.C.cmd));
    }
  }

  if (SegmentCount != 1)
    return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                   "segment", "relocatable object must have one segment");
  if (DynamicSymbolTableCount != 0 && SymbolTableCount == 0)
    return failure(TranslationArtifactViolation::UnsupportedLoadCommand,
                   "LC_DYSYMTAB", "dynamic symbol table has no symbol table");
  return llvm::Error::success();
}

template <typename ELFT>
llvm::Error
verifyELFProgramHeaders(const llvm::object::ELFObjectFile<ELFT> &Object) {
  llvm::Expected<typename ELFT::PhdrRange> HeadersOrErr =
      Object.getELFFile().program_headers();
  if (!HeadersOrErr)
    return malformed(HeadersOrErr.takeError(), "ELF program headers");
  if (!HeadersOrErr->empty())
    return failure(TranslationArtifactViolation::UnsupportedArtifactKind,
                   "ELF program headers",
                   "relocatable translation objects must not define segments");
  return llvm::Error::success();
}

llvm::Error verifyELFProgramHeaders(const llvm::object::ObjectFile &Object) {
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(&Object))
    return verifyELFProgramHeaders(*ELF);
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(&Object))
    return verifyELFProgramHeaders(*ELF);
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF32BEObjectFile>(&Object))
    return verifyELFProgramHeaders(*ELF);
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELF64BEObjectFile>(&Object))
    return verifyELFProgramHeaders(*ELF);
  return failure(TranslationArtifactViolation::MalformedObject,
                 "ELF object class");
}

llvm::Error verifySections(const llvm::object::ObjectFile &Object) {
  const auto *COFF = llvm::dyn_cast<llvm::object::COFFObjectFile>(&Object);
  const auto *MachO = llvm::dyn_cast<llvm::object::MachOObjectFile>(&Object);
  if (Object.isELF())
    if (llvm::Error Error = verifyELFProgramHeaders(Object))
      return Error;
  for (const llvm::object::SectionRef &Section : Object.sections()) {
    llvm::Expected<llvm::StringRef> NameOrErr = Section.getName();
    if (!NameOrErr)
      return malformed(NameOrErr.takeError(), "section name");
    llvm::Expected<llvm::StringRef> ContentsOrErr = Section.getContents();
    if (!ContentsOrErr)
      return malformed(ContentsOrErr.takeError(), *NameOrErr);
    if (Object.isELF()) {
      if (llvm::Error Error = verifyELFSection(Section, *NameOrErr))
        return Error;
    } else if (COFF) {
      if (llvm::Error Error = verifyCOFFSection(*COFF, Section, *NameOrErr))
        return Error;
    } else if (MachO) {
      if (llvm::Error Error = verifyMachOSection(*MachO, Section, *NameOrErr))
        return Error;
    }
  }

  if (MachO) {
    if (llvm::Error Error = verifyMachOLoadCommands(*MachO))
      return Error;
    if (MachO->getDysymtabLoadCommand().nindirectsyms != 0)
      return failure(TranslationArtifactViolation::IndirectSymbol,
                     "LC_DYSYMTAB");
  }
  return llvm::Error::success();
}

bool hasForbiddenDefinitionLinkage(const llvm::object::ObjectFile &Object,
                                   const llvm::object::SymbolRef &Symbol,
                                   uint32_t Flags) {
  if (Flags & llvm::object::SymbolRef::SF_Weak)
    return true;
  if (!(Flags & llvm::object::SymbolRef::SF_Global))
    return false;
  if (Object.isELF()) {
    constexpr uint8_t ELFSymbolVisibilityMask = 0x3;
    const uint8_t Visibility =
        llvm::object::ELFSymbolRef(Symbol).getOther() & ELFSymbolVisibilityMask;
    return Visibility != llvm::ELF::STV_HIDDEN &&
           Visibility != llvm::ELF::STV_INTERNAL;
  }
  // Mach-O represents a hidden external definition as N_PEXT | N_EXT.  LLVM
  // exposes that exact, non-interposable spelling as SF_Global | SF_Hidden;
  // rejecting every global symbol would incorrectly reject the codegen form
  // of an LLVM hidden definition.  Plain N_EXT remains forbidden.
  if (Object.isMachO())
    return !(Flags & llvm::object::SymbolRef::SF_Hidden);
  // A non-weak, non-COMDAT COFF external is a strong definition: duplicate
  // providers are a link error rather than a selectable or interposable
  // definition.  COMDAT sections and weak externals are rejected elsewhere.
  return false;
}

bool isLoadableSection(const llvm::object::ObjectFile &Object,
                       const llvm::object::SectionRef &Section);

bool isExecutableSection(const llvm::object::ObjectFile &Object,
                         const llvm::object::SectionRef &Section) {
  if (Object.isELF()) {
    const llvm::object::ELFSectionRef ELFSection(Section);
    return ELFSection.getType() == llvm::ELF::SHT_PROGBITS &&
           (ELFSection.getFlags() & llvm::ELF::SHF_EXECINSTR);
  }
  if (const auto *COFF =
          llvm::dyn_cast<llvm::object::COFFObjectFile>(&Object)) {
    const llvm::object::coff_section *Raw = COFF->getCOFFSection(Section);
    return Raw && (Raw->Characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE) &&
           (Raw->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
  }
  if (const auto *MachO =
          llvm::dyn_cast<llvm::object::MachOObjectFile>(&Object)) {
    const llvm::object::DataRefImpl Ref = Section.getRawDataRefImpl();
    const uint32_t Flags = MachO->is64Bit() ? MachO->getSection64(Ref).flags
                                            : MachO->getSection(Ref).flags;
    return (Flags & llvm::MachO::SECTION_TYPE) == llvm::MachO::S_REGULAR &&
           (Flags & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                     llvm::MachO::S_ATTR_SOME_INSTRUCTIONS));
  }
  return false;
}

llvm::Expected<uint64_t> symbolExtent(const llvm::object::ObjectFile &Object,
                                      const llvm::object::SymbolRef &Symbol,
                                      const llvm::object::SectionRef &Section) {
  llvm::Expected<uint64_t> AddressOrErr = Symbol.getAddress();
  if (!AddressOrErr)
    return AddressOrErr.takeError();
  const uint64_t SectionAddress = Section.getAddress();
  const uint64_t SectionSize = Section.getSize();
  if (SectionAddress > std::numeric_limits<uint64_t>::max() - SectionSize ||
      *AddressOrErr < SectionAddress ||
      *AddressOrErr - SectionAddress >= SectionSize)
    return uint64_t{0};
  const uint64_t SectionEnd = SectionAddress + SectionSize;

  if (Object.isELF()) {
    const uint64_t Size = llvm::object::ELFSymbolRef(Symbol).getSize();
    if (Size > SectionEnd - *AddressOrErr)
      return uint64_t{0};
    return Size;
  }

  uint64_t End = SectionEnd;
  for (const llvm::object::SymbolRef &Candidate : Object.symbols()) {
    llvm::Expected<llvm::object::section_iterator> CandidateSectionOrErr =
        Candidate.getSection();
    if (!CandidateSectionOrErr)
      return CandidateSectionOrErr.takeError();
    if (*CandidateSectionOrErr == Object.section_end() ||
        **CandidateSectionOrErr != Section)
      continue;
    llvm::Expected<uint64_t> CandidateAddressOrErr = Candidate.getAddress();
    if (!CandidateAddressOrErr)
      return CandidateAddressOrErr.takeError();
    if (*CandidateAddressOrErr > *AddressOrErr && *CandidateAddressOrErr < End)
      End = *CandidateAddressOrErr;
  }
  return End - *AddressOrErr;
}

llvm::Error verifyRequiredBlockManifest(
    const llvm::object::ObjectFile &Object,
    const llvm::StringSet<> &RequiredSymbols,
    llvm::ArrayRef<llvm::StringRef> RequiredSymbolOrder) {
  if (RequiredSymbols.empty())
    return llvm::Error::success();
  llvm::StringSet<> DefinedSymbols;
  for (const llvm::object::SymbolRef &Symbol : Object.symbols()) {
    llvm::Expected<llvm::StringRef> NameOrErr = Symbol.getName();
    if (!NameOrErr)
      return malformed(NameOrErr.takeError(), "symbol name");
    llvm::Expected<uint32_t> FlagsOrErr = Symbol.getFlags();
    if (!FlagsOrErr)
      return malformed(FlagsOrErr.takeError(), *NameOrErr);
    if (!RequiredSymbols.contains(*NameOrErr)) {
      if (!(*FlagsOrErr & (llvm::object::SymbolRef::SF_Undefined |
                           llvm::object::SymbolRef::SF_Absolute |
                           llvm::object::SymbolRef::SF_Common |
                           llvm::object::SymbolRef::SF_Indirect)) &&
          (*FlagsOrErr & llvm::object::SymbolRef::SF_Global)) {
        llvm::Expected<llvm::object::section_iterator> SectionOrErr =
            Symbol.getSection();
        if (!SectionOrErr)
          return malformed(SectionOrErr.takeError(), *NameOrErr);
        if (*SectionOrErr != Object.section_end() &&
            isExecutableSection(Object, **SectionOrErr))
          return failure(
              TranslationArtifactViolation::UnexpectedBlockDefinition,
              *NameOrErr);
      }
      continue;
    }
    if (!DefinedSymbols.insert(*NameOrErr).second)
      return failure(TranslationArtifactViolation::InvalidBlockDefinition,
                     *NameOrErr, "duplicate required symbol");
    if (*FlagsOrErr & (llvm::object::SymbolRef::SF_Undefined |
                       llvm::object::SymbolRef::SF_Absolute |
                       llvm::object::SymbolRef::SF_Common |
                       llvm::object::SymbolRef::SF_Indirect) ||
        hasForbiddenDefinitionLinkage(Object, Symbol, *FlagsOrErr))
      return failure(TranslationArtifactViolation::InvalidBlockDefinition,
                     *NameOrErr, "definition is undefined or preemptible");
    llvm::Expected<llvm::object::SymbolRef::Type> TypeOrErr = Symbol.getType();
    if (!TypeOrErr)
      return malformed(TypeOrErr.takeError(), *NameOrErr);
    if (*TypeOrErr != llvm::object::SymbolRef::ST_Function)
      return failure(TranslationArtifactViolation::InvalidBlockDefinition,
                     *NameOrErr, "definition is not a function");
    llvm::Expected<llvm::object::section_iterator> SectionOrErr =
        Symbol.getSection();
    if (!SectionOrErr)
      return malformed(SectionOrErr.takeError(), *NameOrErr);
    if (*SectionOrErr == Object.section_end() ||
        !isLoadableSection(Object, **SectionOrErr) ||
        !isExecutableSection(Object, **SectionOrErr))
      return failure(TranslationArtifactViolation::InvalidBlockDefinition,
                     *NameOrErr, "definition is not in executable memory");
    llvm::Expected<uint64_t> ExtentOrErr =
        symbolExtent(Object, Symbol, **SectionOrErr);
    if (!ExtentOrErr)
      return malformed(ExtentOrErr.takeError(), *NameOrErr);
    if (*ExtentOrErr == 0)
      return failure(TranslationArtifactViolation::InvalidBlockDefinition,
                     *NameOrErr, "definition has zero size");
  }
  for (llvm::StringRef Required : RequiredSymbolOrder)
    if (!DefinedSymbols.contains(Required))
      return failure(TranslationArtifactViolation::RequiredBlockMissing,
                     Required);
  return llvm::Error::success();
}

llvm::Error verifySymbols(const llvm::object::ObjectFile &Object,
                          const llvm::StringSet<> &AllowedSymbols) {
  const auto *ELF = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(&Object);
  for (const llvm::object::SymbolRef &Symbol : Object.symbols()) {
    llvm::Expected<llvm::StringRef> NameOrErr = Symbol.getName();
    if (!NameOrErr)
      return malformed(NameOrErr.takeError(), "symbol name");
    llvm::Expected<uint32_t> FlagsOrErr = Symbol.getFlags();
    if (!FlagsOrErr)
      return malformed(FlagsOrErr.takeError(), *NameOrErr);

    if (ELF && llvm::object::ELFSymbolRef(Symbol).getELFType() ==
                   llvm::ELF::STT_GNU_IFUNC)
      return failure(TranslationArtifactViolation::IndirectSymbol, *NameOrErr);
    if (*FlagsOrErr & llvm::object::SymbolRef::SF_Indirect)
      return failure(TranslationArtifactViolation::IndirectSymbol, *NameOrErr);
    if (*FlagsOrErr & llvm::object::SymbolRef::SF_Common)
      return failure(TranslationArtifactViolation::ExternalSymbolNotAllowed,
                     *NameOrErr);
    if (*FlagsOrErr & llvm::object::SymbolRef::SF_Undefined) {
      if (NameOrErr->empty() ||
          (*FlagsOrErr & llvm::object::SymbolRef::SF_Weak) ||
          !AllowedSymbols.contains(*NameOrErr))
        return failure(TranslationArtifactViolation::ExternalSymbolNotAllowed,
                       *NameOrErr);
      continue;
    }
    if (hasForbiddenDefinitionLinkage(Object, Symbol, *FlagsOrErr))
      return failure(TranslationArtifactViolation::PreemptibleDefinition,
                     *NameOrErr);
  }

  if (!ELF)
    return llvm::Error::success();
  for (const llvm::object::ELFSymbolRef &Symbol :
       ELF->getDynamicSymbolIterators()) {
    llvm::Expected<llvm::StringRef> NameOrErr = Symbol.getName();
    if (!NameOrErr)
      return malformed(NameOrErr.takeError(), "dynamic symbol name");
    if (NameOrErr->empty())
      continue;
    if (Symbol.getELFType() == llvm::ELF::STT_GNU_IFUNC)
      return failure(TranslationArtifactViolation::IndirectSymbol, *NameOrErr);
    return failure(TranslationArtifactViolation::DynamicSymbolNotAllowed,
                   *NameOrErr);
  }
  return llvm::Error::success();
}

struct RelocationShape {
  uint8_t Width = 0;
  uint8_t Alignment = 1;
};

RelocationShape elfRelocationShape(llvm::Triple::ArchType Arch, uint64_t Type) {
  switch (canonicalArchitecture(Arch)) {
  case llvm::Triple::x86_64:
    switch (Type) {
    case llvm::ELF::R_X86_64_64:
    case llvm::ELF::R_X86_64_PC64:
      return {8, 1};
    case llvm::ELF::R_X86_64_PC32:
    case llvm::ELF::R_X86_64_PLT32:
    case llvm::ELF::R_X86_64_32:
    case llvm::ELF::R_X86_64_32S:
      return {4, 1};
    case llvm::ELF::R_X86_64_16:
    case llvm::ELF::R_X86_64_PC16:
      return {2, 1};
    case llvm::ELF::R_X86_64_8:
    case llvm::ELF::R_X86_64_PC8:
      return {1, 1};
    default:
      return {};
    }
  case llvm::Triple::x86:
    switch (Type) {
    case llvm::ELF::R_386_32:
    case llvm::ELF::R_386_PC32:
      return {4, 1};
    case llvm::ELF::R_386_16:
    case llvm::ELF::R_386_PC16:
      return {2, 1};
    case llvm::ELF::R_386_8:
    case llvm::ELF::R_386_PC8:
      return {1, 1};
    default:
      return {};
    }
  case llvm::Triple::aarch64:
    switch (Type) {
    case llvm::ELF::R_AARCH64_ABS64:
    case llvm::ELF::R_AARCH64_PREL64:
      return {8, 1};
    case llvm::ELF::R_AARCH64_ABS32:
    case llvm::ELF::R_AARCH64_PREL32:
      return {4, 1};
    case llvm::ELF::R_AARCH64_ABS16:
    case llvm::ELF::R_AARCH64_PREL16:
      return {2, 1};
    case llvm::ELF::R_AARCH64_MOVW_UABS_G0:
    case llvm::ELF::R_AARCH64_MOVW_UABS_G0_NC:
    case llvm::ELF::R_AARCH64_MOVW_UABS_G1:
    case llvm::ELF::R_AARCH64_MOVW_UABS_G1_NC:
    case llvm::ELF::R_AARCH64_MOVW_UABS_G2:
    case llvm::ELF::R_AARCH64_MOVW_UABS_G2_NC:
    case llvm::ELF::R_AARCH64_MOVW_UABS_G3:
    case llvm::ELF::R_AARCH64_MOVW_SABS_G0:
    case llvm::ELF::R_AARCH64_MOVW_SABS_G1:
    case llvm::ELF::R_AARCH64_MOVW_SABS_G2:
    case llvm::ELF::R_AARCH64_LD_PREL_LO19:
    case llvm::ELF::R_AARCH64_ADR_PREL_LO21:
    case llvm::ELF::R_AARCH64_ADR_PREL_PG_HI21:
    case llvm::ELF::R_AARCH64_ADR_PREL_PG_HI21_NC:
    case llvm::ELF::R_AARCH64_ADD_ABS_LO12_NC:
    case llvm::ELF::R_AARCH64_LDST8_ABS_LO12_NC:
    case llvm::ELF::R_AARCH64_TSTBR14:
    case llvm::ELF::R_AARCH64_CONDBR19:
    case llvm::ELF::R_AARCH64_JUMP26:
    case llvm::ELF::R_AARCH64_CALL26:
    case llvm::ELF::R_AARCH64_LDST16_ABS_LO12_NC:
    case llvm::ELF::R_AARCH64_LDST32_ABS_LO12_NC:
    case llvm::ELF::R_AARCH64_LDST64_ABS_LO12_NC:
    case llvm::ELF::R_AARCH64_MOVW_PREL_G0:
    case llvm::ELF::R_AARCH64_MOVW_PREL_G0_NC:
    case llvm::ELF::R_AARCH64_MOVW_PREL_G1:
    case llvm::ELF::R_AARCH64_MOVW_PREL_G1_NC:
    case llvm::ELF::R_AARCH64_MOVW_PREL_G2:
    case llvm::ELF::R_AARCH64_MOVW_PREL_G2_NC:
    case llvm::ELF::R_AARCH64_MOVW_PREL_G3:
      return {4, 4};
    default:
      return {};
    }
  case llvm::Triple::arm:
    switch (Type) {
    case llvm::ELF::R_ARM_ABS32:
    case llvm::ELF::R_ARM_REL32:
    case llvm::ELF::R_ARM_ABS32_NOI:
    case llvm::ELF::R_ARM_REL32_NOI:
      return {4, 1};
    case llvm::ELF::R_ARM_ABS16:
      return {2, 1};
    case llvm::ELF::R_ARM_ABS12:
      return {4, 4};
    case llvm::ELF::R_ARM_ABS8:
      return {1, 1};
    case llvm::ELF::R_ARM_PC24:
    case llvm::ELF::R_ARM_THM_CALL:
    case llvm::ELF::R_ARM_CALL:
    case llvm::ELF::R_ARM_JUMP24:
    case llvm::ELF::R_ARM_THM_JUMP24:
    case llvm::ELF::R_ARM_PREL31:
    case llvm::ELF::R_ARM_MOVW_ABS_NC:
    case llvm::ELF::R_ARM_MOVT_ABS:
    case llvm::ELF::R_ARM_MOVW_PREL_NC:
    case llvm::ELF::R_ARM_MOVT_PREL:
    case llvm::ELF::R_ARM_THM_MOVW_ABS_NC:
    case llvm::ELF::R_ARM_THM_MOVT_ABS:
    case llvm::ELF::R_ARM_THM_MOVW_PREL_NC:
    case llvm::ELF::R_ARM_THM_MOVT_PREL:
    case llvm::ELF::R_ARM_THM_JUMP19:
    case llvm::ELF::R_ARM_THM_JUMP6:
      return {4, 2};
    case llvm::ELF::R_ARM_THM_JUMP11:
    case llvm::ELF::R_ARM_THM_JUMP8:
      return {2, 2};
    default:
      return {};
    }
  default:
    return {};
  }
}

RelocationShape coffRelocationShape(llvm::Triple::ArchType Arch,
                                    uint64_t Type) {
  switch (canonicalArchitecture(Arch)) {
  case llvm::Triple::x86_64:
    switch (Type) {
    case llvm::COFF::IMAGE_REL_AMD64_ADDR64:
      return {8, 1};
    case llvm::COFF::IMAGE_REL_AMD64_ADDR32:
    case llvm::COFF::IMAGE_REL_AMD64_ADDR32NB:
    case llvm::COFF::IMAGE_REL_AMD64_REL32:
    case llvm::COFF::IMAGE_REL_AMD64_REL32_1:
    case llvm::COFF::IMAGE_REL_AMD64_REL32_2:
    case llvm::COFF::IMAGE_REL_AMD64_REL32_3:
    case llvm::COFF::IMAGE_REL_AMD64_REL32_4:
    case llvm::COFF::IMAGE_REL_AMD64_REL32_5:
    case llvm::COFF::IMAGE_REL_AMD64_SECREL:
      return {4, 1};
    case llvm::COFF::IMAGE_REL_AMD64_SECTION:
      return {2, 1};
    default:
      return {};
    }
  case llvm::Triple::x86:
    switch (Type) {
    case llvm::COFF::IMAGE_REL_I386_DIR32:
    case llvm::COFF::IMAGE_REL_I386_DIR32NB:
    case llvm::COFF::IMAGE_REL_I386_SECREL:
    case llvm::COFF::IMAGE_REL_I386_REL32:
      return {4, 1};
    case llvm::COFF::IMAGE_REL_I386_SECTION:
      return {2, 1};
    default:
      return {};
    }
  case llvm::Triple::aarch64:
    switch (Type) {
    case llvm::COFF::IMAGE_REL_ARM64_ADDR64:
      return {8, 1};
    case llvm::COFF::IMAGE_REL_ARM64_ADDR32:
    case llvm::COFF::IMAGE_REL_ARM64_ADDR32NB:
    case llvm::COFF::IMAGE_REL_ARM64_SECREL:
    case llvm::COFF::IMAGE_REL_ARM64_REL32:
      return {4, 1};
    case llvm::COFF::IMAGE_REL_ARM64_BRANCH26:
    case llvm::COFF::IMAGE_REL_ARM64_PAGEBASE_REL21:
    case llvm::COFF::IMAGE_REL_ARM64_REL21:
    case llvm::COFF::IMAGE_REL_ARM64_PAGEOFFSET_12A:
    case llvm::COFF::IMAGE_REL_ARM64_PAGEOFFSET_12L:
    case llvm::COFF::IMAGE_REL_ARM64_SECREL_LOW12A:
    case llvm::COFF::IMAGE_REL_ARM64_SECREL_HIGH12A:
    case llvm::COFF::IMAGE_REL_ARM64_SECREL_LOW12L:
    case llvm::COFF::IMAGE_REL_ARM64_BRANCH19:
    case llvm::COFF::IMAGE_REL_ARM64_BRANCH14:
      return {4, 4};
    case llvm::COFF::IMAGE_REL_ARM64_SECTION:
      return {2, 1};
    default:
      return {};
    }
  case llvm::Triple::arm:
    switch (Type) {
    case llvm::COFF::IMAGE_REL_ARM_ADDR32:
    case llvm::COFF::IMAGE_REL_ARM_ADDR32NB:
    case llvm::COFF::IMAGE_REL_ARM_REL32:
    case llvm::COFF::IMAGE_REL_ARM_SECREL:
      return {4, 1};
    case llvm::COFF::IMAGE_REL_ARM_BRANCH20T:
    case llvm::COFF::IMAGE_REL_ARM_BRANCH24T:
    case llvm::COFF::IMAGE_REL_ARM_BLX23T:
      return {4, 2};
    case llvm::COFF::IMAGE_REL_ARM_MOV32T:
      return {8, 2};
    case llvm::COFF::IMAGE_REL_ARM_SECTION:
      return {2, 1};
    default:
      return {};
    }
  default:
    return {};
  }
}

llvm::Expected<RelocationShape>
verifyMachORelocationEncoding(const llvm::object::MachOObjectFile &Object,
                              const llvm::object::RelocationRef &Relocation,
                              llvm::StringRef SectionName) {
  const llvm::MachO::any_relocation_info Raw =
      Object.getRelocation(Relocation.getRawDataRefImpl());
  if (Object.isRelocationScattered(Raw))
    return failure(TranslationArtifactViolation::RelocationTypeNotAllowed,
                   SectionName, "scattered relocation");
  const uint64_t Type = Object.getAnyRelocationType(Raw);
  const unsigned Length = Object.getAnyRelocationLength(Raw);
  const bool PCRelative = Object.getAnyRelocationPCRel(Raw) != 0;
  RelocationShape Shape;
  switch (canonicalArchitecture(Object.getArch())) {
  case llvm::Triple::x86_64:
    if (Type == llvm::MachO::X86_64_RELOC_UNSIGNED)
      Shape = !PCRelative && Length == 3 ? RelocationShape{8, 1}
                                         : RelocationShape{};
    else if (Type == llvm::MachO::X86_64_RELOC_SIGNED ||
             Type == llvm::MachO::X86_64_RELOC_BRANCH ||
             Type == llvm::MachO::X86_64_RELOC_SIGNED_1 ||
             Type == llvm::MachO::X86_64_RELOC_SIGNED_2 ||
             Type == llvm::MachO::X86_64_RELOC_SIGNED_4)
      Shape =
          PCRelative && Length == 2 ? RelocationShape{4, 1} : RelocationShape{};
    break;
  case llvm::Triple::x86:
    if (Type == llvm::MachO::GENERIC_RELOC_VANILLA && Length <= 2)
      Shape = {static_cast<uint8_t>(uint8_t{1} << Length), 1};
    break;
  case llvm::Triple::aarch64:
    switch (Type) {
    case llvm::MachO::ARM64_RELOC_UNSIGNED:
      Shape = !PCRelative && Length == 3 ? RelocationShape{8, 1}
                                         : RelocationShape{};
      break;
    case llvm::MachO::ARM64_RELOC_BRANCH26:
    case llvm::MachO::ARM64_RELOC_PAGE21:
      Shape =
          PCRelative && Length == 2 ? RelocationShape{4, 4} : RelocationShape{};
      break;
    case llvm::MachO::ARM64_RELOC_PAGEOFF12:
      Shape = !PCRelative && Length == 2 ? RelocationShape{4, 4}
                                         : RelocationShape{};
      break;
    default:
      break;
    }
    break;
  case llvm::Triple::arm:
    if (Type == llvm::MachO::ARM_RELOC_VANILLA)
      Shape =
          Length <= 2
              ? RelocationShape{static_cast<uint8_t>(uint8_t{1} << Length), 1}
              : RelocationShape{};
    else if (Type == llvm::MachO::ARM_RELOC_BR24)
      Shape =
          PCRelative && Length == 2 ? RelocationShape{4, 4} : RelocationShape{};
    else if (Type == llvm::MachO::ARM_THUMB_RELOC_BR22)
      Shape =
          PCRelative && Length == 2 ? RelocationShape{4, 2} : RelocationShape{};
    break;
  default:
    break;
  }
  if (Shape.Width == 0)
    return failure(TranslationArtifactViolation::RelocationTypeNotAllowed,
                   SectionName, "invalid relocation encoding");
  return Shape;
}

RelocationShape relocationShape(const llvm::object::ObjectFile &Object,
                                uint64_t Type) {
  if (Object.isELF())
    return elfRelocationShape(Object.getArch(), Type);
  if (Object.isCOFF())
    return coffRelocationShape(Object.getArch(), Type);
  return {};
}

bool isLoadableSection(const llvm::object::ObjectFile &Object,
                       const llvm::object::SectionRef &Section) {
  if (Object.isELF())
    return llvm::object::ELFSectionRef(Section).getFlags() &
           llvm::ELF::SHF_ALLOC;
  if (const auto *COFF =
          llvm::dyn_cast<llvm::object::COFFObjectFile>(&Object)) {
    const llvm::object::coff_section *Raw = COFF->getCOFFSection(Section);
    return Raw && (Raw->Characteristics & (llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                                           llvm::COFF::IMAGE_SCN_MEM_READ |
                                           llvm::COFF::IMAGE_SCN_MEM_WRITE));
  }
  if (const auto *MachO =
          llvm::dyn_cast<llvm::object::MachOObjectFile>(&Object)) {
    const llvm::object::DataRefImpl Ref = Section.getRawDataRefImpl();
    const uint32_t Flags = MachO->is64Bit() ? MachO->getSection64(Ref).flags
                                            : MachO->getSection(Ref).flags;
    return !(Flags & llvm::MachO::S_ATTR_DEBUG) &&
           MachO->getSectionFinalSegmentName(Ref) != "__DWARF";
  }
  return false;
}

bool permitsExternalTarget(const llvm::object::ObjectFile &Object,
                           uint64_t Type) {
  if (!Object.isCOFF())
    return true;
  switch (canonicalArchitecture(Object.getArch())) {
  case llvm::Triple::x86_64:
    return Type != llvm::COFF::IMAGE_REL_AMD64_ADDR32NB &&
           Type != llvm::COFF::IMAGE_REL_AMD64_SECTION &&
           Type != llvm::COFF::IMAGE_REL_AMD64_SECREL;
  case llvm::Triple::x86:
    return Type != llvm::COFF::IMAGE_REL_I386_DIR32NB &&
           Type != llvm::COFF::IMAGE_REL_I386_SECTION &&
           Type != llvm::COFF::IMAGE_REL_I386_SECREL;
  case llvm::Triple::aarch64:
    return Type != llvm::COFF::IMAGE_REL_ARM64_ADDR32NB &&
           Type != llvm::COFF::IMAGE_REL_ARM64_SECTION &&
           Type != llvm::COFF::IMAGE_REL_ARM64_SECREL &&
           Type != llvm::COFF::IMAGE_REL_ARM64_SECREL_LOW12A &&
           Type != llvm::COFF::IMAGE_REL_ARM64_SECREL_HIGH12A &&
           Type != llvm::COFF::IMAGE_REL_ARM64_SECREL_LOW12L;
  case llvm::Triple::arm:
    return Type != llvm::COFF::IMAGE_REL_ARM_ADDR32NB &&
           Type != llvm::COFF::IMAGE_REL_ARM_SECTION &&
           Type != llvm::COFF::IMAGE_REL_ARM_SECREL;
  default:
    return false;
  }
}

llvm::Expected<bool>
verifyRelocationTarget(const llvm::object::ObjectFile &Object,
                       const llvm::object::RelocationRef &Relocation,
                       const llvm::StringSet<> &AllowedSymbols) {
  llvm::object::symbol_iterator Symbol = Relocation.getSymbol();
  if (Symbol == Object.symbol_end()) {
    const auto *MachO = llvm::dyn_cast<llvm::object::MachOObjectFile>(&Object);
    if (!MachO)
      return failure(TranslationArtifactViolation::RelocationTargetNotAllowed);
    const llvm::MachO::any_relocation_info Raw =
        MachO->getRelocation(Relocation.getRawDataRefImpl());
    if (MachO->isRelocationScattered(Raw) ||
        MachO->getPlainRelocationExternal(Raw))
      return failure(TranslationArtifactViolation::RelocationTargetNotAllowed);
    const llvm::object::SectionRef Target = MachO->getAnyRelocationSection(Raw);
    if (Target == *MachO->section_end())
      return failure(TranslationArtifactViolation::RelocationTargetNotAllowed);
    llvm::Expected<llvm::StringRef> NameOrErr = Target.getName();
    if (!NameOrErr)
      return malformed(NameOrErr.takeError(), "relocation target section");
    if (!isLoadableSection(Object, Target))
      return failure(TranslationArtifactViolation::RelocationTargetNotAllowed,
                     *NameOrErr, "target section is not loadable");
    return false;
  }

  llvm::Expected<llvm::StringRef> NameOrErr = Symbol->getName();
  if (!NameOrErr)
    return malformed(NameOrErr.takeError(), "relocation target name");
  llvm::Expected<uint32_t> FlagsOrErr = Symbol->getFlags();
  if (!FlagsOrErr)
    return malformed(FlagsOrErr.takeError(), *NameOrErr);
  if (*FlagsOrErr & llvm::object::SymbolRef::SF_Indirect)
    return failure(TranslationArtifactViolation::IndirectSymbol, *NameOrErr);
  if (*FlagsOrErr & llvm::object::SymbolRef::SF_Undefined) {
    if ((*FlagsOrErr & llvm::object::SymbolRef::SF_Weak) ||
        !AllowedSymbols.contains(*NameOrErr) ||
        !permitsExternalTarget(Object, Relocation.getType()))
      return failure(TranslationArtifactViolation::RelocationTargetNotAllowed,
                     *NameOrErr);
    return true;
  }
  if (*FlagsOrErr & (llvm::object::SymbolRef::SF_Absolute |
                     llvm::object::SymbolRef::SF_Common))
    return failure(TranslationArtifactViolation::RelocationTargetNotAllowed,
                   *NameOrErr);
  if (hasForbiddenDefinitionLinkage(Object, *Symbol, *FlagsOrErr))
    return failure(TranslationArtifactViolation::PreemptibleDefinition,
                   *NameOrErr);
  llvm::Expected<llvm::object::section_iterator> SectionOrErr =
      Symbol->getSection();
  if (!SectionOrErr)
    return malformed(SectionOrErr.takeError(), *NameOrErr);
  if (*SectionOrErr == Object.section_end())
    return failure(TranslationArtifactViolation::RelocationTargetNotAllowed,
                   *NameOrErr);
  if (!isLoadableSection(Object, **SectionOrErr))
    return failure(TranslationArtifactViolation::RelocationTargetNotAllowed,
                   *NameOrErr, "target section is not loadable");
  return false;
}

bool isX86DirectControlTransfer(llvm::StringRef Contents, uint64_t Offset) {
  if (Offset >= 1) {
    const uint8_t Opcode = static_cast<uint8_t>(Contents[Offset - 1]);
    if (Opcode == 0xe8 || Opcode == 0xe9)
      return true;
  }
  return Offset >= 2 && static_cast<uint8_t>(Contents[Offset - 2]) == 0x0f &&
         (static_cast<uint8_t>(Contents[Offset - 1]) & 0xf0) == 0x80;
}

bool hasRelocatedBytes(llvm::StringRef Contents, uint64_t Offset,
                       uint64_t Width) {
  return Offset <= Contents.size() && Width <= Contents.size() - Offset;
}

uint32_t relocatedInstruction32(llvm::StringRef Contents, uint64_t Offset) {
  return llvm::support::endian::read32le(Contents.data() + Offset);
}

bool isAArch64UnconditionalBranch(llvm::StringRef Contents, uint64_t Offset) {
  return hasRelocatedBytes(Contents, Offset, 4) &&
         (relocatedInstruction32(Contents, Offset) & 0x7c000000u) ==
             0x14000000u;
}

bool isAArch64ConditionalBranch(llvm::StringRef Contents, uint64_t Offset) {
  if (!hasRelocatedBytes(Contents, Offset, 4))
    return false;
  const uint32_t Instruction = relocatedInstruction32(Contents, Offset);
  return (Instruction & 0xff000010u) == 0x54000000u ||
         (Instruction & 0x7e000000u) == 0x34000000u;
}

bool isAArch64TestBranch(llvm::StringRef Contents, uint64_t Offset) {
  return hasRelocatedBytes(Contents, Offset, 4) &&
         (relocatedInstruction32(Contents, Offset) & 0x7e000000u) ==
             0x36000000u;
}

bool isARMBranch(llvm::StringRef Contents, uint64_t Offset) {
  return hasRelocatedBytes(Contents, Offset, 4) &&
         (relocatedInstruction32(Contents, Offset) & 0x0e000000u) ==
             0x0a000000u;
}

bool isThumbBranch(llvm::StringRef Contents, uint64_t Offset) {
  if (!hasRelocatedBytes(Contents, Offset, 4))
    return false;
  const uint16_t First =
      llvm::support::endian::read16le(Contents.data() + Offset);
  const uint16_t Second =
      llvm::support::endian::read16le(Contents.data() + Offset + 2);
  return (First & 0xf800u) == 0xf000u &&
         ((Second & 0xd000u) == 0x9000u || (Second & 0xc000u) == 0xc000u);
}

bool isHiddenUndefinedELFSymbol(const llvm::object::ObjectFile &Object,
                                const llvm::object::RelocationRef &Relocation) {
  const llvm::object::symbol_iterator Symbol = Relocation.getSymbol();
  if (Symbol == Object.symbol_end())
    return false;
  llvm::Expected<uint32_t> FlagsOrErr = Symbol->getFlags();
  if (!FlagsOrErr) {
    llvm::consumeError(FlagsOrErr.takeError());
    return false;
  }
  if (!(*FlagsOrErr & llvm::object::SymbolRef::SF_Undefined) ||
      !(*FlagsOrErr & llvm::object::SymbolRef::SF_Hidden))
    return false;
  constexpr uint8_t ELFSymbolVisibilityMask = 0x3;
  const uint8_t Visibility =
      llvm::object::ELFSymbolRef(*Symbol).getOther() & ELFSymbolVisibilityMask;
  return Visibility == llvm::ELF::STV_HIDDEN ||
         Visibility == llvm::ELF::STV_INTERNAL;
}

bool isDirectELFRuntimeHelperRelocation(
    const llvm::object::ObjectFile &Object,
    const llvm::object::RelocationRef &Relocation,
    llvm::StringRef RelocatedContents) {
  const uint64_t Type = Relocation.getType();
  const uint64_t Offset = Relocation.getOffset();
  switch (canonicalArchitecture(Object.getArch())) {
  case llvm::Triple::x86_64:
    if (Type == llvm::ELF::R_X86_64_PC32)
      return isX86DirectControlTransfer(RelocatedContents, Offset);
    // LLVM spells an external x86-64 call as PLT32 even under the static
    // relocation model.  A hidden undefined target cannot be interposed and
    // must resolve inside the sealed link graph, so this is still a direct
    // branch relocation rather than permission to create or use a PLT.
    return Type == llvm::ELF::R_X86_64_PLT32 &&
           isHiddenUndefinedELFSymbol(Object, Relocation) &&
           isX86DirectControlTransfer(RelocatedContents, Offset);
  case llvm::Triple::x86:
    return Type == llvm::ELF::R_386_PC32 &&
           isX86DirectControlTransfer(RelocatedContents, Offset);
  case llvm::Triple::aarch64:
    if (Type == llvm::ELF::R_AARCH64_CALL26 ||
        Type == llvm::ELF::R_AARCH64_JUMP26)
      return isAArch64UnconditionalBranch(RelocatedContents, Offset);
    if (Type == llvm::ELF::R_AARCH64_CONDBR19)
      return isAArch64ConditionalBranch(RelocatedContents, Offset);
    if (Type == llvm::ELF::R_AARCH64_TSTBR14)
      return isAArch64TestBranch(RelocatedContents, Offset);
    return false;
  case llvm::Triple::arm:
    if (Type == llvm::ELF::R_ARM_PC24 || Type == llvm::ELF::R_ARM_CALL ||
        Type == llvm::ELF::R_ARM_JUMP24)
      return isARMBranch(RelocatedContents, Offset);
    if (Type == llvm::ELF::R_ARM_THM_CALL ||
        Type == llvm::ELF::R_ARM_THM_JUMP24 ||
        Type == llvm::ELF::R_ARM_THM_JUMP19)
      return isThumbBranch(RelocatedContents, Offset);
    return false;
  default:
    return false;
  }
}

bool isDirectCOFFRuntimeHelperRelocation(
    const llvm::object::ObjectFile &Object,
    const llvm::object::RelocationRef &Relocation,
    llvm::StringRef RelocatedContents) {
  const uint64_t Type = Relocation.getType();
  const uint64_t Offset = Relocation.getOffset();
  switch (canonicalArchitecture(Object.getArch())) {
  case llvm::Triple::x86_64:
    return Type == llvm::COFF::IMAGE_REL_AMD64_REL32 &&
           isX86DirectControlTransfer(RelocatedContents, Offset);
  case llvm::Triple::x86:
    return Type == llvm::COFF::IMAGE_REL_I386_REL32 &&
           isX86DirectControlTransfer(RelocatedContents, Offset);
  case llvm::Triple::aarch64:
    if (Type == llvm::COFF::IMAGE_REL_ARM64_BRANCH26)
      return isAArch64UnconditionalBranch(RelocatedContents, Offset);
    if (Type == llvm::COFF::IMAGE_REL_ARM64_BRANCH19)
      return isAArch64ConditionalBranch(RelocatedContents, Offset);
    if (Type == llvm::COFF::IMAGE_REL_ARM64_BRANCH14)
      return isAArch64TestBranch(RelocatedContents, Offset);
    return false;
  case llvm::Triple::arm:
    if (Type == llvm::COFF::IMAGE_REL_ARM_BRANCH20T ||
        Type == llvm::COFF::IMAGE_REL_ARM_BRANCH24T ||
        Type == llvm::COFF::IMAGE_REL_ARM_BLX23T)
      return isThumbBranch(RelocatedContents, Offset);
    return false;
  default:
    return false;
  }
}

bool isDirectRuntimeHelperRelocation(
    const llvm::object::ObjectFile &Object,
    const llvm::object::RelocationRef &Relocation,
    const llvm::object::SectionRef &RelocatedSection,
    llvm::StringRef RelocatedContents) {
  if (!Object.isLittleEndian() ||
      !isExecutableSection(Object, RelocatedSection))
    return false;
  if (Object.isELF())
    return isDirectELFRuntimeHelperRelocation(Object, Relocation,
                                              RelocatedContents);
  if (Object.isCOFF())
    return isDirectCOFFRuntimeHelperRelocation(Object, Relocation,
                                               RelocatedContents);
  if (const auto *MachO =
          llvm::dyn_cast<llvm::object::MachOObjectFile>(&Object)) {
    const llvm::MachO::any_relocation_info Raw =
        MachO->getRelocation(Relocation.getRawDataRefImpl());
    if (MachO->isRelocationScattered(Raw) ||
        MachO->getAnyRelocationPCRel(Raw) == 0 ||
        MachO->getAnyRelocationLength(Raw) != 2)
      return false;
    switch (canonicalArchitecture(Object.getArch())) {
    case llvm::Triple::x86_64:
      return MachO->getAnyRelocationType(Raw) ==
                 llvm::MachO::X86_64_RELOC_BRANCH &&
             isX86DirectControlTransfer(RelocatedContents,
                                        Relocation.getOffset());
    case llvm::Triple::x86:
      return MachO->getAnyRelocationType(Raw) ==
                 llvm::MachO::GENERIC_RELOC_VANILLA &&
             isX86DirectControlTransfer(RelocatedContents,
                                        Relocation.getOffset());
    case llvm::Triple::aarch64:
      return MachO->getAnyRelocationType(Raw) ==
                 llvm::MachO::ARM64_RELOC_BRANCH26 &&
             isAArch64UnconditionalBranch(RelocatedContents,
                                          Relocation.getOffset());
    case llvm::Triple::arm:
      if (MachO->getAnyRelocationType(Raw) == llvm::MachO::ARM_RELOC_BR24)
        return isARMBranch(RelocatedContents, Relocation.getOffset());
      if (MachO->getAnyRelocationType(Raw) == llvm::MachO::ARM_THUMB_RELOC_BR22)
        return isThumbBranch(RelocatedContents, Relocation.getOffset());
      return false;
    default:
      return false;
    }
  }
  return false;
}

llvm::Error verifyRelocations(const llvm::object::ObjectFile &Object,
                              const llvm::StringSet<> &AllowedSymbols,
                              bool RequireDirectRuntimeCalls) {
  if (!Object.dynamic_relocation_sections().empty())
    return failure(TranslationArtifactViolation::DynamicRelocation);
  for (const llvm::object::SectionRef &Section : Object.sections()) {
    llvm::Expected<llvm::StringRef> NameOrErr = Section.getName();
    if (!NameOrErr)
      return malformed(NameOrErr.takeError(), "relocation section name");
    const auto Relocations = Section.relocations();
    if (Relocations.begin() == Relocations.end())
      continue;

    llvm::object::SectionRef RelocatedSection = Section;
    if (Object.isELF()) {
      const uint32_t RelocationSectionType =
          llvm::object::ELFSectionRef(Section).getType();
      const bool RequiresAddend =
          canonicalArchitecture(Object.getArch()) == llvm::Triple::x86_64 ||
          canonicalArchitecture(Object.getArch()) == llvm::Triple::aarch64;
      if ((RequiresAddend && RelocationSectionType != llvm::ELF::SHT_RELA) ||
          (!RequiresAddend && RelocationSectionType != llvm::ELF::SHT_REL))
        return failure(TranslationArtifactViolation::RelocationTypeNotAllowed,
                       *NameOrErr, "non-canonical ELF relocation encoding");
      llvm::Expected<llvm::object::section_iterator> RelocatedOrErr =
          Section.getRelocatedSection();
      if (!RelocatedOrErr)
        return malformed(RelocatedOrErr.takeError(), *NameOrErr);
      if (*RelocatedOrErr == Object.section_end())
        return failure(TranslationArtifactViolation::RelocationTargetNotAllowed,
                       *NameOrErr,
                       "relocation section has no object-local destination");
      RelocatedSection = **RelocatedOrErr;
    }
    if (!isLoadableSection(Object, RelocatedSection))
      return failure(TranslationArtifactViolation::RelocationTargetNotAllowed,
                     *NameOrErr, "relocation destination is not loadable");
    llvm::Expected<llvm::StringRef> RelocatedContentsOrErr =
        RelocatedSection.getContents();
    if (!RelocatedContentsOrErr)
      return malformed(RelocatedContentsOrErr.takeError(), *NameOrErr);
    const uint64_t RelocatedSize = RelocatedContentsOrErr->size();

    for (const llvm::object::RelocationRef &Relocation : Relocations) {
      const uint64_t Type = Relocation.getType();
      RelocationShape Shape;
      if (const auto *MachO =
              llvm::dyn_cast<llvm::object::MachOObjectFile>(&Object)) {
        llvm::Expected<RelocationShape> ShapeOrErr =
            verifyMachORelocationEncoding(*MachO, Relocation, *NameOrErr);
        if (!ShapeOrErr)
          return ShapeOrErr.takeError();
        Shape = *ShapeOrErr;
      } else {
        Shape = relocationShape(Object, Type);
        if (Shape.Width == 0)
          return failure(TranslationArtifactViolation::RelocationTypeNotAllowed,
                         *NameOrErr, llvm::utostr(Type));
      }
      if (Shape.Width > RelocatedSize ||
          Relocation.getOffset() > RelocatedSize - Shape.Width ||
          Relocation.getOffset() % Shape.Alignment != 0)
        return failure(TranslationArtifactViolation::MalformedObject,
                       *NameOrErr, "relocation offset or alignment is invalid");
      llvm::Expected<bool> RuntimeTargetOrErr =
          verifyRelocationTarget(Object, Relocation, AllowedSymbols);
      if (!RuntimeTargetOrErr)
        return RuntimeTargetOrErr.takeError();
      const bool IsX86ELFPLT32 =
          Object.isELF() &&
          canonicalArchitecture(Object.getArch()) == llvm::Triple::x86_64 &&
          Type == llvm::ELF::R_X86_64_PLT32;
      if (IsX86ELFPLT32 && (!*RuntimeTargetOrErr || !RequireDirectRuntimeCalls))
        return failure(TranslationArtifactViolation::RelocationTypeNotAllowed,
                       *NameOrErr,
                       "PLT32 is admitted only for a sealed runtime branch");
      if (*RuntimeTargetOrErr && RequireDirectRuntimeCalls &&
          !isDirectRuntimeHelperRelocation(Object, Relocation, RelocatedSection,
                                           *RelocatedContentsOrErr))
        return failure(TranslationArtifactViolation::RelocationTypeNotAllowed,
                       *NameOrErr,
                       "runtime helper requires a direct control transfer");
    }
  }
  return llvm::Error::success();
}

llvm::Error
verifyArtifact(llvm::MemoryBufferRef Artifact,
               const llvm::Triple &ExpectedHostTriple,
               llvm::ArrayRef<llvm::StringRef> AllowedRuntimeSymbols,
               llvm::ArrayRef<llvm::StringRef> RequiredBlockSymbols = {}) {
  llvm::StringSet<> AllowedSymbols;
  for (llvm::StringRef Name : AllowedRuntimeSymbols) {
    if (Name.empty() || Name.contains('\0') ||
        !AllowedSymbols.insert(Name).second)
      return failure(TranslationArtifactViolation::InvalidPolicy, Name);
  }
  llvm::StringSet<> RequiredSymbols;
  for (llvm::StringRef Name : RequiredBlockSymbols) {
    if (Name.empty() || Name.contains('\0') ||
        !RequiredSymbols.insert(Name).second || AllowedSymbols.contains(Name))
      return failure(TranslationArtifactViolation::InvalidPolicy, Name);
  }

  switch (llvm::identify_magic(Artifact.getBuffer())) {
  case llvm::file_magic::elf:
  case llvm::file_magic::elf_relocatable:
  case llvm::file_magic::elf_executable:
  case llvm::file_magic::elf_shared_object:
  case llvm::file_magic::macho_object:
  case llvm::file_magic::macho_executable:
  case llvm::file_magic::macho_fixed_virtual_memory_shared_lib:
  case llvm::file_magic::macho_core:
  case llvm::file_magic::macho_preload_executable:
  case llvm::file_magic::macho_dynamically_linked_shared_lib:
  case llvm::file_magic::macho_dynamic_linker:
  case llvm::file_magic::macho_bundle:
  case llvm::file_magic::macho_dynamically_linked_shared_lib_stub:
  case llvm::file_magic::macho_dsym_companion:
  case llvm::file_magic::macho_kext_bundle:
  case llvm::file_magic::macho_file_set:
  case llvm::file_magic::coff_object:
  case llvm::file_magic::pecoff_executable:
    break;
  case llvm::file_magic::unknown:
    return failure(TranslationArtifactViolation::MalformedObject,
                   Artifact.getBufferIdentifier());
  default:
    return failure(TranslationArtifactViolation::UnsupportedObjectFormat,
                   Artifact.getBufferIdentifier());
  }

  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> ObjectOrErr =
      llvm::object::ObjectFile::createObjectFile(Artifact);
  if (!ObjectOrErr)
    return malformed(ObjectOrErr.takeError(), Artifact.getBufferIdentifier());
  const llvm::object::ObjectFile &Object = **ObjectOrErr;
  const llvm::Triple::ObjectFormatType ActualFormat = objectFormat(Object);
  if (ActualFormat == llvm::Triple::UnknownObjectFormat)
    return failure(TranslationArtifactViolation::UnsupportedObjectFormat,
                   Object.getFileFormatName());
  const llvm::Triple::ObjectFormatType ExpectedFormat =
      ExpectedHostTriple.getObjectFormat();
  if (ExpectedFormat != llvm::Triple::ELF &&
      ExpectedFormat != llvm::Triple::COFF &&
      ExpectedFormat != llvm::Triple::MachO)
    return failure(TranslationArtifactViolation::UnsupportedObjectFormat,
                   ExpectedHostTriple.str());
  if (ActualFormat != ExpectedFormat)
    return failure(TranslationArtifactViolation::ObjectFormatMismatch,
                   Object.getFileFormatName(), ExpectedHostTriple.str());
  if (!isSupportedArchitecture(ExpectedHostTriple.getArch()) ||
      canonicalArchitecture(Object.getArch()) !=
          canonicalArchitecture(ExpectedHostTriple.getArch()) ||
      Object.isLittleEndian() != ExpectedHostTriple.isLittleEndian() ||
      Object.getBytesInAddress() !=
          (canonicalArchitecture(ExpectedHostTriple.getArch()) ==
                       llvm::Triple::x86 ||
                   canonicalArchitecture(ExpectedHostTriple.getArch()) ==
                       llvm::Triple::arm
               ? 4
               : 8))
    return failure(TranslationArtifactViolation::HostArchitectureMismatch,
                   Object.getFileFormatName(), ExpectedHostTriple.str());
  if (!Object.isRelocatableObject())
    return failure(TranslationArtifactViolation::UnsupportedArtifactKind,
                   Object.getFileFormatName());

  if (llvm::Error Error = verifySections(Object))
    return Error;
  if (llvm::Error Error = verifyRequiredBlockManifest(Object, RequiredSymbols,
                                                      RequiredBlockSymbols))
    return Error;
  if (llvm::Error Error = verifySymbols(Object, AllowedSymbols))
    return Error;
  return verifyRelocations(Object, AllowedSymbols, !RequiredSymbols.empty());
}

} // namespace

llvm::Error
verifyTranslationArtifact(llvm::MemoryBufferRef Artifact,
                          const llvm::Triple &ExpectedHostTriple,
                          const TranslationArtifactPolicyV1 &Policy) {
  if (Policy.RequiredBlockSymbols.empty())
    return failure(TranslationArtifactViolation::InvalidPolicy,
                   "RequiredBlockSymbols");
  return verifyArtifact(Artifact, ExpectedHostTriple,
                        Policy.AllowedRuntimeSymbols,
                        Policy.RequiredBlockSymbols);
}

llvm::Error
verifyTranslationArtifact(llvm::ArrayRef<uint8_t> ArtifactBytes,
                          const llvm::Triple &ExpectedHostTriple,
                          const TranslationArtifactPolicyV1 &Policy) {
  const llvm::StringRef Bytes(
      reinterpret_cast<const char *>(ArtifactBytes.data()),
      ArtifactBytes.size());
  return verifyTranslationArtifact(
      llvm::MemoryBufferRef(Bytes, "translation artifact"), ExpectedHostTriple,
      Policy);
}

llvm::Error verifyTranslationArtifact(
    llvm::MemoryBufferRef Artifact, const llvm::Triple &ExpectedHostTriple,
    llvm::ArrayRef<llvm::StringRef> AllowedRuntimeSymbols) {
  return verifyArtifact(Artifact, ExpectedHostTriple, AllowedRuntimeSymbols);
}

llvm::Error verifyTranslationArtifact(
    llvm::ArrayRef<uint8_t> ArtifactBytes,
    const llvm::Triple &ExpectedHostTriple,
    llvm::ArrayRef<llvm::StringRef> AllowedRuntimeSymbols) {
  const llvm::StringRef Bytes(
      reinterpret_cast<const char *>(ArtifactBytes.data()),
      ArtifactBytes.size());
  return verifyArtifact(llvm::MemoryBufferRef(Bytes, "translation artifact"),
                        ExpectedHostTriple, AllowedRuntimeSymbols);
}

} // namespace neverd::translate
