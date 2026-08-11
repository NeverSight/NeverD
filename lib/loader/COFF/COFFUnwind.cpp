//===- COFFUnwind.cpp - PE exception-table parsing ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/ISAEncoding.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <string>

#define DEBUG_TYPE "neverd-coff-loader"

namespace neverd {
namespace coff_loader {

using namespace llvm::COFF;
using namespace llvm::object;

namespace {

void parseX64Exceptions(const COFFObjectFile &Obj, BinaryImage &Img,
                        uint64_t ImageBase) {
  const data_directory *ExcDir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!ExcDir || ExcDir->RelativeVirtualAddress == 0 || ExcDir->Size == 0)
    return;

  uintptr_t ExcPtr;
  if (auto Err = Obj.getRvaPtr(ExcDir->RelativeVirtualAddress, ExcPtr)) {
    llvm::consumeError(std::move(Err));
    Img.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Malformed;
    Img.ExceptionMetadata.Diagnostics.push_back(
        "x64 exception directory RVA is not file-backed");
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
    Img.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Malformed;
    Img.ExceptionMetadata.Diagnostics.push_back(
        "truncated or misaligned x64 exception directory");
  }
  const auto *RFBytes = reinterpret_cast<const uint8_t *>(ExcPtr);

  auto Seen = Img.getSymbolAddresses();

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
      Img.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Malformed;
      Img.ExceptionMetadata.Diagnostics.push_back(
          "x64 exception directory contains non-trailing zero padding");
      DiagnosedInteriorPadding = true;
    }
    uint64_t RecordRVA64 = uint64_t(ExcDir->RelativeVirtualAddress) +
                           uint64_t(I) * sizeof(RuntimeFunc);
    uint32_t RecordRVA = RecordRVA64 <= std::numeric_limits<uint32_t>::max()
                             ? static_cast<uint32_t>(RecordRVA64)
                             : 0;
    ExceptionFunction EF =
        decodeX64ExceptionFunction(Img, ImageBase, RecordRVA, RF.BeginAddress,
                                   RF.EndAddress, RF.UnwindInformation);
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, EF.ParseStatus);
    const bool HasRange = EF.CodeRange.isValid();
    const bool IsChained = EF.Kind == RuntimeFunctionKind::Chained;
    Img.ExceptionMetadata.Functions.push_back(std::move(EF));
    const ExceptionFunction &Stored = Img.ExceptionMetadata.Functions.back();
    if (!HasRange)
      continue;
    va_t Addr = Stored.CodeRange.Begin;
    va_t End = Stored.CodeRange.End;

    Img.KnownCodeRanges.emplace_back(Addr, End);

    if (IsChained)
      continue;
    if (!Seen.insert(Addr).second)
      continue;

    const auto *SegPtr = Img.getSegmentFor(Addr);
    if (!SegPtr || !SegPtr->isExecutable())
      continue;

    {
      size_t Off = static_cast<size_t>(Addr - SegPtr->VA);
      if (!checkPrologueAtOffset(*SegPtr, Off, Img.Arch))
        continue;
    }

    Img.Symbols.push_back(
        Symbol::makeFunc(Addr, RF.EndAddress - RF.BeginAddress));
    ++Added;
  }

  for (size_t I = 0; I < Img.ExceptionMetadata.Functions.size(); ++I) {
    ExceptionFunction &F = Img.ExceptionMetadata.Functions[I];
    if (F.Kind != RuntimeFunctionKind::Chained || !F.ChainedPrimaryRange)
      continue;
    for (size_t J = 0; J < Img.ExceptionMetadata.Functions.size(); ++J) {
      if (I == J)
        continue;
      const ExceptionFunction &Candidate = Img.ExceptionMetadata.Functions[J];
      if (Candidate.CodeRange.Begin == F.ChainedPrimaryRange->Begin &&
          Candidate.CodeRange.End == F.ChainedPrimaryRange->End &&
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
      Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
          Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
    }
  }

  constexpr unsigned MaxChainDepth = 32;
  for (size_t I = 0; I < Img.ExceptionMetadata.Functions.size(); ++I) {
    ExceptionFunction &Root = Img.ExceptionMetadata.Functions[I];
    if (Root.Kind != RuntimeFunctionKind::Chained || !Root.PrimaryFunctionIndex)
      continue;
    std::set<size_t> Visited;
    size_t Current = I;
    bool ReachedTerminal = false;
    for (unsigned Depth = 0; Depth < MaxChainDepth; ++Depth) {
      if (!Visited.insert(Current).second) {
        Root.ParseStatus = mergeExceptionParseStatus(
            Root.ParseStatus, ExceptionParseStatus::Malformed);
        Root.Diagnostics.push_back("chained x64 unwind graph is cyclic");
        break;
      }
      if (Current >= Img.ExceptionMetadata.Functions.size()) {
        Root.ParseStatus = mergeExceptionParseStatus(
            Root.ParseStatus, ExceptionParseStatus::Malformed);
        Root.Diagnostics.push_back("chained x64 unwind index is out of range");
        break;
      }
      const ExceptionFunction &CurrentFunction =
          Img.ExceptionMetadata.Functions[Current];
      if (CurrentFunction.Kind != RuntimeFunctionKind::Chained) {
        ReachedTerminal = true;
        break;
      }
      if (!CurrentFunction.PrimaryFunctionIndex)
        break;
      Current = *CurrentFunction.PrimaryFunctionIndex;
    }
    if (!ReachedTerminal &&
        Root.ParseStatus != ExceptionParseStatus::Malformed) {
      if (Visited.size() == MaxChainDepth) {
        Root.ParseStatus = ExceptionParseStatus::Malformed;
        Root.Diagnostics.push_back(
            "chained x64 unwind graph exceeds depth limit");
      } else {
        Root.ParseStatus = mergeExceptionParseStatus(
            Root.ParseStatus, ExceptionParseStatus::Partial);
        Root.Diagnostics.push_back(
            "chained x64 unwind graph does not reach a primary record");
      }
    }
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, Root.ParseStatus);
  }

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
  Img.ExceptionMetadata.rebuildIndex();
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Count
                          << " RUNTIME_FUNCTION entries (" << Added
                          << " new funcs)\n");
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
    Img.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Malformed;
    Img.ExceptionMetadata.Diagnostics.push_back(
        "ARM exception directory RVA is not file-backed");
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
    Img.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Malformed;
    Img.ExceptionMetadata.Diagnostics.push_back(
        "truncated or misaligned ARM exception directory");
  }
  const auto *RFBytes = reinterpret_cast<const uint8_t *>(ExcPtr);
  auto Seen = Img.getSymbolAddresses();
  const bool IsAArch64 = Img.Arch == Arch::AArch64;
  [[maybe_unused]] const char *ArchName = IsAArch64 ? "ARM64" : "ARM32";
  auto DiagnoseEntry = [&](size_t Index, llvm::StringRef Message) {
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Malformed);
    Img.ExceptionMetadata.Diagnostics.push_back(
        std::string(ArchName) + " pdata entry " + std::to_string(Index) + ": " +
        Message.str());
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
      Img.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Malformed;
      Img.ExceptionMetadata.Diagnostics.push_back(
          std::string(ArchName) +
          " exception directory contains non-trailing zero padding");
      DiagnosedInteriorPadding = true;
    }

    llvm::ARM::WinEH::RuntimeFunctionFlag Flag;
    uint32_t Length = 0;
    uint32_t XDataRVA = 0;
    va_t XDataVA = 0;
    va_t PersonalityVA = 0;
    va_t HandlerDataVA = 0;
    std::vector<uint8_t> NativeUnwindBytes;
    ExceptionParseStatus EntryStatus = ExceptionParseStatus::Complete;
    std::vector<std::string> EntryDiagnostics;
    auto DiagnoseUnwindBody = [&](llvm::StringRef Message) {
      DiagnoseEntry(I, Message);
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
      if (HasValidBody)
        NativeUnwindBytes.assign(XData, XData + StructuralBytes);
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
    } else {
      EF.Encoding = IsFragment ? ExceptionEncoding::ARM32PackedFragment
                               : ExceptionEncoding::ARM32Packed;
    }
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, EF.ParseStatus);
    const bool IsSymbolEligible =
        EF.ParseStatus == ExceptionParseStatus::Complete;
    Img.ExceptionMetadata.Functions.push_back(std::move(EF));

    Img.KnownCodeRanges.emplace_back(Addr, End);
    if (IsFragment || !IsSymbolEligible || !Seen.insert(Addr).second)
      continue;
    size_t Off = static_cast<size_t>(Addr - Seg->VA);
    if (!checkPrologueAtOffset(*Seg, Off, Img.Arch))
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(Addr, Length));
    ++Added;
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

void parseExceptions(const COFFObjectFile &Obj, BinaryImage &Img,
                     uint64_t ImageBase) {
  Img.ExceptionMetadata = ExceptionInfo{};
  if (const data_directory *ExcDir =
          Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE)) {
    Img.ExceptionMetadata.DirectoryRVA = ExcDir->RelativeVirtualAddress;
    Img.ExceptionMetadata.DirectorySize = ExcDir->Size;
  }
  if (Img.Arch == Arch::X64)
    parseX64Exceptions(Obj, Img, ImageBase);
  else if (Img.Arch == Arch::AArch64 || Img.Arch == Arch::ARM)
    parseARMExceptions(Obj, Img, ImageBase);
}

} // namespace coff_loader
} // namespace neverd
