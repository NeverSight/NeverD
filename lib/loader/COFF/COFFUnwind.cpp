//===- COFFUnwind.cpp - PE exception-table parsing ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/ISAEncoding.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <set>

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
  const auto *RFBytes = reinterpret_cast<const uint8_t *>(ExcPtr);

  auto IsChainedUnwind = [&](uint32_t UnwindRVA) -> bool {
    uintptr_t UPtr;
    if (auto Err = Obj.getRvaPtr(UnwindRVA, UPtr)) {
      llvm::consumeError(std::move(Err));
      return false;
    }
    if (UPtr < FileBegin || UPtr >= FileEnd)
      return false;
    uint8_t B = *reinterpret_cast<const uint8_t *>(UPtr);
    uint8_t Flags = (B >> unwind::kFlagsShift) & unwind::kFlagsMask;
    return (Flags & unwind::UNW_ChainInfo) != 0;
  };

  auto Seen = Img.getSymbolAddresses();

  [[maybe_unused]] size_t Added = 0;
  for (size_t I = 0; I < Count; ++I) {
    RuntimeFunc RF;
    std::memcpy(&RF, RFBytes + I * sizeof(RuntimeFunc), sizeof(RuntimeFunc));
    if (RF.BeginAddress == 0 && RF.EndAddress == 0)
      break;
    if (RF.EndAddress <= RF.BeginAddress)
      continue;
    if (RF.EndAddress > InvalidVA - ImageBase)
      continue;

    va_t Addr = ImageBase + RF.BeginAddress;
    va_t End = ImageBase + RF.EndAddress;

    Img.KnownCodeRanges.emplace_back(Addr, End);

    if (IsChainedUnwind(RF.UnwindInformation))
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

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
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
    return;
  }

  llvm::StringRef FileData = Obj.getData();
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(FileData.data());
  uintptr_t FileEnd = FileBegin + FileData.size();
  size_t AvailBytes =
      (ExcPtr >= FileBegin && ExcPtr <= FileEnd) ? (FileEnd - ExcPtr) : 0;
  constexpr size_t EntrySize = 2 * sizeof(uint32_t);
  size_t Count = std::min<size_t>(ExcDir->Size, AvailBytes) / EntrySize;
  const auto *RFBytes = reinterpret_cast<const uint8_t *>(ExcPtr);
  auto Seen = Img.getSymbolAddresses();
  const bool IsAArch64 = Img.Arch == Arch::AArch64;
  [[maybe_unused]] const char *ArchName = IsAArch64 ? "ARM64" : "ARM32";

  [[maybe_unused]] size_t Added = 0;
  for (size_t I = 0; I < Count; ++I) {
    const uint8_t *Entry = RFBytes + I * EntrySize;
    llvm::support::ulittle32_t Words[2];
    std::memcpy(Words, Entry, EntrySize);
    uint32_t BeginWord = Words[0];
    uint32_t UnwindWord = Words[1];
    if (BeginWord == 0 && UnwindWord == 0)
      break;

    llvm::ARM::WinEH::RuntimeFunctionFlag Flag;
    uint32_t Length = 0;
    uint32_t XDataRVA = 0;
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
      uintptr_t XDataPtr;
      if (auto Err = Obj.getRvaPtr(XDataRVA, XDataPtr)) {
        llvm::consumeError(std::move(Err));
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " has invalid xdata RVA\n");
        continue;
      }
      if (XDataPtr < FileBegin || XDataPtr > FileEnd ||
          static_cast<size_t>(FileEnd - XDataPtr) < sizeof(uint32_t)) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " has truncated xdata\n");
        continue;
      }
      llvm::support::ulittle32_t XDataWord;
      std::memcpy(&XDataWord, reinterpret_cast<const void *>(XDataPtr),
                  sizeof(XDataWord));
      llvm::ARM::WinEH::ExceptionDataRecord XR(&XDataWord, IsAArch64);
      if (XR.Vers() != 0) {
        LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                                << " has unsupported xdata version\n");
        continue;
      }
      if (IsAArch64) {
        Length = XR.FunctionLengthInBytesAArch64();
      } else {
        Length = XR.FunctionLengthInBytesARM();
        IsFragment = XR.F();
      }
    } else if (Flag == llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Reserved) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " uses reserved unwind flags\n");
      continue;
    }

    uint64_t Begin = normalizeCodeAddress(BeginWord, Img.Arch, Img.Mode);
    if (Length == 0 || Begin > InvalidVA - ImageBase) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " has invalid start or length\n");
      continue;
    }
    va_t Addr = ImageBase + Begin;
    if (Length > InvalidVA - Addr) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " range overflows\n");
      continue;
    }
    va_t End = Addr + Length;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg || !Seg->isExecutable()) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " does not start in executable data\n");
      continue;
    }
    uint64_t UsableSize = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (UsableSize > InvalidVA - Seg->VA || End > Seg->VA + UsableSize) {
      LLVM_DEBUG(llvm::dbgs() << "coff: " << ArchName << " pdata entry " << I
                              << " exceeds executable data\n");
      continue;
    }

    Img.KnownCodeRanges.emplace_back(Addr, End);
    if (IsFragment || !Seen.insert(Addr).second)
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
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Count << " " << ArchName
                          << " RUNTIME_FUNCTION entries (" << Added
                          << " new funcs)\n");
}

} // namespace

void parseExceptions(const COFFObjectFile &Obj, BinaryImage &Img,
                     uint64_t ImageBase) {
  if (Img.Arch == Arch::X64)
    parseX64Exceptions(Obj, Img, ImageBase);
  else if (Img.Arch == Arch::AArch64 || Img.Arch == Arch::ARM)
    parseARMExceptions(Obj, Img, ImageBase);
}

} // namespace coff_loader
} // namespace neverd
