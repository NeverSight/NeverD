//===- COFFUnwind.cpp - PE exception-table parsing ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/ISAEncoding.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/COFF/COFFUnwindARM.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
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
      if (HasValidBody) {
        NativeUnwindBytes.assign(XData, XData + StructuralBytes);
        UnwindCodeOffset = static_cast<size_t>((HeaderWords + EpilogueWords) * 4);
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
          Scopes[S] = llvm::support::ulittle32_t(
              readLE<uint32_t>(Structural.data() + EpilogueScopeOffset + S * 4));
        applyUnwindCodes(EF, IsAArch64,
                         Structural.slice(UnwindCodeOffset, UnwindCodeLength),
                         Scopes, SingleEpilogueIndex);
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
