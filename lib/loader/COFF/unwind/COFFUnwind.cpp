//===- COFFUnwind.cpp - PE exception-table parsing ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFUnwindDetail.h"

#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/COFF/COFFUnwindARM.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define DEBUG_TYPE "neverd-coff-loader"

namespace neverd {
namespace coff_loader {

using namespace llvm::COFF;
using namespace llvm::object;

namespace {

using FunctionSymbolIndex = std::map<va_t, std::vector<size_t>>;

void diagnoseDirectory(BinaryImage &Img, llvm::StringRef Message) {
  auto &State = *Img.ExceptionMetadata.StructuralDecode;
  State.mergeStatus(ExceptionParseStatus::Malformed);
  State.Diagnostics.push_back(Message.str());
}

FunctionSymbolIndex indexFunctionSymbols(const BinaryImage &Img) {
  // A section or data label must not suppress independent unwind discovery.
  FunctionSymbolIndex Result;
  for (size_t I = 0; I < Img.Symbols.size(); ++I)
    if (Img.Symbols[I].IsFunc)
      Result[Img.Symbols[I].Addr].push_back(I);
  return Result;
}

// Preserve every typed alias, enriching only absent sizes. Indices stay valid
// when unnamed unwind functions append to Img.Symbols and reallocate it.
void completeFunctionSizes(BinaryImage &Img, const FunctionSymbolIndex &Index,
                           va_t Addr, uint64_t Size) {
  auto It = Index.find(Addr);
  if (It == Index.end())
    return;
  for (size_t I : It->second)
    if (Img.Symbols[I].IsFunc && Img.Symbols[I].Size == 0)
      Img.Symbols[I].Size = Size;
}

void commitPrimaryFunctionSymbol(BinaryImage &Img, va_t Addr, va_t End,
                                 const FunctionSymbolIndex &FunctionSymbols,
                                 std::set<va_t> &Seen, size_t &Added) {
  const bool AlreadySeen = !Seen.insert(Addr).second;
  const auto *SegPtr = Img.getSegmentFor(Addr);
  if (!SegPtr || !SegPtr->isExecutable())
    return;
  size_t Off = static_cast<size_t>(Addr - SegPtr->VA);
  if (AlreadySeen) {
    // Completing a pre-existing symbol still requires a decode-shaped
    // start so a data alias at the same VA cannot inherit pdata size.
    if (!checkPrologueAtOffset(*SegPtr, Off, Img.Arch))
      return;
    completeFunctionSizes(Img, FunctionSymbols, Addr, End - Addr);
    return;
  }
  // A primary RUNTIME_FUNCTION is PE-authenticated function identity.
  // Scan-style prologue heuristics miss ordinary entries -- MSVC's Win64
  // register home `mov [rsp+8], ecx` (0x89), an ARM64 `str x19, [sp, #-16]!`,
  // a Thumb-2 leaf that starts with arithmetic -- and would drop unnamed
  // pdata entries.
  Img.Symbols.push_back(Symbol::makeFunc(Addr, End - Addr));
  ++Added;
}

/// A function symbol that pdata sized grows over the chained fragments that
/// follow its primary record without a gap, the rest of its body
/// (ExceptionFunction::contiguousEnd); a size from another source stays.
void extendPrimarySymbolsOverFragments(BinaryImage &Img) {
  struct Extent {
    va_t PrimaryEnd;
    va_t End;
  };
  std::unordered_map<va_t, Extent> Extended;
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
    if (F.Kind == RuntimeFunctionKind::Chained || !F.CodeRange.isValid())
      continue;
    if (const va_t End = F.contiguousEnd(); End != F.CodeRange.End)
      Extended[F.CodeRange.Begin] = {F.CodeRange.End, End};
  }
  for (Symbol &Sym : Img.Symbols) {
    if (!Sym.IsFunc)
      continue;
    const auto It = Extended.find(Sym.Addr);
    if (It != Extended.end() && Sym.Size == It->second.PrimaryEnd - Sym.Addr)
      Sym.Size = It->second.End - Sym.Addr;
  }
}

bool runtimeFunctionCovers(va_t Addr, va_t End, va_t Query) {
  return Query >= Addr && Query < End;
}

/// True when the UNWIND_INFO at \p UnwindRVA chains to another record.
/// UNWIND_INFO byte 0 is Version:3 | Flags:5.
bool hasChainedUnwindInfo(const BinaryImage &Img, uint64_t ImageBase,
                          uint32_t UnwindRVA) {
  if (UnwindRVA > InvalidVA - ImageBase)
    return false;
  const uint8_t *Header = Img.readVA(ImageBase + UnwindRVA, 1);
  return Header && ((*Header >> 3) & unwind::UNW_ChainInfo) != 0;
}

void commitKnownCodeRanges(BinaryImage &Img) {
  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
}

void noteKnownCodeRange(BinaryImage &Img, va_t Begin, va_t End) {
  if (Begin < End)
    Img.KnownCodeRanges.emplace_back(Begin, End);
}

void notePdataNeighbors(BinaryImage &Img, uint64_t ImageBase, va_t Begin,
                        va_t End) {
  noteKnownCodeRange(Img, Begin, End);
  if (Img.COFFPDataRecords.empty() || Begin < ImageBase)
    return;
  const uint32_t BeginRVA = static_cast<uint32_t>(Begin - ImageBase);
  const auto It = std::lower_bound(
      Img.COFFPDataRecords.begin(), Img.COFFPDataRecords.end(), BeginRVA,
      [](const BinaryImage::COFFPDataRecord &Rec, uint32_t RVA) {
        return Rec.BeginRVA < RVA;
      });
  auto addRec = [&](const BinaryImage::COFFPDataRecord &Rec) {
    if (Rec.BeginRVA < Rec.EndRVA && Rec.BeginRVA <= InvalidVA - ImageBase &&
        Rec.EndRVA <= InvalidVA - ImageBase)
      noteKnownCodeRange(Img, ImageBase + Rec.BeginRVA, ImageBase + Rec.EndRVA);
  };
  if (It != Img.COFFPDataRecords.begin())
    addRec(*std::prev(It));
  if (It != Img.COFFPDataRecords.end()) {
    const auto Next = std::next(It);
    if (Next != Img.COFFPDataRecords.end())
      addRec(*Next);
  }
}

void parseX64Exceptions(const COFFObjectFile &Obj, BinaryImage &Img,
                        uint64_t ImageBase) {
  const data_directory *ExcDir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!ExcDir || ExcDir->RelativeVirtualAddress == 0 || ExcDir->Size == 0)
    return;

  uintptr_t ExcPtr;
  if (auto Err = Obj.getRvaPtr(ExcDir->RelativeVirtualAddress, ExcPtr)) {
    llvm::consumeError(std::move(Err));
    diagnoseDirectory(Img, "x64 exception directory RVA is not file-backed");
    return;
  }

  using RuntimeFunc = llvm::object::coff_runtime_function_x64;
  // getRvaPtr only guarantees the start RVA is mapped; ExcDir->Size is
  // untrusted, so clamp the entry count to the bytes actually present between
  // the table pointer and the end of the file buffer.
  llvm::StringRef FileData = Obj.getData();
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(FileData.data());
  uintptr_t FileEnd = FileBegin + FileData.size();
  size_t AvailBytes =
      (ExcPtr >= FileBegin && ExcPtr <= FileEnd) ? (FileEnd - ExcPtr) : 0;
  size_t Count =
      std::min<size_t>(ExcDir->Size, AvailBytes) / sizeof(RuntimeFunc);
  if (ExcDir->Size > AvailBytes || ExcDir->Size % sizeof(RuntimeFunc) != 0) {
    diagnoseDirectory(Img, "truncated or misaligned x64 exception directory");
  }
  const auto *RFBytes = reinterpret_cast<const uint8_t *>(ExcPtr);

  const auto FunctionSymbols = indexFunctionSymbols(Img);
  std::set<va_t> Seen;
  for (const auto &Entry : FunctionSymbols)
    Seen.insert(Entry.first);

  const bool Restrict = !Img.LoadOnlyFunctionEntries.empty();
  if (Restrict)
    Img.KnownCodeRanges.reserve(Img.KnownCodeRanges.size() +
                                3 * Img.LoadOnlyFunctionEntries.size() + 8);
  else
    Img.KnownCodeRanges.reserve(Img.KnownCodeRanges.size() + Count);
  if (Restrict)
    Img.COFFPDataRecords.reserve(Count);
  else
    Img.ExceptionMetadata.Functions.reserve(Count);
  std::unordered_map<size_t, ExceptionFunction> RestrictedDecoded;
  const std::vector<bool> RestrictedNeeded =
      Restrict ? unwind_detail::selectRestrictedX64Records(
                     Img, ImageBase, RFBytes, Count,
                     ExcDir->RelativeVirtualAddress, RestrictedDecoded)
               : std::vector<bool>();

  [[maybe_unused]] size_t Added = 0;
  bool SawZeroEntry = false;
  bool DiagnosedInteriorPadding = false;
  for (size_t I = 0; I < Count; ++I) {
    RuntimeFunc RF;
    std::memcpy(&RF, RFBytes + I * sizeof(RuntimeFunc), sizeof(RuntimeFunc));
    if (RF.BeginAddress == 0 && RF.EndAddress == 0 &&
        RF.UnwindInformation == 0) {
      SawZeroEntry = true;
      continue;
    }
    if (SawZeroEntry && !DiagnosedInteriorPadding) {
      diagnoseDirectory(
          Img, "x64 exception directory contains non-trailing zero padding");
      DiagnosedInteriorPadding = true;
    }
    uint64_t RecordRVA64 = uint64_t(ExcDir->RelativeVirtualAddress) +
                           uint64_t(I) * sizeof(RuntimeFunc);
    uint32_t RecordRVA = RecordRVA64 <= std::numeric_limits<uint32_t>::max()
                             ? static_cast<uint32_t>(RecordRVA64)
                             : 0;
    va_t Addr = 0;
    va_t End = 0;
    const bool HaveRange = RF.BeginAddress < RF.EndAddress &&
                           RF.BeginAddress <= InvalidVA - ImageBase &&
                           RF.EndAddress <= InvalidVA - ImageBase;
    if (HaveRange) {
      Addr = ImageBase + RF.BeginAddress;
      End = ImageBase + RF.EndAddress;
      if (!Restrict)
        Img.KnownCodeRanges.emplace_back(Addr, End);
      // Read the chain flag here too, because a restricted load decodes only
      // the records it selects.
      if (hasChainedUnwindInfo(Img, ImageBase, RF.UnwindInformation))
        Img.ContinuationCodeStarts.insert(Addr);
    }
    if (Restrict) {
      Img.COFFPDataRecords.push_back(
          {RF.BeginAddress, RF.EndAddress, RF.UnwindInformation, RecordRVA});
      if (!HaveRange || !RestrictedNeeded[I])
        continue;
    }
    const auto Predecoded = RestrictedDecoded.find(I);
    // Construct directly in the reserved directory. Moving a complete record
    // also visits the many optional language tables absent from most entries.
    ExceptionFunction &Stored = Img.ExceptionMetadata.Functions.emplace_back();
    if (Predecoded != RestrictedDecoded.end())
      Stored = std::move(Predecoded->second);
    else
      unwind_detail::decodeX64ExceptionFunctionInto(
          Img, ImageBase, RecordRVA, RF.BeginAddress, RF.EndAddress,
          RF.UnwindInformation, Stored);
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, Stored.ParseStatus);
    const bool HasRange = Stored.CodeRange.isValid();
    const bool IsChained = Stored.Kind == RuntimeFunctionKind::Chained;
    if (!HasRange)
      continue;
    Addr = Stored.CodeRange.Begin;
    End = Stored.CodeRange.End;
    // Full loads recorded this directory range before decoding the unwind
    // info, including records whose unwind bytes are malformed.
    if (Restrict)
      notePdataNeighbors(Img, ImageBase, Addr, End);

    if (IsChained)
      continue;
    commitPrimaryFunctionSymbol(Img, Addr, End, FunctionSymbols, Seen, Added);
  }

  if (Restrict) {
    std::vector<va_t> Primaries;
    for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
      if (F.Kind == RuntimeFunctionKind::Chained && F.ChainedPrimaryRange)
        Primaries.push_back(F.ChainedPrimaryRange->Begin);
    for (va_t Primary : Primaries)
      ensureX64RuntimeFunction(Img, Primary);
  }

  unwind_detail::resolveX64UnwindChains(Img.ExceptionMetadata);
  extendPrimarySymbolsOverFragments(Img);

  if (Restrict) {
    for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
      if (F.CodeRange.isValid())
        notePdataNeighbors(Img, ImageBase, F.CodeRange.Begin, F.CodeRange.End);
  }

  commitKnownCodeRanges(Img);
  Img.ExceptionMetadata.rebuildIndex();
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Count
                          << " RUNTIME_FUNCTION entries (" << Added
                          << " new funcs)\n");
}

/// Note whichever operation established the frame pointer, so a consumer that
/// only wants to know how to find the frame does not have to walk the whole
/// prologue looking for it.
void recordFrameRegister(ExceptionFunction &EF) {
  for (const UnwindOperation &Op : EF.UnwindOperations) {
    if (Op.Kind != UnwindOperationKind::SetFramePointer &&
        Op.Kind != UnwindOperationKind::AddFramePointer)
      continue;
    EF.FrameRegister = Op.Register;
    EF.FrameOffset = static_cast<uint32_t>(Op.StackOffset);
    return;
  }
}

/// Attach a decoded prologue sequence to \p EF.
void applyPackedUnwind(ExceptionFunction &EF, ARMUnwindDecode Decoded) {
  EF.PrologueSize = Decoded.PrologueSize;
  EF.UnwindOperations = std::move(Decoded.Operations);
  EF.ParseStatus = mergeExceptionParseStatus(EF.ParseStatus, Decoded.Status);
  for (std::string &Message : Decoded.Diagnostics)
    EF.Diagnostics.push_back(std::move(Message));
  recordFrameRegister(EF);
}

/// Decode the prologue and every epilogue an unpacked `.xdata` record holds.
///
/// \p Codes is the unwind-code byte array and \p Scopes the epilogue scope
/// words that precede it, empty when the record used the single-epilogue form
/// that puts the one scope's index in the header instead.  \p SingleEpilogue,
/// when set, is that index.
void applyUnwindCodes(ExceptionFunction &EF, bool IsAArch64,
                      llvm::ArrayRef<uint8_t> Codes,
                      llvm::ArrayRef<llvm::support::ulittle32_t> Scopes,
                      std::optional<uint32_t> SingleEpilogue) {
  applyPackedUnwind(EF, IsAArch64 ? decodeARM64UnwindCodes(Codes)
                                  : decodeARM32UnwindCodes(Codes));

  // An epilogue's offset is stored in instruction units, which differ between
  // the two instruction sets: ARM64 counts words, Thumb-2 halfwords.
  const uint32_t OffsetScale = IsAArch64 ? 4 : 2;
  auto decodeEpilogue = [&](uint32_t StartOffset, uint32_t StartIndex,
                            uint8_t Condition) {
    UnwindEpilog Epilog;
    Epilog.StartOffset = int64_t(StartOffset) * OffsetScale;
    Epilog.Flags = Condition;
    Epilog.FirstOperationOffset = StartIndex;
    ARMUnwindDecode Decoded = IsAArch64
                                  ? decodeARM64UnwindCodes(Codes, StartIndex)
                                  : decodeARM32UnwindCodes(Codes, StartIndex);
    // The epilogue's own length is the span of the instructions its codes
    // stand against, measured from where the scope says it starts.
    Epilog.LastInstructionOffset =
        static_cast<uint32_t>(Epilog.StartOffset) + Decoded.PrologueSize;
    Epilog.Operations = std::move(Decoded.Operations);
    EF.ParseStatus = mergeExceptionParseStatus(EF.ParseStatus, Decoded.Status);
    for (std::string &Message : Decoded.Diagnostics)
      EF.Diagnostics.push_back(std::move(Message));
    EF.Epilogs.push_back(std::move(Epilog));
  };

  if (SingleEpilogue) {
    // The compact form names no offset: the one epilogue runs to the end of
    // the function, so where it starts is not recorded separately.
    decodeEpilogue(0, *SingleEpilogue, 0);
    return;
  }
  for (llvm::support::ulittle32_t Word : Scopes) {
    const llvm::ARM::WinEH::EpilogueScope ES(Word);
    decodeEpilogue(ES.EpilogueStartOffset(),
                   IsAArch64 ? ES.EpilogueStartIndexAArch64()
                             : ES.EpilogueStartIndexARM(),
                   IsAArch64 ? 0 : ES.Condition());
  }
}

void parseARMExceptions(const COFFObjectFile &Obj, BinaryImage &Img,
                        uint64_t ImageBase) {
  const data_directory *ExcDir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!ExcDir || ExcDir->RelativeVirtualAddress == 0 || ExcDir->Size == 0)
    return;

  uintptr_t ExcPtr;
  if (auto Err = Obj.getRvaPtr(ExcDir->RelativeVirtualAddress, ExcPtr)) {
    llvm::consumeError(std::move(Err));
    diagnoseDirectory(Img, "ARM exception directory RVA is not file-backed");
    return;
  }

  llvm::StringRef FileData = Obj.getData();
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(FileData.data());
  uintptr_t FileEnd = FileBegin + FileData.size();
  size_t AvailBytes =
      (ExcPtr >= FileBegin && ExcPtr <= FileEnd) ? (FileEnd - ExcPtr) : 0;
  constexpr size_t EntrySize = 2 * sizeof(uint32_t);
  size_t Count = std::min<size_t>(ExcDir->Size, AvailBytes) / EntrySize;
  if (ExcDir->Size > AvailBytes || ExcDir->Size % EntrySize != 0) {
    diagnoseDirectory(Img, "truncated or misaligned ARM exception directory");
  }
  const auto *RFBytes = reinterpret_cast<const uint8_t *>(ExcPtr);
  Img.ExceptionMetadata.Functions.reserve(Count);
  const auto FunctionSymbols = indexFunctionSymbols(Img);
  std::set<va_t> Seen;
  for (const auto &Entry : FunctionSymbols)
    Seen.insert(Entry.first);
  const bool IsAArch64 = Img.Arch == Arch::AArch64;
  [[maybe_unused]] const char *ArchName = IsAArch64 ? "ARM64" : "ARM32";
  auto DiagnoseEntry = [&](size_t Index, llvm::StringRef Message) {
    diagnoseDirectory(Img, std::string(ArchName) + " pdata entry " +
                               std::to_string(Index) + ": " + Message.str());
  };

  [[maybe_unused]] size_t Added = 0;
  bool SawZeroEntry = false;
  bool DiagnosedInteriorPadding = false;
  for (size_t I = 0; I < Count; ++I) {
    const uint8_t *Entry = RFBytes + I * EntrySize;
    llvm::support::ulittle32_t Words[2];
    std::memcpy(Words, Entry, EntrySize);
    uint32_t BeginWord = Words[0];
    uint32_t UnwindWord = Words[1];
    if (BeginWord == 0 && UnwindWord == 0) {
      SawZeroEntry = true;
      continue;
    }
    if (SawZeroEntry && !DiagnosedInteriorPadding) {
      diagnoseDirectory(Img, std::string(ArchName) +
                                 " exception directory contains non-trailing "
                                 "zero padding");
      DiagnosedInteriorPadding = true;
    }

    llvm::ARM::WinEH::RuntimeFunctionFlag Flag;
    uint32_t Length = 0;
    uint32_t XDataRVA = 0;
    va_t XDataVA = 0;
    va_t PersonalityVA = 0;
    va_t HandlerDataVA = 0;
    std::vector<uint8_t> NativeUnwindBytes;
    // Where the unwind codes and the epilogue scopes sit inside those bytes.
    // Recorded here rather than decoded in place because the byte vector is
    // handed to the record before the codes are read out of it.
    size_t UnwindCodeOffset = 0;
    size_t UnwindCodeLength = 0;
    size_t EpilogueScopeOffset = 0;
    size_t EpilogueScopeCount = 0;
    std::optional<uint32_t> SingleEpilogueIndex;
    ExceptionParseStatus EntryStatus = ExceptionParseStatus::Complete;
    std::vector<std::string> EntryDiagnostics;
    auto DiagnoseUnwindBody = [&](llvm::StringRef Message) {
      EntryStatus = mergeExceptionParseStatus(EntryStatus,
                                              ExceptionParseStatus::Malformed);
      EntryDiagnostics.push_back(Message.str());
    };
    if (IsAArch64) {
      llvm::ARM::WinEH::RuntimeFunctionARM64 RF(Words);
      Flag = RF.Flag();
      if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed ||
          Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment)
        Length = RF.FunctionLength();
      else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked)
        XDataRVA = RF.ExceptionInformationRVA();
    } else {
      llvm::ARM::WinEH::RuntimeFunction RF(Words);
      Flag = RF.Flag();
      if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed ||
          Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment)
        Length = RF.FunctionLength();
      else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked)
        XDataRVA = RF.ExceptionInformationRVA();
    }

    bool IsFragment =
        Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment;
    bool HasNoPhysicalPrologue = IsFragment;
    if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked) {
      if (XDataRVA > InvalidVA - ImageBase) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " has invalid xdata RVA\n");
        DiagnoseEntry(I, "xdata RVA overflows");
        continue;
      }
      XDataVA = ImageBase + XDataRVA;
      const uint8_t *XData = Img.readVA(XDataVA, sizeof(uint32_t));
      if (!XData) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " has truncated xdata\n");
        DiagnoseEntry(I, "xdata header is truncated");
        continue;
      }
      uint32_t FirstXDataWord = readLE<uint32_t>(XData);
      const bool HasExtension = IsAArch64 ? (FirstXDataWord & 0xffc00000u) == 0
                                          : (FirstXDataWord & 0xff800000u) == 0;
      const size_t HeaderBytes = HasExtension ? 8 : 4;
      XData = Img.readVA(XDataVA, HeaderBytes);
      if (!XData) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " has truncated extended xdata header\n");
        DiagnoseEntry(I, "extended xdata header is truncated");
        continue;
      }
      llvm::support::ulittle32_t XDataWords[2] = {};
      std::memcpy(XDataWords, XData, HeaderBytes);
      llvm::ARM::WinEH::ExceptionDataRecord XR(XDataWords, IsAArch64);
      if (XR.Vers() != 0) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " has unsupported xdata version\n");
        DiagnoseEntry(I, "xdata version is unsupported");
        continue;
      }
      if (IsAArch64) {
        Length = XR.FunctionLengthInBytesAArch64();
      } else {
        Length = XR.FunctionLengthInBytesARM();
        IsFragment = XR.F();
        HasNoPhysicalPrologue = IsFragment;
      }

      const uint64_t HeaderWords = HasExtension ? 2 : 1;
      const uint64_t EpilogueWords = XR.E() ? 0 : XR.EpilogueCount();
      const uint64_t CodeWords = XR.CodeWords();
      bool HasValidBody = true;
      if (HeaderWords > std::numeric_limits<uint64_t>::max() - EpilogueWords ||
          HeaderWords + EpilogueWords >
              std::numeric_limits<uint64_t>::max() - CodeWords) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " xdata size overflows\n");
        DiagnoseUnwindBody("xdata size overflows");
        HasValidBody = false;
      }
      uint64_t PreHandlerWords = 0;
      uint64_t StructuralWords = 0;
      if (HasValidBody) {
        PreHandlerWords = HeaderWords + EpilogueWords + CodeWords;
        StructuralWords = PreHandlerWords + (XR.X() ? 1 : 0);
      }
      if (HasValidBody &&
          StructuralWords > std::numeric_limits<size_t>::max() / 4) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " xdata size is unrepresentable\n");
        DiagnoseUnwindBody("xdata size is unrepresentable");
        HasValidBody = false;
      }
      size_t StructuralBytes = 0;
      if (HasValidBody) {
        StructuralBytes = static_cast<size_t>(StructuralWords * 4);
        XData = Img.readVA(XDataVA, StructuralBytes);
        if (!XData) {
          LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry "
                                  << I << " has truncated xdata body\n");
          DiagnoseUnwindBody("xdata body is truncated");
          HasValidBody = false;
        }
      }
      if (HasValidBody) {
        NativeUnwindBytes.assign(XData, XData + StructuralBytes);
        UnwindCodeOffset =
            static_cast<size_t>((HeaderWords + EpilogueWords) * 4);
        UnwindCodeLength = static_cast<size_t>(CodeWords * 4);
        EpilogueScopeOffset = static_cast<size_t>(HeaderWords * 4);
        EpilogueScopeCount = static_cast<size_t>(EpilogueWords);
        // With the E bit set the record carries one epilogue and no scope
        // array, and the field that would have held the scope count holds
        // that epilogue's first unwind code instead.
        if (XR.E())
          SingleEpilogueIndex = XR.EpilogueCount();
      }
      if (HasValidBody && XR.X()) {
        uint32_t HandlerRVA =
            readLE<uint32_t>(XData + static_cast<size_t>(PreHandlerWords * 4));
        if (HandlerRVA == 0 || HandlerRVA > InvalidVA - ImageBase) {
          LLVM_DEBUG(llvm::dbgs()
                     << "coff: " << ArchName << " pdata entry " << I
                     << " has invalid exception handler RVA\n");
          DiagnoseUnwindBody("exception-handler RVA is invalid");
        } else if (XDataVA > InvalidVA - StructuralBytes) {
          LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry "
                                  << I << " handler-data address overflows\n");
          DiagnoseUnwindBody("handler-data address overflows");
        } else {
          PersonalityVA = ImageBase + HandlerRVA;
          HandlerDataVA = XDataVA + StructuralBytes;
        }
      }
    } else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Reserved) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " uses reserved unwind flags\n");
      DiagnoseEntry(I, "reserved runtime-function flags");
      continue;
    }

    uint64_t Begin = normalizeCodeAddress(BeginWord, Img.Arch, Img.Mode);
    if (Length == 0 || Begin > InvalidVA - ImageBase) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " has invalid start or length\n");
      DiagnoseEntry(I, "start or length is invalid");
      continue;
    }
    va_t Addr = ImageBase + Begin;
    if (Length > InvalidVA - Addr) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " range overflows\n");
      DiagnoseEntry(I, "code range overflows");
      continue;
    }
    va_t End = Addr + Length;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg || !Seg->isExecutable()) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " does not start in executable data\n");
      DiagnoseEntry(I, "code range does not start in executable data");
      continue;
    }
    uint64_t UsableSize = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (UsableSize > InvalidVA - Seg->VA || End > Seg->VA + UsableSize) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " exceeds executable data\n");
      DiagnoseEntry(I, "code range exceeds executable data");
      continue;
    }

    ExceptionFunction EF;
    uint64_t RecordRVA64 =
        uint64_t(ExcDir->RelativeVirtualAddress) + uint64_t(I) * EntrySize;
    EF.RuntimeFunctionRVA = RecordRVA64 <= std::numeric_limits<uint32_t>::max()
                                ? static_cast<uint32_t>(RecordRVA64)
                                : 0;
    EF.CodeRange = {Addr, End};
    EF.PackedUnwindData = UnwindWord;
    EF.Kind = IsFragment ? RuntimeFunctionKind::Fragment
                         : RuntimeFunctionKind::Primary;
    EF.ParseStatus = EntryStatus;
    EF.Diagnostics = std::move(EntryDiagnostics);
    if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Unpacked) {
      EF.Encoding = IsAArch64 ? ExceptionEncoding::ARM64Unpacked
                              : ExceptionEncoding::ARM32Unpacked;
      EF.UnwindInfoRVA = XDataRVA;
      EF.UnwindInfoVA = XDataVA;
      EF.NativeUnwindBytes = std::move(NativeUnwindBytes);
      if (UnwindCodeLength != 0) {
        llvm::ArrayRef<uint8_t> Structural(EF.NativeUnwindBytes);
        std::vector<llvm::support::ulittle32_t> Scopes(EpilogueScopeCount);
        for (size_t S = 0; S < EpilogueScopeCount; ++S)
          Scopes[S] = llvm::support::ulittle32_t(readLE<uint32_t>(
              Structural.data() + EpilogueScopeOffset + S * 4));
        applyUnwindCodes(EF, IsAArch64,
                         Structural.slice(UnwindCodeOffset, UnwindCodeLength),
                         Scopes, SingleEpilogueIndex);
        if (IsAArch64 &&
            std::any_of(EF.UnwindOperations.begin(), EF.UnwindOperations.end(),
                        [](const UnwindOperation &Op) {
                          return Op.Kind == UnwindOperationKind::EndChained;
                        })) {
          IsFragment = true;
          EF.Kind = RuntimeFunctionKind::Fragment;
        }
      }
      if (PersonalityVA != 0) {
        EF.PersonalityVA = PersonalityVA;
        EF.HandlerDataVA = HandlerDataVA;
        EF.Personality = ExceptionPersonality::Unknown;
        if (!Img.getSegmentFor(PersonalityVA) ||
            !Img.getSegmentFor(PersonalityVA)->isExecutable()) {
          EF.ParseStatus = ExceptionParseStatus::Partial;
          EF.Diagnostics.push_back(
              "ARM personality RVA is not mapped executable code");
        }
      }
    } else if (IsAArch64) {
      EF.Encoding = IsFragment ? ExceptionEncoding::ARM64PackedFragment
                               : ExceptionEncoding::ARM64Packed;
      applyPackedUnwind(EF, expandARM64PackedUnwind(UnwindWord));
    } else {
      EF.Encoding = IsFragment ? ExceptionEncoding::ARM32PackedFragment
                               : ExceptionEncoding::ARM32Packed;
      applyPackedUnwind(EF, expandARM32PackedUnwind(UnwindWord));
    }
    // Packed fragments and ARM32's unpacked F form explicitly have no physical
    // prologue.  ARM64 instead marks a secondary region with `end_c`; a
    // shrink-wrapped region may have real local prologue operations before that
    // marker, whose size was already separated from the phantom parent scope by
    // the unwind-code decoder and must be retained.
    if (HasNoPhysicalPrologue)
      EF.PrologueSize = 0;
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, EF.ParseStatus);
    const bool IsSymbolEligible =
        EF.ParseStatus == ExceptionParseStatus::Complete;
    Img.ExceptionMetadata.Functions.push_back(std::move(EF));

    Img.KnownCodeRanges.emplace_back(Addr, End);
    if (IsFragment || !IsSymbolEligible)
      continue;
    commitPrimaryFunctionSymbol(Img, Addr, End, FunctionSymbols, Seen, Added);
  }

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
  Img.ExceptionMetadata.rebuildIndex();
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Count << " " << ArchName
                          << " RUNTIME_FUNCTION entries (" << Added
                          << " new funcs)\n");
}

} // namespace

void unwind_detail::resolveX64UnwindChains(ExceptionInfo &Info) {
  // Rebuilt on every call: a lazily decoded record re-runs the resolution.
  for (ExceptionFunction &Function : Info.Functions)
    Function.FragmentRanges.clear();

  // Directory entries usually arrive in address order. A contiguous index
  // avoids a node and a separate vector allocation for every start address.
  // Preserve directory order among duplicate starts, including unsorted input.
  std::vector<std::pair<va_t, size_t>> ByBegin;
  ByBegin.reserve(Info.Functions.size());
  for (size_t J = 0; J < Info.Functions.size(); ++J) {
    const ExceptionAddressRange &Range = Info.Functions[J].CodeRange;
    if (Range.isValid())
      ByBegin.emplace_back(Range.Begin, J);
  }
  const auto ByAddress = [](const auto &A, const auto &B) {
    return A.first < B.first;
  };
  if (!std::is_sorted(ByBegin.begin(), ByBegin.end(), ByAddress))
    std::stable_sort(ByBegin.begin(), ByBegin.end(), ByAddress);

  for (size_t I = 0; I < Info.Functions.size(); ++I) {
    ExceptionFunction &F = Info.Functions[I];
    if (F.Kind != RuntimeFunctionKind::Chained || !F.ChainedPrimaryRange)
      continue;
    const va_t ParentBegin = F.ChainedPrimaryRange->Begin;
    const auto Found = std::lower_bound(
        ByBegin.begin(), ByBegin.end(), ParentBegin,
        [](const auto &Entry, va_t Begin) { return Entry.first < Begin; });
    for (auto It = Found; It != ByBegin.end() && It->first == ParentBegin;
         ++It) {
      const size_t J = It->second;
      if (I == J)
        continue;
      const ExceptionFunction &Candidate = Info.Functions[J];
      if (Candidate.CodeRange.End == F.ChainedPrimaryRange->End &&
          (F.ChainedUnwindInfoRVA == 0 ||
           Candidate.UnwindInfoRVA == F.ChainedUnwindInfoRVA)) {
        F.PrimaryFunctionIndex = J;
        break;
      }
    }
    if (!F.PrimaryFunctionIndex) {
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
      F.Diagnostics.push_back("chained x64 primary record is not in directory");
      Info.ParseStatus =
          mergeExceptionParseStatus(Info.ParseStatus, F.ParseStatus);
    }
  }

  for (size_t I = 0; I < Info.Functions.size(); ++I) {
    ExceptionFunction &Root = Info.Functions[I];
    if (Root.Kind != RuntimeFunctionKind::Chained || !Root.PrimaryFunctionIndex)
      continue;
    llvm::SmallDenseSet<size_t, 8> Visited;
    size_t Current = I;
    bool ReachedTerminal = false;
    size_t Terminal = I;
    while (true) {
      if (Current >= Info.Functions.size()) {
        Root.ParseStatus = mergeExceptionParseStatus(
            Root.ParseStatus, ExceptionParseStatus::Malformed);
        Root.Diagnostics.push_back("chained x64 unwind index is out of range");
        break;
      }
      if (!Visited.insert(Current).second) {
        Root.ParseStatus = mergeExceptionParseStatus(
            Root.ParseStatus, ExceptionParseStatus::Malformed);
        Root.Diagnostics.push_back("chained x64 unwind graph is cyclic");
        break;
      }
      const ExceptionFunction &CurrentFunction = Info.Functions[Current];
      if (CurrentFunction.Kind != RuntimeFunctionKind::Chained) {
        ReachedTerminal = true;
        Terminal = Current;
        if (Root.FrameRegister != CurrentFunction.FrameRegister) {
          Root.ParseStatus = mergeExceptionParseStatus(
              Root.ParseStatus, ExceptionParseStatus::Malformed);
          Root.Diagnostics.push_back(
              "chained x64 frame register differs from primary record");
        }
        if (Root.FrameOffset != CurrentFunction.FrameOffset) {
          Root.ParseStatus = mergeExceptionParseStatus(
              Root.ParseStatus, ExceptionParseStatus::Malformed);
          Root.Diagnostics.push_back(
              "chained x64 frame offset differs from primary record");
        }
        break;
      }
      if (!CurrentFunction.PrimaryFunctionIndex)
        break;
      Current = *CurrentFunction.PrimaryFunctionIndex;
    }
    if (!ReachedTerminal &&
        Root.ParseStatus != ExceptionParseStatus::Malformed) {
      Root.ParseStatus = mergeExceptionParseStatus(
          Root.ParseStatus, ExceptionParseStatus::Partial);
      Root.Diagnostics.push_back(
          "chained x64 unwind graph does not reach a primary record");
    }
    // A fault in this fragment unwinds through the chain to the primary
    // record, whose handler and scope table then govern it.
    if (ReachedTerminal && Root.ParseStatus == ExceptionParseStatus::Complete &&
        Root.CodeRange.isValid())
      Info.Functions[Terminal].FragmentRanges.push_back(Root.CodeRange);
    Info.ParseStatus =
        mergeExceptionParseStatus(Info.ParseStatus, Root.ParseStatus);
  }
  for (ExceptionFunction &Function : Info.Functions)
    std::sort(Function.FragmentRanges.begin(), Function.FragmentRanges.end(),
              [](const ExceptionAddressRange &A,
                 const ExceptionAddressRange &B) { return A.Begin < B.Begin; });
}

std::vector<bool> unwind_detail::selectRestrictedX64Records(
    const BinaryImage &Img, uint64_t ImageBase, const uint8_t *RFBytes,
    size_t Count, uint32_t DirectoryRVA,
    std::unordered_map<size_t, ExceptionFunction> &Decoded) {
  using RuntimeFunc = llvm::object::coff_runtime_function_x64;
  auto Read = [&](size_t I) {
    RuntimeFunc RF;
    std::memcpy(&RF, RFBytes + I * sizeof(RuntimeFunc), sizeof(RuntimeFunc));
    return RF;
  };
  auto HasRange = [&](const RuntimeFunc &RF) {
    return RF.BeginAddress < RF.EndAddress &&
           RF.BeginAddress <= InvalidVA - ImageBase &&
           RF.EndAddress <= InvalidVA - ImageBase;
  };
  std::vector<bool> Needed(Count, false);
  std::unordered_map<va_t, std::vector<size_t>> ChainedByParent;
  std::vector<size_t> Work;
  for (size_t I = 0; I < Count; ++I) {
    const RuntimeFunc RF = Read(I);
    if (!HasRange(RF))
      continue;
    const va_t Addr = ImageBase + RF.BeginAddress;
    const va_t End = ImageBase + RF.EndAddress;
    for (va_t Query : Img.LoadOnlyFunctionEntries) {
      if (runtimeFunctionCovers(Addr, End, Query)) {
        Needed[I] = true;
        Work.push_back(I);
        break;
      }
    }
    if (!hasChainedUnwindInfo(Img, ImageBase, RF.UnwindInformation))
      continue;
    const uint64_t RecordRVA =
        uint64_t(DirectoryRVA) + uint64_t(I) * sizeof(RuntimeFunc);
    ExceptionFunction EF = decodeX64ExceptionFunction(
        Img, ImageBase,
        RecordRVA <= std::numeric_limits<uint32_t>::max()
            ? static_cast<uint32_t>(RecordRVA)
            : 0,
        RF.BeginAddress, RF.EndAddress, RF.UnwindInformation);
    if (EF.ChainedPrimaryRange)
      ChainedByParent[EF.ChainedPrimaryRange->Begin].push_back(I);
    Decoded.emplace(I, std::move(EF));
  }
  while (!Work.empty()) {
    const size_t I = Work.back();
    Work.pop_back();
    const auto Children =
        ChainedByParent.find(ImageBase + Read(I).BeginAddress);
    if (Children == ChainedByParent.end())
      continue;
    for (size_t Child : Children->second) {
      if (Needed[Child])
        continue;
      Needed[Child] = true;
      Work.push_back(Child);
    }
  }
  return Needed;
}

bool ensureX64RuntimeFunction(BinaryImage &Img, va_t Address) {
  if (Img.Arch != Arch::X64)
    return false;
  if (const ExceptionFunction *Existing =
          Img.ExceptionMetadata.findFunction(Address);
      Existing && Existing->CodeRange.isValid())
    return false;
  if (Img.COFFPDataRecords.empty() || Address < Img.Base)
    return false;

  const uint32_t QueryRVA = static_cast<uint32_t>(Address - Img.Base);
  const auto It = std::upper_bound(
      Img.COFFPDataRecords.begin(), Img.COFFPDataRecords.end(), QueryRVA,
      [](uint32_t RVA, const BinaryImage::COFFPDataRecord &Rec) {
        return RVA < Rec.BeginRVA;
      });
  if (It == Img.COFFPDataRecords.begin())
    return false;
  const BinaryImage::COFFPDataRecord &Rec = *std::prev(It);
  if (QueryRVA < Rec.BeginRVA || QueryRVA >= Rec.EndRVA)
    return false;
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
    if (F.RuntimeFunctionRVA == Rec.RecordRVA)
      return false;
    if (F.CodeRange.isValid() && F.CodeRange.Begin == Img.Base + Rec.BeginRVA &&
        F.CodeRange.End == Img.Base + Rec.EndRVA)
      return false;
  }

  ExceptionFunction EF =
      decodeX64ExceptionFunction(Img, Img.Base, Rec.RecordRVA, Rec.BeginRVA,
                                 Rec.EndRVA, Rec.UnwindInfoRVA);
  Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
      Img.ExceptionMetadata.ParseStatus, EF.ParseStatus);
  const bool HasRange = EF.CodeRange.isValid();
  const bool IsChained = EF.Kind == RuntimeFunctionKind::Chained;
  const std::optional<ExceptionAddressRange> PrimaryRange =
      EF.ChainedPrimaryRange;
  Img.ExceptionMetadata.Functions.push_back(std::move(EF));
  if (HasRange) {
    const ExceptionFunction &Stored = Img.ExceptionMetadata.Functions.back();
    notePdataNeighbors(Img, Img.Base, Stored.CodeRange.Begin,
                       Stored.CodeRange.End);
    commitKnownCodeRanges(Img);
    if (!IsChained) {
      const auto FunctionSymbols = indexFunctionSymbols(Img);
      std::set<va_t> Seen;
      for (const auto &Entry : FunctionSymbols)
        Seen.insert(Entry.first);
      size_t Added = 0;
      commitPrimaryFunctionSymbol(Img, Stored.CodeRange.Begin,
                                  Stored.CodeRange.End, FunctionSymbols, Seen,
                                  Added);
    }
  }
  Img.ExceptionMetadata.rebuildIndex();
  if (IsChained && PrimaryRange)
    ensureX64RuntimeFunction(Img, PrimaryRange->Begin);
  unwind_detail::resolveX64UnwindChains(Img.ExceptionMetadata);
  extendPrimarySymbolsOverFragments(Img);
  return true;
}

void parseExceptions(const COFFObjectFile &Obj, BinaryImage &Img,
                     uint64_t ImageBase) {
  Img.ExceptionMetadata = ExceptionInfo{};
  // Establish the directory owner before aggregating localized frame faults.
  Img.ExceptionMetadata.captureUnstructuredParseContributions();
  if (const data_directory *ExcDir =
          Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE)) {
    Img.ExceptionMetadata.DirectoryRVA = ExcDir->RelativeVirtualAddress;
    Img.ExceptionMetadata.DirectorySize = ExcDir->Size;
  }
  if (Img.Arch == Arch::X64)
    parseX64Exceptions(Obj, Img, ImageBase);
  else if (Img.Arch == Arch::AArch64 || Img.Arch == Arch::ARM)
    parseARMExceptions(Obj, Img, ImageBase);
  Img.ExceptionMetadata.captureUnstructuredParseContributions();
  Img.ExceptionMetadata.rebuildParseSummary();
}

} // namespace coff_loader
} // namespace neverd
