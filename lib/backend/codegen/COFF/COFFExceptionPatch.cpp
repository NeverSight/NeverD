//===- COFFExceptionPatch.cpp - Safe PE exception-table rewrite ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"

#include "neverd/object/PELayout.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/Win64EH.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

constexpr uint64_t X64RuntimeFunctionSize = 3 * sizeof(uint32_t);
constexpr uint64_t ARMRuntimeFunctionSize = 2 * sizeof(uint32_t);
constexpr uint32_t ReturnFlowGuardMask = 0x000e0000u;
constexpr uint32_t RetpolinePresent = 0x00100000u;
constexpr uint32_t XFGEnabled = 0x00800000u;

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "coff exception patch: " + Message);
}

bool hasUnsupportedGeneratedGuardMode(uint32_t GuardFlags) {
  const uint32_t Unsupported =
      uint32_t(llvm::COFF::GuardFlags::CFW_INSTRUMENTED) | ReturnFlowGuardMask |
      RetpolinePresent | XFGEnabled;
  return (GuardFlags & Unsupported) != 0;
}

std::optional<uint64_t> metadataUInt(const llvm::MDNode &Node, unsigned Index) {
  if (Index >= Node.getNumOperands())
    return std::nullopt;
  auto *CAM = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node.getOperand(Index).get());
  auto *CI = CAM ? llvm::dyn_cast<llvm::ConstantInt>(CAM->getValue()) : nullptr;
  if (!CI || CI->getBitWidth() > 64)
    return std::nullopt;
  return CI->getZExtValue();
}

std::optional<llvm::StringRef> metadataString(const llvm::MDNode &Node,
                                              unsigned Index) {
  if (Index >= Node.getNumOperands())
    return std::nullopt;
  auto *S =
      llvm::dyn_cast_or_null<llvm::MDString>(Node.getOperand(Index).get());
  if (!S)
    return std::nullopt;
  return S->getString();
}

std::optional<va_t> autoFunctionAddress(llvm::StringRef Name) {
  if (!Name.consume_front(kAutoFuncPrefix) || Name.empty())
    return std::nullopt;
  va_t Address = 0;
  if (Name.getAsInteger(16, Address))
    return std::nullopt;
  return Address;
}

const ExceptionFunction *findExactExceptionFunction(const BinaryImage &Image,
                                                    va_t Begin, va_t End) {
  for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions)
    if (EH.CodeRange.Begin == Begin && EH.CodeRange.End == End)
      return &EH;
  return nullptr;
}

bool hasNativeEHMarker(const llvm::Function &F, llvm::StringRef Kind) {
  const llvm::MDNode *Marker = F.getMetadata(windows_eh_md::NativeAttachment);
  return Marker && Marker->getNumOperands() == 2 &&
         metadataUInt(*Marker, 0).value_or(0) == 1 &&
         metadataString(*Marker, 1).value_or("") == Kind;
}

bool hasPersonality(const llvm::Function &F, llvm::StringRef Name) {
  if (!F.hasPersonalityFn())
    return false;
  const llvm::Value *Personality = F.getPersonalityFn()->stripPointerCasts();
  const auto *PersonalityFunction = llvm::dyn_cast<llvm::Function>(Personality);
  return PersonalityFunction && PersonalityFunction->getName() == Name;
}

bool hasNativeLanguageIRShape(const llvm::Function &F,
                              const ExceptionFunction &EH) {
  bool HasInvoke = false;
  bool HasCatchSwitch = false;
  bool HasCatchPad = false;
  bool HasCleanupPad = false;
  for (const llvm::BasicBlock &Block : F)
    for (const llvm::Instruction &Inst : Block) {
      HasInvoke |= llvm::isa<llvm::InvokeInst>(Inst);
      HasCatchSwitch |= llvm::isa<llvm::CatchSwitchInst>(Inst);
      HasCatchPad |= llvm::isa<llvm::CatchPadInst>(Inst);
      HasCleanupPad |= llvm::isa<llvm::CleanupPadInst>(Inst);
    }
  if (!HasInvoke)
    return false;
  if (EH.Personality == ExceptionPersonality::CxxFrameHandler3)
    return HasCatchSwitch && HasCatchPad;
  if (EH.Personality != ExceptionPersonality::CSpecificHandler || !EH.SEH)
    return false;
  const bool NeedsCatch =
      std::any_of(EH.SEH->Scopes.begin(), EH.SEH->Scopes.end(),
                  [](const SEHScopeRecord &Scope) {
                    return Scope.Kind != SEHScopeKind::Finally;
                  });
  const bool NeedsCleanup =
      std::any_of(EH.SEH->Scopes.begin(), EH.SEH->Scopes.end(),
                  [](const SEHScopeRecord &Scope) {
                    return Scope.Kind == SEHScopeKind::Finally;
                  });
  return (!NeedsCatch || (HasCatchSwitch && HasCatchPad)) &&
         (!NeedsCleanup || HasCleanupPad);
}

llvm::Error validateExceptionRecord(const ExceptionFunction &EH,
                                    Arch TargetArch,
                                    const llvm::Twine &Context) {
  if (EH.Kind != RuntimeFunctionKind::Primary)
    return patchError("function is not backed by a primary runtime record: " +
                      Context);
  if (EH.ParseStatus != ExceptionParseStatus::Complete)
    return patchError("incomplete exception metadata for " + Context);
  if (TargetArch == Arch::X64) {
    if (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
        EH.Encoding != ExceptionEncoding::X64UnwindV2)
      return patchError("unsupported unwind encoding " +
                        llvm::Twine(getExceptionEncodingName(EH.Encoding)) +
                        " in " + Context);
  } else if (TargetArch == Arch::AArch64) {
    if (EH.Encoding != ExceptionEncoding::ARM64Packed &&
        EH.Encoding != ExceptionEncoding::ARM64Unpacked)
      return patchError("unsupported ARM64 unwind encoding in " + Context);
  } else if (TargetArch == Arch::ARM) {
    if (EH.Encoding != ExceptionEncoding::ARM32Packed &&
        EH.Encoding != ExceptionEncoding::ARM32Unpacked)
      return patchError("unsupported ARM32 unwind encoding in " + Context);
  } else {
    return patchError("table-based exception regeneration is unsupported for " +
                      Context);
  }
  if (isGSWrappedPersonality(EH.Personality))
    return patchError("GS-wrapped language metadata cannot be regenerated "
                      "exactly for " +
                      Context);
  if (EH.Personality == ExceptionPersonality::CxxFrameHandler4)
    return patchError("FH4 language metadata is analysis-only for " + Context);
  if (EH.Personality == ExceptionPersonality::Unknown)
    return patchError("unknown Windows personality in " + Context);

  if (EH.Personality != ExceptionPersonality::None) {
    if (TargetArch != Arch::X64)
      return patchError("ARM language-handler reconstruction is not yet "
                        "native for " +
                        Context);
    if (!EH.canRegenerateLanguageMetadata())
      return patchError("language metadata failed regeneration checks for " +
                        Context);
  }
  return llvm::Error::success();
}

llvm::Error validateExceptionFunction(const llvm::Function &F,
                                      const ExceptionFunction &EH,
                                      Arch TargetArch) {
  if (llvm::Error Err = validateExceptionRecord(
          EH, TargetArch, llvm::Twine("function ") + F.getName()))
    return Err;

  if (EH.Personality != ExceptionPersonality::None) {
    llvm::StringRef NativeKind =
        EH.Personality == ExceptionPersonality::CSpecificHandler
            ? "seh-x64-native"
            : "cxx-fh3-native";
    llvm::StringRef PersonalityName =
        EH.Personality == ExceptionPersonality::CSpecificHandler
            ? "__C_specific_handler"
            : "__CxxFrameHandler3";
    if (!hasNativeEHMarker(F, NativeKind))
      return patchError("native WinEH lowering is unavailable for function " +
                        F.getName());
    if (!hasPersonality(F, PersonalityName) || !hasNativeLanguageIRShape(F, EH))
      return patchError("native WinEH IR contract was altered for function " +
                        F.getName());
  }
  return llvm::Error::success();
}

std::optional<size_t> rvaToFileOffset(const PEHeaderPtrs &PE,
                                      llvm::ArrayRef<uint8_t> Binary,
                                      uint32_t RVA, uint64_t Size) {
  uint32_t HeaderSize = getPESizeOfHeaders(PE);
  if (RVA < HeaderSize) {
    if (Size <= HeaderSize - RVA && rangeInBounds(RVA, Size, Binary.size()))
      return static_cast<size_t>(RVA);
    return std::nullopt;
  }

  std::optional<size_t> Result;
  forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
    if (Result || RVA < S.VirtualAddress)
      return;
    uint64_t Delta = uint64_t(RVA) - S.VirtualAddress;
    if (Delta > S.SizeOfRawData || Size > S.SizeOfRawData - Delta)
      return;
    uint64_t FileOffset = uint64_t(S.PointerToRawData) + Delta;
    if (!rangeInBounds(FileOffset, Size, Binary.size()))
      return;
    Result = static_cast<size_t>(FileOffset);
  });
  return Result;
}

struct RuntimeEntry {
  uint32_t Begin = 0;
  uint32_t End = 0;
  uint32_t Unwind = 0;
  uint32_t Handler = 0;
  bool HasLanguageHandler = false;
  std::array<uint32_t, 3> Words{};
  uint8_t WordCount = 0;

  friend bool operator==(const RuntimeEntry &A, const RuntimeEntry &B) {
    return A.WordCount == B.WordCount && A.Words == B.Words;
  }
};

template <typename ReadRVA>
llvm::Expected<RuntimeEntry> decodeRuntimeEntry(const uint8_t *Record,
                                                Arch TargetArch,
                                                ReadRVA &&ReadAtRVA) {
  RuntimeEntry Entry;
  if (TargetArch == Arch::X64) {
    Entry.Words = {readLE<uint32_t>(Record), readLE<uint32_t>(Record + 4),
                   readLE<uint32_t>(Record + 8)};
    Entry.WordCount = 3;
    Entry.Begin = Entry.Words[0];
    Entry.End = Entry.Words[1];
    Entry.Unwind = Entry.Words[2];

    const uint8_t *Header = ReadAtRVA(Entry.Unwind, 4);
    if (!Header)
      return patchError("x64 runtime-function entry references missing unwind "
                        "information");
    const uint8_t Version = Header[0] & 0x7u;
    const uint8_t Flags = Header[0] >> 3;
    const uint8_t HandlerFlags = llvm::Win64EH::UNW_ExceptionHandler |
                                 llvm::Win64EH::UNW_TerminateHandler;
    if ((Flags & HandlerFlags) != 0 &&
        (Flags & llvm::Win64EH::UNW_ChainInfo) != 0)
      return patchError("x64 unwind record combines handler and chain flags");
    if ((Version == 1 || Version == 2) && (Flags & HandlerFlags) != 0) {
      const uint64_t SlotCount = (uint64_t(Header[2]) + 1) & ~uint64_t(1);
      const uint64_t HandlerOffset = 4 + SlotCount * 2;
      if (HandlerOffset > std::numeric_limits<uint32_t>::max() - Entry.Unwind)
        return patchError("x64 language-handler location overflows");
      const uint8_t *Handler =
          ReadAtRVA(Entry.Unwind + static_cast<uint32_t>(HandlerOffset),
                    sizeof(uint32_t));
      if (!Handler)
        return patchError("x64 language-handler field is truncated");
      Entry.Handler = readLE<uint32_t>(Handler);
      if (Entry.Handler == 0)
        return patchError("x64 unwind record has a zero language handler");
      Entry.HasLanguageHandler = true;
    }
    return Entry;
  }
  if (TargetArch != Arch::AArch64 && TargetArch != Arch::ARM)
    return patchError("unsupported runtime-function architecture");

  Entry.Words = {readLE<uint32_t>(Record), readLE<uint32_t>(Record + 4), 0};
  Entry.WordCount = 2;
  Entry.Begin =
      TargetArch == Arch::ARM ? Entry.Words[0] & ~uint32_t(1) : Entry.Words[0];
  Entry.Unwind = Entry.Words[1];

  llvm::support::ulittle32_t NativeWords[2];
  NativeWords[0] = Entry.Words[0];
  NativeWords[1] = Entry.Words[1];
  llvm::ARM::WinEH::RuntimeFunctionFlag Flag;
  uint32_t Length = 0;
  uint32_t XDataRVA = 0;
  if (TargetArch == Arch::AArch64) {
    llvm::ARM::WinEH::RuntimeFunctionARM64 RF(NativeWords);
    Flag = RF.Flag();
    if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed ||
        Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment)
      Length = RF.FunctionLength();
    else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked)
      XDataRVA = RF.ExceptionInformationRVA();
  } else {
    llvm::ARM::WinEH::RuntimeFunction RF(NativeWords);
    Flag = RF.Flag();
    if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed ||
        Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment)
      Length = RF.FunctionLength();
    else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked)
      XDataRVA = RF.ExceptionInformationRVA();
  }
  if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Reserved)
    return patchError("ARM runtime-function entry uses reserved flags");

  if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked) {
    const uint8_t *Header = ReadAtRVA(XDataRVA, sizeof(uint32_t));
    if (!Header)
      return patchError("ARM runtime-function entry references missing xdata");
    uint32_t First = readLE<uint32_t>(Header);
    bool Extended = TargetArch == Arch::AArch64 ? (First & 0xffc00000u) == 0
                                                : (First & 0xff800000u) == 0;
    size_t HeaderBytes = Extended ? 8 : 4;
    Header = ReadAtRVA(XDataRVA, HeaderBytes);
    if (!Header)
      return patchError("ARM runtime-function entry has truncated xdata");
    llvm::support::ulittle32_t XDataWords[2] = {};
    std::memcpy(XDataWords, Header, HeaderBytes);
    llvm::ARM::WinEH::ExceptionDataRecord XR(XDataWords,
                                             TargetArch == Arch::AArch64);
    if (XR.Vers() != 0)
      return patchError("ARM xdata uses an unsupported version");
    Length = TargetArch == Arch::AArch64 ? XR.FunctionLengthInBytesAArch64()
                                         : XR.FunctionLengthInBytesARM();

    const uint64_t HeaderWords = Extended ? 2 : 1;
    const uint64_t EpilogueWords = XR.E() ? 0 : XR.EpilogueCount();
    const uint64_t CodeWords = XR.CodeWords();
    if (HeaderWords > std::numeric_limits<uint64_t>::max() - EpilogueWords ||
        HeaderWords + EpilogueWords >
            std::numeric_limits<uint64_t>::max() - CodeWords)
      return patchError("ARM xdata structural size overflows");
    const uint64_t PreHandlerWords = HeaderWords + EpilogueWords + CodeWords;
    const uint64_t StructuralWords = PreHandlerWords + (XR.X() ? 1 : 0);
    if (StructuralWords > std::numeric_limits<size_t>::max() / 4)
      return patchError("ARM xdata structural size exceeds host limits");
    const size_t StructuralBytes = static_cast<size_t>(StructuralWords * 4);
    const uint8_t *Body = ReadAtRVA(XDataRVA, StructuralBytes);
    if (!Body)
      return patchError("ARM xdata body is truncated");
    if (XR.X()) {
      Entry.Handler =
          readLE<uint32_t>(Body + static_cast<size_t>(PreHandlerWords * 4));
      if (Entry.Handler == 0)
        return patchError("ARM xdata has a zero language-handler RVA");
      Entry.HasLanguageHandler = true;
    }
  }
  if (Length == 0 ||
      Length > std::numeric_limits<uint32_t>::max() - Entry.Begin)
    return patchError("ARM runtime-function entry has an invalid length");
  Entry.End = Entry.Begin + Length;
  return Entry;
}

std::set<uint32_t>
collectSupersededRuntimeRecords(const BinaryImage &Image,
                                llvm::ArrayRef<va_t> PatchedEntries,
                                std::vector<ExceptionAddressRange> &Ranges) {
  const auto &Functions = Image.ExceptionMetadata.Functions;
  std::set<size_t> Indices;
  for (va_t Entry : PatchedEntries)
    for (size_t I = 0; I < Functions.size(); ++I)
      if (Functions[I].CodeRange.contains(Entry))
        Indices.insert(I);

  bool Changed = true;
  while (Changed) {
    Changed = false;
    std::vector<size_t> Snapshot(Indices.begin(), Indices.end());
    for (size_t I : Snapshot) {
      const ExceptionFunction &F = Functions[I];
      if (F.PrimaryFunctionIndex && *F.PrimaryFunctionIndex < Functions.size())
        Changed |= Indices.insert(*F.PrimaryFunctionIndex).second;
      for (size_t J = 0; J < Functions.size(); ++J) {
        const ExceptionFunction &Candidate = Functions[J];
        if (Candidate.PrimaryFunctionIndex &&
            *Candidate.PrimaryFunctionIndex == I)
          Changed |= Indices.insert(J).second;
        if (Candidate.ChainedPrimaryRange &&
            Candidate.ChainedPrimaryRange->Begin == F.CodeRange.Begin &&
            Candidate.ChainedPrimaryRange->End == F.CodeRange.End)
          Changed |= Indices.insert(J).second;
      }
    }
  }

  std::set<uint32_t> RuntimeRVAs;
  for (size_t I : Indices) {
    const ExceptionFunction &F = Functions[I];
    Ranges.push_back(F.CodeRange);
    if (F.RuntimeFunctionRVA != 0)
      RuntimeRVAs.insert(F.RuntimeFunctionRVA);
  }
  return RuntimeRVAs;
}

bool overlapsAny(const RuntimeEntry &Entry, uint64_t ImageBase,
                 llvm::ArrayRef<ExceptionAddressRange> Ranges) {
  if (Entry.End <= Entry.Begin || Entry.Begin > InvalidVA - ImageBase ||
      Entry.End > InvalidVA - ImageBase)
    return false;
  ExceptionAddressRange Native{ImageBase + Entry.Begin, ImageBase + Entry.End};
  return std::any_of(Ranges.begin(), Ranges.end(),
                     [&](const ExceptionAddressRange &Range) {
                       return Native.overlaps(Range);
                     });
}

llvm::Error validateRuntimeEntries(llvm::ArrayRef<RuntimeEntry> Entries,
                                   uint64_t FinalSizeOfImage, Arch TargetArch) {
  uint32_t PreviousEnd = 0;
  bool HavePrevious = false;
  for (const RuntimeEntry &Entry : Entries) {
    if (Entry.Begin >= Entry.End)
      return patchError("runtime-function entry has an empty/reversed range");
    if (Entry.End > FinalSizeOfImage)
      return patchError("runtime-function RVA leaves the patched image");
    if (TargetArch == Arch::X64) {
      if (Entry.Unwind >= FinalSizeOfImage)
        return patchError("unwind-info RVA leaves the patched image");
      if ((Entry.Unwind & 3u) != 0)
        return patchError("x64 unwind-info RVA is not four-byte aligned");
    } else if ((Entry.Unwind & 3u) == 0 && Entry.Unwind >= FinalSizeOfImage) {
      return patchError("ARM xdata RVA leaves the patched image");
    }
    if (HavePrevious && Entry.Begin < PreviousEnd)
      return patchError("runtime-function entries overlap after merging");
    PreviousEnd = Entry.End;
    HavePrevious = true;
  }
  return llvm::Error::success();
}

} // namespace

std::optional<va_t> findCOFFExceptionPersonalityVA(const BinaryImage &Image,
                                                   llvm::StringRef SymbolName) {
  SymbolName.consume_front("\01");
  for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions) {
    if (EH.Personality == ExceptionPersonality::None ||
        EH.Personality == ExceptionPersonality::Unknown ||
        EH.PersonalityVA == 0 ||
        SymbolName != getExceptionPersonalityName(EH.Personality))
      continue;
    const Segment *Target = Image.getSegmentFor(EH.PersonalityVA);
    if (Target && Target->isExecutable() && Image.readVA(EH.PersonalityVA, 1))
      return EH.PersonalityVA;
  }
  return std::nullopt;
}

llvm::Expected<COFFExceptionPatchPlan>
planCOFFExceptionPatch(const llvm::Module &Mod, const BinaryImage &Image,
                       Arch TargetArch) {
  COFFExceptionPatchPlan Plan;
  if (hasUnsupportedGeneratedGuardMode(Image.DynInfo.GuardFlags))
    return patchError(
        "input uses a guard instrumentation mode not supported by generated "
        "code");
  if (Image.ExceptionMetadata.ParseStatus == ExceptionParseStatus::Malformed)
    return patchError("input exception directory is malformed");

  std::set<va_t> Seen;
  for (const llvm::Function &F : Mod) {
    if (F.isDeclaration())
      continue;
    const llvm::MDNode *MD = F.getMetadata(windows_eh_md::FunctionAttachment);
    if (!MD) {
      // A manually supplied module can still target an auto-named lifted
      // function.  Refuse omission of an exception contract for that case.
      if (auto Address = autoFunctionAddress(F.getName())) {
        if (const ExceptionFunction *EH =
                Image.ExceptionMetadata.findFunction(*Address))
          return patchError("function " + F.getName() +
                            " omits its Windows EH metadata");
      }
      continue;
    }
    if (MD->getNumOperands() != windows_eh_md::OperandCount)
      return patchError("unsupported Windows EH metadata shape on function " +
                        F.getName());
    auto Version = metadataUInt(*MD, windows_eh_md::Version);
    auto Begin = metadataUInt(*MD, windows_eh_md::CodeBegin);
    auto End = metadataUInt(*MD, windows_eh_md::CodeEnd);
    auto Status = metadataString(*MD, windows_eh_md::ParseStatus);
    if (!Version || *Version != windows_eh_md::SchemaVersion || !Begin ||
        !End || !Status || *Status != "complete" || *Begin >= *End)
      return patchError("invalid/incomplete Windows EH metadata on function " +
                        F.getName());
    const ExceptionFunction *EH =
        findExactExceptionFunction(Image, *Begin, *End);
    if (!EH)
      return patchError("Windows EH metadata does not match the input image "
                        "for function " +
                        F.getName());
    if (llvm::Error Err = validateExceptionFunction(F, *EH, TargetArch))
      return std::move(Err);
    if (Seen.insert(*Begin).second) {
      Plan.ExceptionFunctionEntries.push_back(*Begin);
      if (EH->Personality != ExceptionPersonality::None)
        Plan.LanguageExceptionFunctionEntries.push_back(*Begin);
    }
  }
  if ((TargetArch == Arch::ARM || TargetArch == Arch::AArch64) &&
      !Plan.ExceptionFunctionEntries.empty() &&
      std::any_of(Image.ExceptionMetadata.Functions.begin(),
                  Image.ExceptionMetadata.Functions.end(),
                  [](const ExceptionFunction &EH) {
                    return EH.Kind == RuntimeFunctionKind::Fragment;
                  }))
    return patchError(
        "ARM image contains independently addressable function fragments; "
        "their host association is not provable for rewrite");
  return Plan;
}

llvm::Expected<COFFExceptionDirectoryUpdate> prepareCOFFExceptionDirectory(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled, llvm::ArrayRef<va_t> PatchedOriginalEntries,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch) {
  COFFExceptionDirectoryUpdate Update;
  if (OriginalBinary.empty())
    return patchError("empty input image");

  auto PE = locatePEHeaders(const_cast<uint8_t *>(OriginalBinary.data()),
                            OriginalBinary.size());
  if (!PE.valid())
    return patchError("invalid PE headers");
  const llvm::object::data_directory *Directory =
      getPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE);
  const bool HasOriginal = Directory &&
                           Directory->RelativeVirtualAddress != 0 &&
                           Directory->Size != 0;

  std::vector<const CompiledSection *> GeneratedPDataSections;
  for (const CompiledSection &Section : Compiled.Sections) {
    llvm::StringRef Name(Section.Name);
    if ((Name == ".pdata" || Name.starts_with(".pdata$")) && Section.Size != 0)
      GeneratedPDataSections.push_back(&Section);
  }

  std::vector<ExceptionAddressRange> SupersededRanges;
  std::set<uint32_t> SupersededRecords = collectSupersededRuntimeRecords(
      Image, PatchedOriginalEntries, SupersededRanges);
  const bool NeedsUpdate = !GeneratedPDataSections.empty() ||
                           !SupersededRecords.empty() ||
                           !SupersededRanges.empty();
  if (!NeedsUpdate)
    return Update;
  if (TargetArch != Arch::X64 && TargetArch != Arch::AArch64 &&
      TargetArch != Arch::ARM)
    return patchError("native exception-directory installation is unsupported "
                      "for this architecture");
  if (NewSectionVA < Image.Base)
    return patchError("new section precedes the PE image base");
  uint64_t NewSectionRVA = NewSectionVA - Image.Base;
  if (NewSectionRVA > std::numeric_limits<uint32_t>::max())
    return patchError("new section exceeds PE RVA limits");

  for (va_t PatchedEntry : PatchedOriginalEntries) {
    const ExceptionFunction *PatchedEH = nullptr;
    for (const ExceptionFunction &EH : Image.ExceptionMetadata.Functions) {
      if (EH.Kind == RuntimeFunctionKind::Primary &&
          EH.CodeRange.Begin == PatchedEntry) {
        PatchedEH = &EH;
        break;
      }
    }
    if (!PatchedEH) {
      if (Image.ExceptionMetadata.findFunction(PatchedEntry))
        return patchError("patched entry does not identify a primary runtime "
                          "function");
      continue;
    }
    if (llvm::Error Err = validateExceptionRecord(
            *PatchedEH, TargetArch,
            llvm::Twine("patched entry 0x") + llvm::utohexstr(PatchedEntry)))
      return std::move(Err);
  }
  const uint64_t RuntimeFunctionSize =
      TargetArch == Arch::X64 ? X64RuntimeFunctionSize : ARMRuntimeFunctionSize;

  auto ReadAtRVA = [&](uint32_t RVA, size_t Size) -> const uint8_t * {
    if (RVA >= NewSectionRVA) {
      uint64_t Offset = uint64_t(RVA) - NewSectionRVA;
      if (rangeInBounds(Offset, Size, Compiled.Bytes.size()))
        return Compiled.Bytes.data() + Offset;
    }
    auto FileOffset = rvaToFileOffset(PE, OriginalBinary, RVA, Size);
    return FileOffset ? OriginalBinary.data() + *FileOffset : nullptr;
  };

  std::vector<RuntimeEntry> Entries;
  if (HasOriginal) {
    if (Directory->Size % RuntimeFunctionSize != 0)
      return patchError("input exception directory is misaligned");
    auto TableOffset = rvaToFileOffset(
        PE, OriginalBinary, Directory->RelativeVirtualAddress, Directory->Size);
    if (!TableOffset)
      return patchError("input exception directory is not file-backed");
    size_t Count = Directory->Size / RuntimeFunctionSize;
    Entries.reserve(Count);
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Record =
          OriginalBinary.data() + *TableOffset + I * RuntimeFunctionSize;
      bool IsZero = true;
      for (uint64_t W = 0; W < RuntimeFunctionSize / 4; ++W)
        IsZero &= readLE<uint32_t>(Record + W * 4) == 0;
      if (IsZero)
        continue;
      auto EntryOrErr = decodeRuntimeEntry(Record, TargetArch, ReadAtRVA);
      if (!EntryOrErr)
        return EntryOrErr.takeError();
      RuntimeEntry Entry = *EntryOrErr;
      uint64_t RecordRVA =
          uint64_t(Directory->RelativeVirtualAddress) + I * RuntimeFunctionSize;
      if (RecordRVA <= std::numeric_limits<uint32_t>::max() &&
          SupersededRecords.count(static_cast<uint32_t>(RecordRVA)))
        continue;
      if (overlapsAny(Entry, Image.Base, SupersededRanges))
        continue;
      Entries.push_back(Entry);
    }
  }

  std::vector<RuntimeEntry> GeneratedEntries;
  for (const CompiledSection *GeneratedPData : GeneratedPDataSections) {
    if (!GeneratedPData->IsAllocated ||
        GeneratedPData->Offset > InvalidVA - NewSectionVA ||
        GeneratedPData->VA != NewSectionVA + GeneratedPData->Offset ||
        GeneratedPData->Alignment < alignof(uint32_t) ||
        (GeneratedPData->Alignment & (GeneratedPData->Alignment - 1)) != 0)
      return patchError("generated .pdata has invalid placement traits");
    if (GeneratedPData->Size % RuntimeFunctionSize != 0 ||
        !rangeInBounds(GeneratedPData->Offset, GeneratedPData->Size,
                       Compiled.Bytes.size()))
      return patchError("generated .pdata has an invalid extent");
    const uint8_t *Bytes = Compiled.Bytes.data() + GeneratedPData->Offset;
    size_t Count = GeneratedPData->Size / RuntimeFunctionSize;
    Entries.reserve(Entries.size() + Count);
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Record = Bytes + I * RuntimeFunctionSize;
      bool IsZero = true;
      for (uint64_t W = 0; W < RuntimeFunctionSize / 4; ++W)
        IsZero &= readLE<uint32_t>(Record + W * 4) == 0;
      if (IsZero)
        continue;
      auto EntryOrErr = decodeRuntimeEntry(Record, TargetArch, ReadAtRVA);
      if (!EntryOrErr)
        return EntryOrErr.takeError();
      GeneratedEntries.push_back(*EntryOrErr);
      Entries.push_back(*EntryOrErr);
    }
  }

  for (const auto &[OriginalVA, GeneratedVA] : PatchedEntryMappings) {
    auto EHIt =
        std::find_if(Image.ExceptionMetadata.Functions.begin(),
                     Image.ExceptionMetadata.Functions.end(),
                     [&](const ExceptionFunction &EH) {
                       return EH.Kind == RuntimeFunctionKind::Primary &&
                              EH.CodeRange.Begin == OriginalVA &&
                              EH.Personality != ExceptionPersonality::None;
                     });
    if (EHIt == Image.ExceptionMetadata.Functions.end())
      continue;
    if (GeneratedVA < Image.Base ||
        GeneratedVA - Image.Base > std::numeric_limits<uint32_t>::max())
      return patchError("generated language-EH entry exceeds PE RVA limits");
    uint32_t GeneratedRVA = static_cast<uint32_t>(GeneratedVA - Image.Base);
    bool Covered = std::any_of(
        GeneratedEntries.begin(), GeneratedEntries.end(),
        [&](const RuntimeEntry &Entry) {
          if (Entry.Begin > GeneratedRVA || GeneratedRVA >= Entry.End ||
              !Entry.HasLanguageHandler)
            return false;
          if (Entry.Handler > InvalidVA - Image.Base)
            return false;
          const va_t HandlerVA = Image.Base + Entry.Handler;
          return std::any_of(Image.ExceptionMetadata.Functions.begin(),
                             Image.ExceptionMetadata.Functions.end(),
                             [&](const ExceptionFunction &Candidate) {
                               return Candidate.Personality ==
                                          EHIt->Personality &&
                                      Candidate.PersonalityVA == HandlerVA;
                             });
        });
    if (!Covered)
      return patchError(
          "generated language-EH function has no matching language-handler "
          "runtime record");
  }

  std::sort(Entries.begin(), Entries.end(),
            [](const RuntimeEntry &A, const RuntimeEntry &B) {
              return std::tie(A.Begin, A.End, A.Words) <
                     std::tie(B.Begin, B.End, B.Words);
            });
  Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());

  uint64_t TableOffset = alignUp(Compiled.Bytes.size(), uint64_t(4));
  uint64_t TableBytes = uint64_t(Entries.size()) * RuntimeFunctionSize;
  if (TableOffset > std::numeric_limits<size_t>::max() ||
      TableBytes > std::numeric_limits<size_t>::max() - TableOffset)
    return patchError("replacement exception directory exceeds host limits");
  if (TableOffset > std::numeric_limits<uint32_t>::max() - NewSectionRVA ||
      TableBytes >
          std::numeric_limits<uint32_t>::max() - (NewSectionRVA + TableOffset))
    return patchError("replacement exception directory exceeds PE RVA limits");
  uint64_t FinalVirtualSize = NewSectionRVA + TableOffset + TableBytes;
  if (llvm::Error Err =
          validateRuntimeEntries(Entries, FinalVirtualSize, TargetArch))
    return std::move(Err);

  Update.Apply = true;
  if (Entries.empty()) {
    Update.RVA = 0;
    Update.Size = 0;
    return Update;
  }

  Compiled.Bytes.resize(static_cast<size_t>(TableOffset + TableBytes), 0);
  uint8_t *Out = Compiled.Bytes.data() + TableOffset;
  for (const RuntimeEntry &Entry : Entries) {
    if (Entry.WordCount != RuntimeFunctionSize / 4)
      return patchError("runtime-function serialization width changed");
    for (uint8_t I = 0; I < Entry.WordCount; ++I)
      writeLE<uint32_t>(Out + uint64_t(I) * 4, Entry.Words[I]);
    Out += RuntimeFunctionSize;
  }
  Compiled.Sections.push_back(
      {".ndpdata", TableOffset, NewSectionVA + TableOffset, TableBytes, 4,
       llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData, true});
  Update.RVA = static_cast<uint32_t>(NewSectionRVA + TableOffset);
  Update.Size = static_cast<uint32_t>(TableBytes);
  return Update;
}

llvm::Error
applyCOFFExceptionDirectoryUpdate(std::vector<uint8_t> &Binary,
                                  const COFFExceptionDirectoryUpdate &Update) {
  if (!Update.Apply)
    return llvm::Error::success();
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (!PE.valid())
    return patchError("patched image has invalid PE headers");
  if (!getPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE))
    return patchError("patched image has no exception-directory slot");
  setPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE, Update.RVA, Update.Size);
  clearPEChecksum(PE);
  return llvm::Error::success();
}

llvm::Expected<COFFGuardTableUpdate> prepareCOFFGuardTables(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch,
    bool RequireGeneratedEHContinuations) {
  COFFGuardTableUpdate Update;
  if (OriginalBinary.empty())
    return patchError("empty input image while rebuilding guard tables");
  auto PE = locatePEHeaders(const_cast<uint8_t *>(OriginalBinary.data()),
                            OriginalBinary.size());
  if (!PE.valid())
    return patchError("invalid PE headers while rebuilding guard tables");
  if (NewSectionVA < Image.Base ||
      NewSectionVA - Image.Base > std::numeric_limits<uint32_t>::max())
    return patchError("guard table placement exceeds PE RVA limits");
  if ((TargetArch == Arch::X64 || TargetArch == Arch::AArch64) != PE.Is64)
    return patchError(
        "guard table PE class does not match target architecture");

  const uint32_t GuardFlags = Image.DynInfo.GuardFlags;
  const uint32_t CFPresent =
      uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  const uint32_t EHPresent =
      uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT);
  const bool HasCF = (GuardFlags & CFPresent) != 0;
  const bool HasEHCont = (GuardFlags & EHPresent) != 0;

  // These modes require code-generation contracts beyond ordinary CFG/EHCont
  // tables (write guard, return-flow guard, retpolines, or XFG hashes).  Keep
  // their existing metadata intact for analysis, but never advertise them on
  // newly generated code until the corresponding backend contracts are
  // represented and verified.
  if (!PatchedEntryMappings.empty() &&
      hasUnsupportedGeneratedGuardMode(GuardFlags))
    return patchError(
        "input uses a guard instrumentation mode not supported by generated "
        "code");
  if (!HasCF && !HasEHCont)
    return Update;

  if (HasCF) {
    for (const std::string &Unresolved : Compiled.Unresolved) {
      llvm::StringRef Name(Unresolved);
      Name.consume_front("\01");
      Name.consume_front("__imp_");
      if (Name == "__guard_check_icall_fptr" ||
          Name == "__guard_dispatch_icall_fptr" ||
          Name == "__guard_xfg_check_icall_fptr" ||
          Name == "__guard_xfg_dispatch_icall_fptr")
        return patchError("generated CFG instrumentation has an unresolved " +
                          llvm::Twine(Unresolved));
    }
  }

  const bool HasGeneratedCFReferences = std::any_of(
      Compiled.Sections.begin(), Compiled.Sections.end(),
      [](const CompiledSection &Section) {
        return llvm::StringRef(Section.Name).starts_with(".gfids") &&
               !Section.SymbolIndexReferences.empty();
      });

  const uint64_t NewSectionRVA = NewSectionVA - Image.Base;
  auto ToRVA = [&](va_t VA) -> llvm::Expected<uint32_t> {
    if (VA < Image.Base ||
        VA - Image.Base > std::numeric_limits<uint32_t>::max())
      return patchError("generated guard target exceeds PE RVA limits");
    return static_cast<uint32_t>(VA - Image.Base);
  };
  auto IsOriginalExecutableRVA = [&](uint32_t RVA) {
    if (RVA >= getPESizeOfImage(PE) || RVA > InvalidVA - Image.Base)
      return false;
    const Segment *Segment = Image.getSegmentFor(Image.Base + RVA);
    return Segment && Segment->isExecutable();
  };
  auto IsGeneratedCodeVA = [&](va_t VA) {
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!Section.IsAllocated ||
          Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code ||
          VA < Section.VA)
        continue;
      if (VA - Section.VA < Section.Size)
        return true;
    }
    return false;
  };
  auto IsGuardCodeVA = [&](va_t VA) {
    if (IsGeneratedCodeVA(VA))
      return true;
    if (VA < Image.Base ||
        VA - Image.Base > std::numeric_limits<uint32_t>::max())
      return false;
    return IsOriginalExecutableRVA(static_cast<uint32_t>(VA - Image.Base));
  };
  auto AppendTable =
      [&](llvm::StringRef Name,
          llvm::ArrayRef<uint8_t> Bytes) -> llvm::Expected<uint32_t> {
    uint64_t Offset = alignUp(Compiled.Bytes.size(), uint64_t(4));
    if (Offset > std::numeric_limits<size_t>::max() ||
        Bytes.size() > std::numeric_limits<size_t>::max() - Offset ||
        Offset > std::numeric_limits<uint32_t>::max() - NewSectionRVA ||
        Bytes.size() >
            std::numeric_limits<uint32_t>::max() - (NewSectionRVA + Offset))
      return patchError("replacement guard table exceeds PE RVA limits");
    Compiled.Bytes.resize(static_cast<size_t>(Offset), 0);
    Compiled.Bytes.insert(Compiled.Bytes.end(), Bytes.begin(), Bytes.end());
    Compiled.Sections.push_back(
        {Name.str(), Offset, NewSectionVA + Offset,
         static_cast<uint64_t>(Bytes.size()), 4,
         llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData, true});
    return static_cast<uint32_t>(NewSectionRVA + Offset);
  };

  if (HasCF && (!PatchedEntryMappings.empty() || HasGeneratedCFReferences)) {
    uint32_t ExtraBytes =
        (GuardFlags &
         uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_SIZE_MASK)) >>
        28;
    uint32_t Stride = 4 + ExtraBytes;
    std::map<uint32_t, std::vector<uint8_t>> Entries;
    uint64_t OriginalCount = Image.DynInfo.GuardCFFunctionCount;
    if (OriginalCount != 0) {
      if (Image.DynInfo.GuardCFFunctionTableRVA == 0 ||
          Image.DynInfo.GuardCFFunctionTableRVA >
              std::numeric_limits<uint32_t>::max() ||
          OriginalCount > OriginalBinary.size() / Stride)
        return patchError("invalid Guard CF function-table extent");
      uint64_t Bytes = OriginalCount * Stride;
      auto Offset = rvaToFileOffset(
          PE, OriginalBinary,
          static_cast<uint32_t>(Image.DynInfo.GuardCFFunctionTableRVA), Bytes);
      if (!Offset)
        return patchError("Guard CF function table is not file-backed");
      for (uint64_t I = 0; I < OriginalCount; ++I) {
        const uint8_t *Record = OriginalBinary.data() + *Offset + I * Stride;
        uint32_t RVA = readLE<uint32_t>(Record);
        if (!IsOriginalExecutableRVA(RVA))
          return patchError("Guard CF table contains a non-executable target");
        Entries.emplace(RVA, std::vector<uint8_t>(Record, Record + Stride));
      }
    }
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!llvm::StringRef(Section.Name).starts_with(".gfids"))
        continue;
      for (const auto &Reference : Section.SymbolIndexReferences) {
        if (!rangeInBounds(Reference.Offset, sizeof(uint32_t), Section.Size))
          return patchError("generated Guard CF record is truncated");
        if (Reference.TargetVA == 0 || !IsGuardCodeVA(Reference.TargetVA))
          return patchError("generated Guard CF reference is not code");
        auto RVAOrErr = ToRVA(Reference.TargetVA);
        if (!RVAOrErr)
          return RVAOrErr.takeError();
        std::vector<uint8_t> Record(Stride, 0);
        writeLE<uint32_t>(Record.data(), *RVAOrErr);
        Entries.emplace(*RVAOrErr, std::move(Record));
      }
    }
    for (const auto &[OriginalVA, GeneratedVA] : PatchedEntryMappings) {
      (void)OriginalVA;
      if (!IsGeneratedCodeVA(GeneratedVA))
        return patchError("generated Guard CF target is not emitted code");
      auto RVAOrErr = ToRVA(GeneratedVA);
      if (!RVAOrErr)
        return RVAOrErr.takeError();
      std::vector<uint8_t> Record(Stride, 0);
      writeLE<uint32_t>(Record.data(), *RVAOrErr);
      Entries.emplace(*RVAOrErr, std::move(Record));
    }
    std::vector<uint8_t> Bytes;
    Bytes.reserve(Entries.size() * Stride);
    for (const auto &[RVA, Record] : Entries) {
      (void)RVA;
      Bytes.insert(Bytes.end(), Record.begin(), Record.end());
    }
    auto RVAOrErr = AppendTable(".ndgfids", Bytes);
    if (!RVAOrErr)
      return RVAOrErr.takeError();
    Update.ApplyCF = true;
    Update.CFFunctionTableRVA = *RVAOrErr;
    Update.CFFunctionCount = Entries.size();
  }

  if (HasEHCont) {
    std::set<uint32_t> Entries;
    uint64_t OriginalCount = Image.DynInfo.GuardEHContinuationCount;
    if (OriginalCount != 0) {
      if (Image.DynInfo.GuardEHContinuationTableRVA == 0 ||
          Image.DynInfo.GuardEHContinuationTableRVA >
              std::numeric_limits<uint32_t>::max() ||
          OriginalCount > OriginalBinary.size() / sizeof(uint32_t))
        return patchError("invalid Guard EH continuation-table extent");
      uint64_t Bytes = OriginalCount * sizeof(uint32_t);
      auto Offset = rvaToFileOffset(
          PE, OriginalBinary,
          static_cast<uint32_t>(Image.DynInfo.GuardEHContinuationTableRVA),
          Bytes);
      if (!Offset)
        return patchError("Guard EH continuation table is not file-backed");
      for (uint64_t I = 0; I < OriginalCount; ++I) {
        uint32_t RVA = readLE<uint32_t>(OriginalBinary.data() + *Offset +
                                        I * sizeof(uint32_t));
        if (!IsOriginalExecutableRVA(RVA))
          return patchError(
              "Guard EH continuation table has a non-executable target");
        Entries.insert(RVA);
      }
    }

    size_t GeneratedCount = 0;
    for (const CompiledSection &Section : Compiled.Sections) {
      if (!llvm::StringRef(Section.Name).starts_with(".gehcont"))
        continue;
      for (const auto &Reference : Section.SymbolIndexReferences) {
        if (!rangeInBounds(Reference.Offset, sizeof(uint32_t), Section.Size))
          return patchError(
              "generated Guard EH continuation record is truncated");
        if (Reference.TargetVA == 0 || !IsGeneratedCodeVA(Reference.TargetVA))
          return patchError("generated Guard EH continuation is not code");
        auto RVAOrErr = ToRVA(Reference.TargetVA);
        if (!RVAOrErr)
          return RVAOrErr.takeError();
        Entries.insert(*RVAOrErr);
        ++GeneratedCount;
      }
    }
    if (RequireGeneratedEHContinuations && GeneratedCount == 0)
      return patchError(
          "native language EH emitted no Guard EH continuation targets");
    if (GeneratedCount != 0) {
      std::vector<uint8_t> Bytes(Entries.size() * sizeof(uint32_t));
      size_t I = 0;
      for (uint32_t RVA : Entries)
        writeLE<uint32_t>(Bytes.data() + I++ * sizeof(uint32_t), RVA);
      auto RVAOrErr = AppendTable(".ndgehcont", Bytes);
      if (!RVAOrErr)
        return RVAOrErr.takeError();
      Update.ApplyEHCont = true;
      Update.EHContinuationTableRVA = *RVAOrErr;
      Update.EHContinuationCount = Entries.size();
    }
  }
  return Update;
}

llvm::Error applyCOFFGuardTableUpdate(std::vector<uint8_t> &Binary,
                                      const BinaryImage &Image,
                                      const COFFGuardTableUpdate &Update) {
  if (!Update.ApplyCF && !Update.ApplyEHCont)
    return llvm::Error::success();
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (!PE.valid())
    return patchError("patched image has invalid PE headers for load config");
  if (Image.DynInfo.LoadConfigRVA == 0 ||
      Image.DynInfo.LoadConfigRVA > std::numeric_limits<uint32_t>::max() ||
      Image.DynInfo.LoadConfigSize == 0)
    return patchError("guard-enabled image has no writable load config");
  auto ConfigOffset = rvaToFileOffset(
      PE, Binary, static_cast<uint32_t>(Image.DynInfo.LoadConfigRVA),
      Image.DynInfo.LoadConfigSize);
  if (!ConfigOffset)
    return patchError("load configuration is not file-backed");

  auto WritePair = [&](size_t TableOffset, size_t CountOffset,
                       uint32_t TableRVA, uint64_t Count) -> llvm::Error {
    size_t Width = PE.Is64 ? sizeof(uint64_t) : sizeof(uint32_t);
    if (!rangeInBounds(TableOffset, Width, Image.DynInfo.LoadConfigSize) ||
        !rangeInBounds(CountOffset, Width, Image.DynInfo.LoadConfigSize) ||
        !rangeInBounds(*ConfigOffset + TableOffset, Width, Binary.size()) ||
        !rangeInBounds(*ConfigOffset + CountOffset, Width, Binary.size()))
      return patchError("load configuration is too short for guard fields");
    if (TableRVA > InvalidVA - Image.Base)
      return patchError("guard table VA overflows");
    uint64_t TableVA = Image.Base + TableRVA;
    if (PE.Is64) {
      writeLE<uint64_t>(Binary.data() + *ConfigOffset + TableOffset, TableVA);
      writeLE<uint64_t>(Binary.data() + *ConfigOffset + CountOffset, Count);
    } else {
      if (TableVA > std::numeric_limits<uint32_t>::max() ||
          Count > std::numeric_limits<uint32_t>::max())
        return patchError("32-bit guard load-config field overflows");
      writeLE<uint32_t>(Binary.data() + *ConfigOffset + TableOffset,
                        static_cast<uint32_t>(TableVA));
      writeLE<uint32_t>(Binary.data() + *ConfigOffset + CountOffset,
                        static_cast<uint32_t>(Count));
    }
    return llvm::Error::success();
  };

  using llvm::object::coff_load_configuration32;
  using llvm::object::coff_load_configuration64;
  if (Update.ApplyCF) {
    size_t TableOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardCFFunctionTable)
                : offsetof(coff_load_configuration32, GuardCFFunctionTable);
    size_t CountOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardCFFunctionCount)
                : offsetof(coff_load_configuration32, GuardCFFunctionCount);
    if (llvm::Error Err =
            WritePair(TableOffset, CountOffset, Update.CFFunctionTableRVA,
                      Update.CFFunctionCount))
      return Err;
  }
  if (Update.ApplyEHCont) {
    size_t TableOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardEHContinuationTable)
                : offsetof(coff_load_configuration32, GuardEHContinuationTable);
    size_t CountOffset =
        PE.Is64 ? offsetof(coff_load_configuration64, GuardEHContinuationCount)
                : offsetof(coff_load_configuration32, GuardEHContinuationCount);
    if (llvm::Error Err =
            WritePair(TableOffset, CountOffset, Update.EHContinuationTableRVA,
                      Update.EHContinuationCount))
      return Err;
  }
  clearPEChecksum(PE);
  return llvm::Error::success();
}

llvm::Error validatePatchedCOFFImage(llvm::ArrayRef<uint8_t> Binary,
                                     Arch TargetArch) {
  if (Binary.empty())
    return patchError("final PE image is empty");
  llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Binary.data()),
                      Binary.size()),
      "neverd-patched-pe");
  auto ObjectOrErr = llvm::object::ObjectFile::createObjectFile(Buffer);
  if (!ObjectOrErr)
    return patchError("LLVM rejected final PE image: " +
                      llvm::toString(ObjectOrErr.takeError()));
  const auto *COFF =
      llvm::dyn_cast<llvm::object::COFFObjectFile>(ObjectOrErr->get());
  if (!COFF)
    return patchError("final image is not a COFF object");
  uint16_t ExpectedMachine = 0;
  switch (TargetArch) {
  case Arch::X64:
    ExpectedMachine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
    break;
  case Arch::AArch64:
    ExpectedMachine = llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
    break;
  case Arch::ARM:
    ExpectedMachine = llvm::COFF::IMAGE_FILE_MACHINE_ARMNT;
    break;
  default:
    return patchError("final PE target architecture is unsupported");
  }
  if (COFF->getMachine() != ExpectedMachine)
    return patchError("final PE machine does not match target architecture");

  auto PE =
      locatePEHeaders(const_cast<uint8_t *>(Binary.data()), Binary.size());
  if (!PE.valid())
    return patchError("final PE headers are invalid");
  if (!PE.SectionTable || PE.NumSections == 0)
    return patchError("final PE has no valid section table");
  if ((TargetArch == Arch::X64 || TargetArch == Arch::AArch64) != PE.Is64)
    return patchError("final PE class does not match target architecture");
  uint64_t ImageBase = getPEImageBase(PE);
  uint32_t SizeOfImage = getPESizeOfImage(PE);
  uint32_t SizeOfHeaders = getPESizeOfHeaders(PE);
  if (ImageBase == 0 || SizeOfImage == 0 || SizeOfHeaders == 0 ||
      SizeOfHeaders > Binary.size() || SizeOfImage > InvalidVA - ImageBase)
    return patchError("final PE has an invalid image extent");

  std::vector<std::pair<uint64_t, uint64_t>> VirtualSectionRanges;
  std::vector<std::pair<uint64_t, uint64_t>> RawSectionRanges;
  bool InvalidSection = false;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    uint64_t VirtualExtent =
        std::max<uint32_t>(Section.VirtualSize, Section.SizeOfRawData);
    if (VirtualExtent != 0) {
      if (Section.VirtualAddress >= SizeOfImage ||
          VirtualExtent > SizeOfImage - Section.VirtualAddress) {
        InvalidSection = true;
      } else {
        VirtualSectionRanges.emplace_back(Section.VirtualAddress,
                                          uint64_t(Section.VirtualAddress) +
                                              VirtualExtent);
      }
    }
    if (Section.SizeOfRawData != 0) {
      if (Section.PointerToRawData < SizeOfHeaders ||
          !rangeInBounds(Section.PointerToRawData, Section.SizeOfRawData,
                         Binary.size())) {
        InvalidSection = true;
      } else {
        RawSectionRanges.emplace_back(Section.PointerToRawData,
                                      uint64_t(Section.PointerToRawData) +
                                          Section.SizeOfRawData);
      }
    }
  });
  auto HasOverlap = [](auto &Ranges) {
    std::sort(Ranges.begin(), Ranges.end());
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return true;
    return false;
  };
  if (InvalidSection || HasOverlap(VirtualSectionRanges) ||
      HasOverlap(RawSectionRanges))
    return patchError("final PE section extents are invalid or overlapping");

  BinaryImage ValidationImage;
  ValidationImage.Base = ImageBase;
  ValidationImage.Arch = TargetArch;
  ValidationImage.Bits = PE.Is64 ? Bitness::Bits64 : Bitness::Bits32;
  ValidationImage.Format = BinaryFormat::COFF;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    uint64_t VirtualExtent =
        std::max<uint32_t>(Section.VirtualSize, Section.SizeOfRawData);
    if (VirtualExtent == 0 || Section.VirtualAddress > InvalidVA - ImageBase)
      return;
    Segment Mapped;
    Mapped.VA = ImageBase + Section.VirtualAddress;
    Mapped.Size = VirtualExtent;
    Mapped.FileOff = Section.PointerToRawData;
    Mapped.FileSz = Section.SizeOfRawData;
    Mapped.Flags = coffFlagsToNd(Section.Characteristics);
    if (Section.SizeOfRawData != 0)
      Mapped.Data.assign(Binary.begin() + Section.PointerToRawData,
                         Binary.begin() + Section.PointerToRawData +
                             Section.SizeOfRawData);
    ValidationImage.Segments.push_back(std::move(Mapped));
  });

  auto ReadAtRVA = [&](uint32_t RVA, size_t Size) -> const uint8_t * {
    auto Offset = rvaToFileOffset(PE, Binary, RVA, Size);
    return Offset ? Binary.data() + *Offset : nullptr;
  };
  auto IsExecutableRVA = [&](uint32_t RVA) {
    if (RVA >= SizeOfImage)
      return false;
    bool Executable = false;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
      if (RVA < Section.VirtualAddress)
        return;
      uint64_t Delta = uint64_t(RVA) - Section.VirtualAddress;
      uint64_t Extent =
          std::max<uint32_t>(Section.VirtualSize, Section.SizeOfRawData);
      if (Delta < Extent &&
          (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) != 0)
        Executable = true;
    });
    return Executable;
  };

  const llvm::object::data_directory *ExceptionDirectory =
      getPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE);
  if (ExceptionDirectory && ExceptionDirectory->Size != 0) {
    uint64_t RecordSize = TargetArch == Arch::X64 ? X64RuntimeFunctionSize
                                                  : ARMRuntimeFunctionSize;
    if (TargetArch != Arch::X64 && TargetArch != Arch::AArch64 &&
        TargetArch != Arch::ARM)
      return patchError("final exception table uses unsupported architecture");
    if (ExceptionDirectory->RelativeVirtualAddress == 0 ||
        ExceptionDirectory->Size % RecordSize != 0)
      return patchError("final exception directory has an invalid extent");
    if (ExceptionDirectory->RelativeVirtualAddress >= SizeOfImage ||
        ExceptionDirectory->Size >
            SizeOfImage - ExceptionDirectory->RelativeVirtualAddress)
      return patchError("final exception directory leaves the image extent");
    auto TableOffset =
        rvaToFileOffset(PE, Binary, ExceptionDirectory->RelativeVirtualAddress,
                        ExceptionDirectory->Size);
    if (!TableOffset)
      return patchError("final exception directory is not file-backed");

    std::vector<RuntimeEntry> Entries;
    size_t Count = ExceptionDirectory->Size / RecordSize;
    Entries.reserve(Count);
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Record = Binary.data() + *TableOffset + I * RecordSize;
      auto EntryOrErr = decodeRuntimeEntry(Record, TargetArch, ReadAtRVA);
      if (!EntryOrErr)
        return EntryOrErr.takeError();
      Entries.push_back(*EntryOrErr);
      if (EntryOrErr->Begin >= EntryOrErr->End ||
          !IsExecutableRVA(EntryOrErr->Begin) ||
          !IsExecutableRVA(EntryOrErr->End - 1))
        return patchError(
            "final runtime-function range is not executable code");

      if (TargetArch != Arch::X64) {
        if (EntryOrErr->HasLanguageHandler &&
            !IsExecutableRVA(EntryOrErr->Handler))
          return patchError("final ARM language handler is not executable");
        continue;
      }
      RuntimeEntry Current = *EntryOrErr;
      std::set<uint32_t> SeenUnwind;
      bool ReachedTerminal = false;
      for (unsigned Depth = 0; Depth < 32; ++Depth) {
        if (Current.Begin >= Current.End || Current.End > SizeOfImage ||
            !IsExecutableRVA(Current.Begin) ||
            !IsExecutableRVA(Current.End - 1))
          return patchError("final chained x64 code range is invalid");
        if (!SeenUnwind.insert(Current.Unwind).second)
          return patchError("final x64 unwind chain is cyclic");
        if (Current.Unwind == 0 || Current.Unwind >= SizeOfImage ||
            (Current.Unwind & 3u) != 0)
          return patchError("final chained x64 unwind RVA is invalid");

        ExceptionFunction Decoded = coff_loader::decodeX64ExceptionFunction(
            ValidationImage, ImageBase, /*RuntimeFunctionRVA=*/0, Current.Begin,
            Current.End, Current.Unwind);
        if (Decoded.ParseStatus != ExceptionParseStatus::Complete) {
          std::string Message = "final x64 unwind record is not fully valid";
          if (!Decoded.Diagnostics.empty())
            Message += ": " + Decoded.Diagnostics.front();
          return patchError(Message);
        }
        if (Decoded.Kind == RuntimeFunctionKind::Chained) {
          if (!Decoded.ChainedPrimaryRange ||
              Decoded.ChainedUnwindInfoRVA == 0 ||
              Decoded.ChainedPrimaryRange->Begin < ImageBase ||
              Decoded.ChainedPrimaryRange->End < ImageBase ||
              Decoded.ChainedPrimaryRange->Begin - ImageBase >
                  std::numeric_limits<uint32_t>::max() ||
              Decoded.ChainedPrimaryRange->End - ImageBase >
                  std::numeric_limits<uint32_t>::max())
            return patchError("final chained x64 runtime record is invalid");
          Current.Begin = static_cast<uint32_t>(
              Decoded.ChainedPrimaryRange->Begin - ImageBase);
          Current.End = static_cast<uint32_t>(Decoded.ChainedPrimaryRange->End -
                                              ImageBase);
          Current.Unwind = Decoded.ChainedUnwindInfoRVA;
          continue;
        }
        ReachedTerminal = true;
        break;
      }
      if (!ReachedTerminal)
        return patchError("final x64 unwind chain exceeds depth limit");
    }
    if (!std::is_sorted(Entries.begin(), Entries.end(),
                        [](const RuntimeEntry &A, const RuntimeEntry &B) {
                          return A.Begin < B.Begin;
                        }))
      return patchError("final runtime-function entries are not RVA-sorted");
    if (llvm::Error Err =
            validateRuntimeEntries(Entries, SizeOfImage, TargetArch))
      return Err;

    const uint32_t PointerSize = PE.Is64 ? 8 : 4;
    for (auto I = COFF->import_directory_begin(),
              E = COFF->import_directory_end();
         I != E; ++I) {
      llvm::StringRef DLLName;
      if (llvm::Error Err = I->getName(DLLName)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      uint32_t IATRVA = 0;
      if (llvm::Error Err = I->getImportAddressTableRVA(IATRVA)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      uint32_t Index = 0;
      for (auto SI = I->imported_symbol_begin(), SE = I->imported_symbol_end();
           SI != SE; ++SI, ++Index) {
        const uint64_t SlotRVA =
            uint64_t(IATRVA) + uint64_t(Index) * PointerSize;
        if (SlotRVA > InvalidVA - ImageBase)
          break;
        coff_loader::addImportedSymbol(SI, DLLName, ImageBase + SlotRVA,
                                       ValidationImage);
      }
    }
    for (auto I = COFF->export_directory_begin(),
              E = COFF->export_directory_end();
         I != E; ++I) {
      llvm::StringRef Name;
      uint32_t RVA = 0;
      uint32_t Ordinal = 0;
      if (llvm::Error Err = I->getSymbolName(Name)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      if (llvm::Error Err = I->getExportRVA(RVA)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      if (llvm::Error Err = I->getOrdinal(Ordinal)) {
        llvm::consumeError(std::move(Err));
        continue;
      }
      if (RVA > InvalidVA - ImageBase)
        continue;
      va_t Address = ImageBase + RVA;
      const Segment *Target = ValidationImage.getSegmentFor(Address);
      if (Target && Target->isExecutable())
        Address =
            normalizeCodeAddress(Address, TargetArch, ValidationImage.Mode);
      Export Exp;
      Exp.Name = Name.str();
      Exp.Ordinal = Ordinal;
      Exp.Addr = Address;
      ValidationImage.Exports.push_back(std::move(Exp));
    }
    coff_loader::parseDelayImports(*COFF, ValidationImage);
    coff_loader::parseExceptions(*COFF, ValidationImage, ImageBase);
    coff_loader::parseSymbolTable(*COFF, ValidationImage, ImageBase);
    coff_loader::resolveExceptionHandlers(ValidationImage);
    for (const ExceptionFunction &EH :
         ValidationImage.ExceptionMetadata.Functions) {
      if (EH.ParseStatus == ExceptionParseStatus::Malformed) {
        std::string Message =
            "final language exception metadata is malformed at 0x" +
            llvm::utohexstr(EH.CodeRange.Begin);
        if (!EH.Diagnostics.empty())
          Message += ": " + EH.Diagnostics.front();
        if (EH.Cxx && !EH.Cxx->IPMap.empty())
          Message += " (range 0x" + llvm::utohexstr(EH.CodeRange.Begin) +
                     "..0x" + llvm::utohexstr(EH.CodeRange.End) +
                     ", first IP 0x" +
                     llvm::utohexstr(EH.Cxx->IPMap.front().IP) + ")";
        return patchError(Message);
      }
      if ((EH.Personality == ExceptionPersonality::CSpecificHandler ||
           EH.Personality == ExceptionPersonality::CxxFrameHandler3) &&
          EH.ParseStatus != ExceptionParseStatus::Complete)
        return patchError(
            "final supported language exception metadata is incomplete");
    }
  }

  const llvm::object::data_directory *LoadConfigDirectory =
      getPEDataDirectory(PE, llvm::COFF::LOAD_CONFIG_TABLE);
  if (!LoadConfigDirectory || LoadConfigDirectory->Size == 0)
    return llvm::Error::success();
  if (LoadConfigDirectory->RelativeVirtualAddress == 0)
    return patchError("final load-config directory has a zero RVA");
  if (LoadConfigDirectory->RelativeVirtualAddress >= SizeOfImage ||
      LoadConfigDirectory->Size >
          SizeOfImage - LoadConfigDirectory->RelativeVirtualAddress)
    return patchError("final load configuration leaves the image extent");
  auto LoadOffset =
      rvaToFileOffset(PE, Binary, LoadConfigDirectory->RelativeVirtualAddress,
                      LoadConfigDirectory->Size);
  if (!LoadOffset)
    return patchError("final load configuration is not file-backed");
  if (LoadConfigDirectory->Size < sizeof(uint32_t))
    return patchError("final load configuration has no size field");
  uint32_t DeclaredLoadSize = readLE<uint32_t>(Binary.data() + *LoadOffset);
  if (DeclaredLoadSize < sizeof(uint32_t))
    return patchError("final load configuration declares an invalid size");
  uint32_t EffectiveLoadSize =
      std::min<uint32_t>(LoadConfigDirectory->Size, DeclaredLoadSize);

  uint32_t GuardFlags = 0;
  uint64_t CFTableVA = 0, CFCount = 0, EHTableVA = 0, EHCount = 0;
  bool HasCFTableField = false, HasCFCountField = false;
  bool HasEHTableField = false, HasEHCountField = false;
  auto ReadField = [&](size_t Offset, size_t Width, uint64_t &Value) -> bool {
    if (!rangeInBounds(Offset, Width, EffectiveLoadSize) ||
        !rangeInBounds(*LoadOffset + Offset, Width, Binary.size()))
      return false;
    Value = Width == 8 ? readLE<uint64_t>(Binary.data() + *LoadOffset + Offset)
                       : readLE<uint32_t>(Binary.data() + *LoadOffset + Offset);
    return true;
  };
  using llvm::object::coff_load_configuration32;
  using llvm::object::coff_load_configuration64;
  if (PE.Is64) {
    uint64_t Flags = 0;
    if (ReadField(offsetof(coff_load_configuration64, GuardFlags), 4, Flags))
      GuardFlags = static_cast<uint32_t>(Flags);
    HasCFTableField =
        ReadField(offsetof(coff_load_configuration64, GuardCFFunctionTable), 8,
                  CFTableVA);
    HasCFCountField = ReadField(
        offsetof(coff_load_configuration64, GuardCFFunctionCount), 8, CFCount);
    HasEHTableField =
        ReadField(offsetof(coff_load_configuration64, GuardEHContinuationTable),
                  8, EHTableVA);
    HasEHCountField =
        ReadField(offsetof(coff_load_configuration64, GuardEHContinuationCount),
                  8, EHCount);
  } else {
    uint64_t Flags = 0;
    if (ReadField(offsetof(coff_load_configuration32, GuardFlags), 4, Flags))
      GuardFlags = static_cast<uint32_t>(Flags);
    HasCFTableField =
        ReadField(offsetof(coff_load_configuration32, GuardCFFunctionTable), 4,
                  CFTableVA);
    HasCFCountField = ReadField(
        offsetof(coff_load_configuration32, GuardCFFunctionCount), 4, CFCount);
    HasEHTableField =
        ReadField(offsetof(coff_load_configuration32, GuardEHContinuationTable),
                  4, EHTableVA);
    HasEHCountField =
        ReadField(offsetof(coff_load_configuration32, GuardEHContinuationCount),
                  4, EHCount);
  }

  auto ValidateGuardTable = [&](uint64_t TableVA, uint64_t Count,
                                uint32_t Stride,
                                llvm::StringRef Name) -> llvm::Error {
    if (Count == 0)
      return llvm::Error::success();
    if (TableVA < ImageBase ||
        TableVA - ImageBase > std::numeric_limits<uint32_t>::max() ||
        Count > Binary.size() / Stride)
      return patchError(Name + " has an invalid pointer/count");
    uint32_t TableRVA = static_cast<uint32_t>(TableVA - ImageBase);
    uint64_t Bytes = Count * Stride;
    if (TableRVA >= SizeOfImage || Bytes > SizeOfImage - TableRVA)
      return patchError(Name + " leaves the image extent");
    auto Offset = rvaToFileOffset(PE, Binary, TableRVA, Bytes);
    if (!Offset)
      return patchError(Name + " is not file-backed");
    uint32_t Previous = 0;
    bool HavePrevious = false;
    for (uint64_t I = 0; I < Count; ++I) {
      uint32_t RVA = readLE<uint32_t>(Binary.data() + *Offset + I * Stride);
      if (!IsExecutableRVA(RVA))
        return patchError(Name + " contains a non-executable target");
      if (HavePrevious && RVA <= Previous)
        return patchError(Name + " is not strictly RVA-sorted");
      Previous = RVA;
      HavePrevious = true;
    }
    return llvm::Error::success();
  };

  if ((GuardFlags &
       uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT)) != 0) {
    if (!HasCFTableField || !HasCFCountField)
      return patchError("final Guard CF fields are truncated");
    uint32_t ExtraBytes =
        (GuardFlags &
         uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_SIZE_MASK)) >>
        28;
    if (llvm::Error Err = ValidateGuardTable(CFTableVA, CFCount, 4 + ExtraBytes,
                                             "final Guard CF table"))
      return Err;
  }
  if ((GuardFlags &
       uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT)) != 0) {
    if (!HasEHTableField || !HasEHCountField)
      return patchError("final Guard EH continuation fields are truncated");
    if (llvm::Error Err = ValidateGuardTable(
            EHTableVA, EHCount, 4, "final Guard EH continuation table"))
      return Err;
  }
  return llvm::Error::success();
}

} // namespace neverd
