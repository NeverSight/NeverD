//===- KernelReferenceImage.cpp - Original kernel code, modeled calls ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original kernel code, modeled calls.
///

#include "KernelReferenceImage.h"

#include "../driver/GuardFunctionTable.h"
#include "KernelHalDispatch.h"
#include "KernelNlsData.h"
#include "KernelObjectTypes.h"
#include "KernelProcessData.h"
#include "KernelProcessorData.h"
#include "KernelSystemVA.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/modules/ModuleSet.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <set>
namespace neverd::emulation {
namespace {
constexpr uint64_t ReferenceBase = 0xfffff80000000000;
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace
llvm::Expected<KernelReferenceImage>
KernelReferenceImage::load(const std::filesystem::path &Path,
                           uint64_t MemoryLimit) {
  return load(Path, MemoryLimit, {});
}
llvm::Expected<KernelReferenceImage>
KernelReferenceImage::load(const std::filesystem::path &Path,
                           uint64_t MemoryLimit,
                           const std::filesystem::path &PDBPath) {
  modules::ModuleOptions Inputs;
  std::error_code EC;
  const auto ImagePath = std::filesystem::absolute(Path, EC);
  if (EC)
    return llvm::createStringError(EC, "cannot resolve explicit kernel_image");
  Inputs.Modules.push_back({"kernel-reference", ImagePath});
  if (!PDBPath.empty()) {
    const auto DebugPath = std::filesystem::absolute(PDBPath, EC);
    if (EC)
      return llvm::createStringError(EC, "cannot resolve explicit kernel_pdb");
    Inputs.Modules.back().Debug =
        modules::DebugInput{modules::DebugKind::PDB, DebugPath};
  }
  auto Sources = modules::ModuleSet::create(std::move(Inputs));
  if (!Sources)
    return Sources.takeError();
  auto Source = (*Sources)->prepare("kernel-reference");
  if (!Source)
    return Source.takeError();
  return fromModule(**Source, MemoryLimit);
}
llvm::Expected<KernelReferenceImage>
KernelReferenceImage::fromModule(const modules::ModuleImage &Source,
                                 uint64_t MemoryLimit) {
  const auto *Image = &Source.image();
  if (Image->Arch != Arch::X64 || Image->Format != BinaryFormat::COFF ||
      !Image->Base || Image->Base >= ReferenceBase || Image->Exports.empty())
    return invalid("kernel_image requires an exported x64 PE image");
  const uint64_t Bias = ReferenceBase - Image->Base;
  KernelReferenceImage Result;
  auto Object = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Image->Raw.data()),
                      Image->Raw.size()),
      "kernel-reference"));
  if (!Object)
    return Object.takeError();
  const auto *Header = (*Object)->getPE32PlusHeader();
  if (!Header || !Header->SizeOfImage || !Header->SizeOfHeaders ||
      Header->SizeOfHeaders > Image->Raw.size() || Header->SizeOfImage % 4096 ||
      Header->SizeOfImage > MemoryLimit ||
      Header->SizeOfHeaders > Header->SizeOfImage)
    return invalid("invalid kernel_image PE headers");
  const uint64_t HeaderExtent =
      (uint64_t(Header->SizeOfHeaders) + 4095) & ~uint64_t(4095);
  std::vector<std::pair<uint64_t, uint64_t>> Extents;
  for (const auto &Segment : Image->Segments) {
    const uint64_t Extent = std::max(Segment.Size, Segment.FileSz);
    if (!Extent || Segment.VA < Image->Base ||
        Segment.VA - Image->Base < HeaderExtent ||
        Segment.VA - Image->Base >= Header->SizeOfImage ||
        Extent > Header->SizeOfImage - (Segment.VA - Image->Base))
      return invalid("kernel_image section exceeds its declared image");
    Extents.emplace_back(Segment.VA, Segment.VA + Extent);
  }
  std::sort(Extents.begin(), Extents.end());
  for (size_t I = 1; I < Extents.size(); ++I)
    if (Extents[I].first < Extents[I - 1].second)
      return invalid("kernel_image contains overlapping sections");
  Result.Base = ReferenceBase;
  Result.Size = Header->SizeOfImage;
  if (!Image->InputFileSHA256)
    return invalid("kernel_image has no authenticated input digest");
  Result.Digest = llvm::toHex(llvm::ArrayRef(*Image->InputFileSHA256), true);
  if (Source.debug()) {
    if (Source.debugKind() != modules::DebugKind::PDB ||
        Source.debugDigest().empty())
      return invalid(
          "kernel reference layout facts require an authenticated PDB");
    // ModuleSet already verifies the PDB build identity. Bounded kernel facts
    // are proved from the original bytes below; this does not grant general
    // function-signature or object-extent trust to a names-only PDB provider.
    const auto *Debug = Source.debug();
    Result.PDBDigest = Source.debugDigest();
    std::map<std::string, uint64_t> Symbols;
    for (const auto &Symbol : Debug->allDataObjects()) {
      auto [Found, New] = Symbols.emplace(Symbol.Name, Symbol.Addr);
      if (!New && Found->second != Symbol.Addr)
        return invalid("ambiguous kernel_pdb data symbol");
    }
    auto InSection = [&](uint64_t Address, uint64_t Length, bool Code) {
      for (const auto &Segment : Image->Segments) {
        if (Segment.isExecutable() != Code || Address < Segment.VA)
          continue;
        const auto Size = std::max(Segment.Size, Segment.FileSz);
        if (Length <= Size && Address - Segment.VA <= Size - Length)
          return true;
      }
      return false;
    };
    auto Require = [&](llvm::StringRef Name,
                       uint64_t Size) -> llvm::Expected<uint64_t> {
      auto Found = Symbols.find(Name.str());
      if (Found == Symbols.end() || Found->second < Image->Base ||
          Found->second - Image->Base >= Result.Size ||
          !InSection(Found->second, Size, false))
        return invalid("kernel_pdb lacks required data symbol: " + Name.str());
      return Found->second;
    };
    auto ReadOriginal =
        [&](uint64_t Address,
            uint64_t Length) -> llvm::Expected<std::vector<uint8_t>> {
      for (const auto &Segment : Image->Segments) {
        if (Address < Segment.VA)
          continue;
        const auto Offset = Address - Segment.VA;
        if (Offset > Segment.FileSz || Length > Segment.FileSz - Offset ||
            Segment.FileOff > Image->Raw.size() ||
            Segment.FileSz > Image->Raw.size() - Segment.FileOff)
          continue;
        const auto Begin = Image->Raw.begin() + Segment.FileOff + Offset;
        return std::vector<uint8_t>(Begin, Begin + Length);
      }
      return invalid(
          "kernel debugger facts require original file-backed bytes");
    };
    auto Address = Require("KdDebuggerDataBlock", 0x380);
    auto List = Require("KdpDebuggerDataListHead", 16);
    auto LoaderBlock = Require("KdpLoaderDebuggerBlock", 8);
    if (!Address || !List || !LoaderBlock)
      return llvm::joinErrors(
          llvm::joinErrors(Address.takeError(), List.takeError()),
          LoaderBlock.takeError());
    auto Bytes = ReadOriginal(*Address, 0x380);
    if (!Bytes)
      return Bytes.takeError();
    DebuggerFacts Facts;
    Facts.Template = std::move(*Bytes);
    Facts.Address = *Address + Bias;
    Facts.ListHead = *List + Bias;
    Facts.LoaderBlock = *LoaderBlock + Bias;
    if (Symbols.contains("HalpPciMcfgTableCount") ||
        Symbols.contains("HalpPciMcfgTable")) {
      auto Count = Require("HalpPciMcfgTableCount", 4);
      auto Pointer = Require("HalpPciMcfgTable", 8);
      if (!Count || !Pointer)
        return llvm::joinErrors(Count.takeError(), Pointer.takeError());
      if (*Count % 4 || *Pointer % 8 ||
          (*Count < *Pointer + 8 && *Pointer < *Count + 4))
        return invalid("invalid kernel_pdb MCFG field alignment or overlap");
      Facts.PciMcfgCount = *Count + Bias;
      Facts.PciMcfgPointer = *Pointer + Bias;
    }
    Facts.ProcessSize =
        llvm::support::endian::read16le(Facts.Template.data() + 0x2a8);
    Facts.ThreadSize =
        llvm::support::endian::read16le(Facts.Template.data() + 0x2c0);
    const auto OwnerTag =
        llvm::support::endian::read32le(Facts.Template.data() + 0x10);
    const auto DeclaredSize =
        llvm::support::endian::read32le(Facts.Template.data() + 0x14);
    if ((OwnerTag && OwnerTag != 0x4742444b) ||
        (DeclaredSize && DeclaredSize != 0x380) || Facts.ProcessSize < 0x800 ||
        Facts.ThreadSize < 0x800 || Facts.ProcessSize % 8 ||
        Facts.ThreadSize % 8 || Facts.ProcessSize > 0x10000 ||
        Facts.ThreadSize > 0x10000)
      return invalid(
          "kernel_pdb debugger layout is outside the x64 19041 profile");
    // PE names retain folded aliases which the PDB function index may merge.
    for (const auto &Export : Image->Exports) {
      uint32_t *ThreadField = nullptr;
      if (Export.Name == "PsGetThreadId")
        ThreadField = &Facts.ThreadIDOffset;
      else if (Export.Name == "PsGetThreadProcessId")
        ThreadField = &Facts.ThreadProcessIDOffset;
      else if (Export.Name == "PsGetThreadProcess" ||
               Export.Name == "IoThreadToProcess")
        ThreadField = &Facts.ThreadProcessOffset;
      else if (Export.Name == "PsGetThreadWin32Thread")
        ThreadField = &Facts.ThreadWin32ThreadOffset;
      if (Export.Name == "PsGetCurrentThreadWin32Thread") {
        if (!InSection(Export.Addr, 17, true))
          return invalid("invalid current thread Win32 accessor");
        auto Code = ReadOriginal(Export.Addr, 17);
        if (!Code)
          return Code.takeError();
        const uint8_t Prefix[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x88,
                                  0x01, 0x00, 0x00, 0x48, 0x8b, 0x80};
        if (!std::equal(std::begin(Prefix), std::end(Prefix), Code->begin()) ||
            (*Code)[16] != 0xc3)
          return invalid("unsupported current thread Win32 accessor shape");
        const auto Offset = llvm::support::endian::read32le(Code->data() + 12);
        if (!Offset || Offset % 8 || Offset > Facts.ThreadSize - 8 ||
            (Facts.ThreadWin32ThreadOffset &&
             Facts.ThreadWin32ThreadOffset != Offset))
          return invalid("thread Win32 field exceeds or disagrees with "
                         "authenticated thread layout");
        Facts.ThreadWin32ThreadOffset = Offset;
      }
      if (ThreadField) {
        if (!InSection(Export.Addr, 8, true))
          return invalid("invalid thread identity accessor");
        auto Code = ReadOriginal(Export.Addr, 8);
        if (!Code)
          return Code.takeError();
        if ((*Code)[0] != 0x48 || (*Code)[1] != 0x8b || (*Code)[2] != 0x81 ||
            (*Code)[7] != 0xc3)
          return invalid("unsupported thread identity accessor shape");
        const auto Offset = llvm::support::endian::read32le(Code->data() + 3);
        if (!Offset || Offset % 8 || Offset > Facts.ThreadSize - 8 ||
            (*ThreadField && *ThreadField != Offset))
          return invalid("thread identity field exceeds or disagrees with "
                         "authenticated thread layout");
        *ThreadField = Offset;
      }
    }
    uint64_t HalSetup = 0, HalDispatch = 0, HalLookup = 0;
    for (const auto &F : Debug->allFunctions()) {
      uint64_t *Target = F.Name == "HalpSetupAcpiPhase0"       ? &HalSetup
                         : F.Name == "HalAcpiGetTableDispatch" ? &HalDispatch
                         : F.Name == "HalpAcpiGetTable"        ? &HalLookup
                                                               : nullptr;
      if (!Target)
        continue;
      if ((*Target && *Target != F.Addr) || !InSection(F.Addr, 1, true))
        return invalid("invalid or ambiguous HAL ACPI routine");
      *Target = F.Addr;
    }
    if (HalSetup || HalDispatch || HalLookup) {
      auto Table = Require("HalDispatchTable", hal_dispatch::Bytes);
      if (!Table)
        return Table.takeError();
      if (!HalSetup || !HalDispatch || !HalLookup ||
          !InSection(HalSetup, 0x18e, true) ||
          !InSection(HalDispatch, 24, true))
        return invalid("HAL ACPI lacks authenticated initialization routines");
      auto Boot = ReadOriginal(HalSetup + 0x179, 21),
           Call = ReadOriginal(HalDispatch, 24),
           Version = ReadOriginal(*Table, 4);
      if (!Boot || !Call || !Version)
        return llvm::joinErrors(
            llvm::joinErrors(Boot.takeError(), Call.takeError()),
            Version.takeError());
      const uint8_t Prefix[] = {0x48, 0x83, 0xec, 0x28, 0x4d, 0x8b, 0xc8, 0x4c,
                                0x8b, 0xc2, 0x8b, 0xd1, 0x33, 0xc9, 0xe8};
      const uint8_t Suffix[] = {0x48, 0x83, 0xc4, 0x28, 0xc3};
      using llvm::support::endian::read32le;
      if (!std::equal(std::begin(Prefix), std::end(Prefix), Call->begin()) ||
          !std::equal(std::begin(Suffix), std::end(Suffix),
                      Call->begin() + 19) ||
          HalDispatch + 19 + int32_t(read32le(Call->data() + 15)) !=
              HalLookup ||
          read32le(Version->data()) != 4 || (*Boot)[0] != 0x48 ||
          (*Boot)[1] != 0x8d || (*Boot)[2] != 0x05 || (*Boot)[7] != 0x40 ||
          (*Boot)[8] != 0x88 || (*Boot)[9] != 0x2d || (*Boot)[14] != 0x48 ||
          (*Boot)[15] != 0x89 || (*Boot)[16] != 0x05 ||
          HalSetup + 0x180 + int32_t(read32le(Boot->data() + 3)) !=
              HalDispatch ||
          HalSetup + 0x18e + int32_t(read32le(Boot->data() + 17)) !=
              *Table + hal_dispatch::CacheOffset)
        return invalid(
            "unsupported original HAL ACPI dispatch initialization shape");
      Facts.HalCacheDispatch = HalDispatch + Bias;
    }
    bool CommitFound = false;
    // Windows 19041's file table contains RVAs. Boot compacts each entry to
    // a signed relative displacement with a four-bit stack-argument count.
    // Authenticate both original boot routines before publishing runtime data.
    if (Symbols.contains("KeServiceDescriptorTable") ||
        Symbols.contains("KiServiceTable") ||
        Symbols.contains("KiServiceLimit") ||
        Symbols.contains("KiArgumentTable")) {
      auto Descriptor = Require("KeServiceDescriptorTable", 32);
      auto Table = Require("KiServiceTable", 4);
      auto Limit = Require("KiServiceLimit", 4);
      auto Arguments = Require("KiArgumentTable", 1);
      if (!Descriptor || !Table || !Limit || !Arguments)
        return llvm::joinErrors(
            llvm::joinErrors(Descriptor.takeError(), Table.takeError()),
            llvm::joinErrors(Limit.takeError(), Arguments.takeError()));
      auto CountBytes = ReadOriginal(*Limit, 4);
      auto InitialDescriptor = ReadOriginal(*Descriptor, 32);
      if (!CountBytes || !InitialDescriptor)
        return llvm::joinErrors(CountBytes.takeError(),
                                InitialDescriptor.takeError());
      const uint32_t Count =
          llvm::support::endian::read32le(CountBytes->data());
      if (!Count || Count > 4096 || *Descriptor % 16 || *Table % 4 ||
          !InSection(*Table, Count * 4, false) ||
          !InSection(*Arguments, Count, false) ||
          std::any_of(InitialDescriptor->begin(), InitialDescriptor->end(),
                      [](uint8_t Byte) { return Byte != 0; }))
        return invalid("unsupported primary service table layout");
      std::vector<std::pair<uint64_t, uint64_t>> ServiceRanges{
          {*Descriptor, *Descriptor + 32},
          {*Table, *Table + Count * 4},
          {*Limit, *Limit + 4},
          {*Arguments, *Arguments + Count}};
      std::sort(ServiceRanges.begin(), ServiceRanges.end());
      for (unsigned I = 1; I < ServiceRanges.size(); ++I)
        if (ServiceRanges[I].first < ServiceRanges[I - 1].second)
          return invalid("overlapping primary service table inputs");
      for (const auto &Segment : Image->Segments)
        if (Segment.isWritable() &&
            ((*Table >= Segment.VA && *Table - Segment.VA < Segment.Size) ||
             (*Arguments >= Segment.VA &&
              *Arguments - Segment.VA < Segment.Size)))
          return invalid("service table inputs require read-only storage");
      uint64_t Compactor = 0, Boot = 0, Lock = 0;
      for (const auto &Function : Debug->allFunctions()) {
        uint64_t *Slot = Function.Name == "KeCompactServiceTable" ? &Compactor
                         : Function.Name == "KiInitSystem"        ? &Boot
                         : Function.Name == "KiLockServiceTable"  ? &Lock
                                                                  : nullptr;
        if (Slot) {
          if (*Slot || !InSection(Function.Addr, 1, true))
            return invalid("invalid or ambiguous service boot routine");
          *Slot = Function.Addr;
        }
      }
      if (!Compactor || !Boot || !Lock || !InSection(Compactor, 86, true) ||
          !InSection(Boot + 0xe6, 47, true))
        return invalid("service table lacks authenticated boot routines");
      auto CompactCode = ReadOriginal(Compactor, 86);
      auto BootCode = ReadOriginal(Boot + 0xe6, 47);
      if (!CompactCode || !BootCode)
        return llvm::joinErrors(CompactCode.takeError(), BootCode.takeError());
      const uint8_t CompactShape[] = {
          0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x45,
          0x8b, 0xd0, 0x48, 0x8b, 0xda, 0x4c, 0x8b, 0xd9, 0x45, 0x85, 0xc0,
          0x74, 0x2b, 0x41, 0x8b, 0xfa, 0x45, 0x8b, 0x03, 0x0f, 0xb6, 0x03,
          0x44, 0x2b, 0xc1, 0x44, 0x03, 0x44, 0x24, 0x50, 0x48, 0xff, 0xc3,
          0x41, 0xc1, 0xe0, 0x04, 0xc1, 0xe8, 0x02, 0x44, 0x0b, 0xc0, 0x45,
          0x89, 0x03, 0x4d, 0x8d, 0x5b, 0x04, 0x48, 0x83, 0xef, 0x01, 0x75,
          0xd8, 0x45, 0x8b, 0xc2, 0xe8};
      const uint8_t CompactReturn[] = {0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48,
                                       0x83, 0xc4, 0x20, 0x5f, 0xc3};
      if (!std::equal(std::begin(CompactShape), std::end(CompactShape),
                      CompactCode->begin()) ||
          !std::equal(std::begin(CompactReturn), std::end(CompactReturn),
                      CompactCode->begin() + 75) ||
          Compactor + 75 +
                  int64_t(int32_t(llvm::support::endian::read32le(
                      CompactCode->data() + 71))) !=
              Lock)
        return invalid("unsupported service compaction semantics");
      auto RIP = [&](unsigned At, std::initializer_list<uint8_t> Opcode,
                     uint64_t Target) {
        const unsigned End = At + Opcode.size() + 4;
        return std::equal(Opcode.begin(), Opcode.end(),
                          BootCode->begin() + At) &&
               Boot + 0xe6 + End +
                       int64_t(int32_t(llvm::support::endian::read32le(
                           BootCode->data() + End - 4))) ==
                   Target;
      };
      if (!RIP(0, {0x48, 0x8d, 0x05}, *Table) ||
          !RIP(7, {0x48, 0x89, 0x05}, *Descriptor) ||
          !RIP(14, {0x8b, 0x05}, *Limit) ||
          !RIP(20, {0x0f, 0x28, 0x1d}, *Descriptor) ||
          !RIP(27, {0x89, 0x05}, *Descriptor + 16) ||
          !RIP(33, {0x48, 0x8d, 0x05}, *Arguments) ||
          !RIP(40, {0x48, 0x89, 0x05}, *Descriptor + 24))
        return invalid("primary service descriptor disagrees with boot code");
      auto Raw = ReadOriginal(*Table, Count * 4);
      auto Args = ReadOriginal(*Arguments, Count);
      if (!Raw || !Args)
        return llvm::joinErrors(Raw.takeError(), Args.takeError());
      for (unsigned I = 0; I < Count; ++I) {
        const auto RVA = llvm::support::endian::read32le(Raw->data() + I * 4);
        const int64_t Delta = int64_t(RVA) - int64_t(*Table - Image->Base);
        const auto Argument = (*Args)[I];
        if (RVA >= Result.Size || !InSection(Image->Base + RVA, 1, true) ||
            Delta < -(1LL << 27) || Delta >= (1LL << 27) || Argument % 4 ||
            Argument > 60)
          return invalid("invalid service target or stack-argument encoding");
        llvm::support::endian::write32le(
            Raw->data() + I * 4, uint32_t(Delta * 16) | (Argument >> 2));
      }
      Facts.ServiceDescriptor = *Descriptor + Bias;
      Facts.ServiceTable = *Table + Bias;
      Facts.ServiceArguments = *Arguments + Bias;
      Facts.CompactedServices = std::move(*Raw);
      Facts.ServiceArgumentBytes = std::move(*Args);
    }
    for (const auto &Function : Debug->allFunctions()) {
      if (Function.Name == "KiSystemCall64") {
        if (Facts.UnifiedSystemCallEntry || !InSection(Function.Addr, 12, true))
          return invalid("invalid or ambiguous unified system-call entry");
        auto Code = ReadOriginal(Function.Addr, 12);
        if (!Code)
          return Code.takeError();
        // SWAPGS; MOV GS:[0x10], RSP. The shadow entry saves to GS:[0x9010]
        // and is not the LSTAR source for the unified guest address space.
        const uint8_t Prefix[] = {0x0f, 0x01, 0xf8, 0x65, 0x48, 0x89,
                                  0x24, 0x25, 0x10, 0x00, 0x00, 0x00};
        if (!std::equal(std::begin(Prefix), std::end(Prefix), Code->begin()))
          return invalid("unsupported unified system-call entry shape");
        Facts.UnifiedSystemCallEntry = Function.Addr + Bias;
      }
      if (Function.Name == "PsQueryThreadStartAddress") {
        if (Facts.ThreadWin32StartOffset || !InSection(Function.Addr, 27, true))
          return invalid("invalid or ambiguous thread start accessor");
        auto Code = ReadOriginal(Function.Addr, 27);
        if (!Code)
          return Code.takeError();
        // The system-thread branch and edx == 0 branch return this same
        // field. Other branches are not interpreted as Win32StartAddress.
        const uint8_t Prefix[] = {
            0xf7, 0x41, 0x74, 0x00, 0x04, 0x00, 0x00, 0x4c, 0x8b, 0xc9, 0x75,
            0x07, 0x45, 0x33, 0xc0, 0x85, 0xd2, 0x75, 0x09, 0x48, 0x8b, 0x81};
        if (!std::equal(std::begin(Prefix), std::end(Prefix), Code->begin()) ||
            (*Code)[26] != 0xc3)
          return invalid("unsupported thread start accessor shape");
        const auto Offset = llvm::support::endian::read32le(Code->data() + 22);
        if (!Offset || Offset % 8 || Offset > Facts.ThreadSize - 8)
          return invalid("thread start field exceeds authenticated layout");
        Facts.ThreadWin32StartOffset = Offset;
      }
      if (Function.Name == "PsGetProcessId") {
        if (Facts.ProcessIDOffset || !InSection(Function.Addr, 8, true))
          return invalid("invalid or ambiguous process ID accessor");
        auto Code = ReadOriginal(Function.Addr, 8);
        if (!Code)
          return Code.takeError();
        if ((*Code)[0] != 0x48 || (*Code)[1] != 0x8b || (*Code)[2] != 0x81 ||
            (*Code)[7] != 0xc3)
          return invalid("unsupported process ID accessor shape");
        const auto Offset = llvm::support::endian::read32le(Code->data() + 3);
        if (!Offset || Offset % 8 || Offset > Facts.ProcessSize - 8)
          return invalid(
              "process ID field exceeds authenticated process layout");
        Facts.ProcessIDOffset = Offset;
      }
      if (Function.Name == "PsGetNextProcess" &&
          Symbols.contains("PsActiveProcessHead")) {
        if (Facts.ActiveProcessHead || !InSection(Function.Addr, 100, true))
          return invalid("invalid or ambiguous active process accessor");
        auto Code = ReadOriginal(Function.Addr, 100);
        if (!Code)
          return Code.takeError();
        auto Match = [&](unsigned At, std::initializer_list<uint8_t> Bytes) {
          return std::equal(Bytes.begin(), Bytes.end(), Code->begin() + At);
        };
        if (!Match(62, {0x48, 0x8b, 0x1d}) ||
            !Match(69, {0x48, 0x85, 0xff, 0x74, 0x07, 0x48, 0x8b, 0x9f}) ||
            !Match(81, {0x4c, 0x8d, 0x3d}) ||
            !Match(88, {0x49, 0x3b, 0xdf, 0x74}) ||
            !Match(93, {0x4c, 0x8d, 0xb3}))
          return invalid("unsupported active process accessor shape");
        auto Head = Require("PsActiveProcessHead", 16);
        if (!Head)
          return Head.takeError();
        const int32_t First =
            llvm::support::endian::read32le(Code->data() + 65);
        const int32_t Second =
            llvm::support::endian::read32le(Code->data() + 84);
        const uint32_t Links =
            llvm::support::endian::read32le(Code->data() + 77);
        const int32_t Subtract =
            llvm::support::endian::read32le(Code->data() + 96);
        if (*Head % 8 || *Head != Function.Addr + 69 + int64_t(First) ||
            *Head != Function.Addr + 88 + int64_t(Second) || !Links ||
            Links % 8 || Links > Facts.ProcessSize - 16 ||
            int64_t(Subtract) != -int64_t(Links))
          return invalid(
              "active process list disagrees with authenticated accessor");
        Facts.ActiveProcessHead = *Head + Bias;
        Facts.ActiveProcessLinksOffset = Links;
      }
      if (Function.Name == "_guard_dispatch_icall" &&
          Symbols.contains("_guard_icall_bitmap")) {
        if (Facts.GuardBitmapPointer || !InSection(Function.Addr, 49, true))
          return invalid("invalid or ambiguous kernel CFG bitmap accessor");
        auto Code = ReadOriginal(Function.Addr, 49);
        if (!Code)
          return Code.takeError();
        const uint8_t Load[] = {0x4c, 0x8b, 0x1d};
        const uint8_t Test[] = {0x48, 0x85, 0xc0};
        const uint8_t Index[] = {0x4c, 0x8b, 0xd0, 0x49, 0xc1, 0xea, 0x09,
                                 0x4f, 0x8b, 0x1c, 0xd3, 0x4c, 0x8b, 0xd0,
                                 0x49, 0xc1, 0xea, 0x03, 0xa8, 0x0f};
        const uint8_t BitTest[] = {0x4d, 0x0f, 0xa3, 0xd3};
        if (!std::equal(std::begin(Load), std::end(Load), Code->begin()) ||
            !std::equal(std::begin(Test), std::end(Test), Code->begin() + 7) ||
            (*Code)[10] != 0x0f || (*Code)[11] != 0x8d || (*Code)[16] != 0x4d ||
            (*Code)[17] != 0x85 || (*Code)[18] != 0xdb || (*Code)[19] != 0x74 ||
            !std::equal(std::begin(Index), std::end(Index),
                        Code->begin() + 21) ||
            (*Code)[41] != 0x75 ||
            !std::equal(std::begin(BitTest), std::end(BitTest),
                        Code->begin() + 43) ||
            (*Code)[47] != 0x73)
          return invalid("unsupported kernel CFG bitmap accessor shape");
        const int32_t Offset =
            llvm::support::endian::read32le(Code->data() + 3);
        auto Slot = Require("_guard_icall_bitmap", 8);
        if (!Slot)
          return Slot.takeError();
        if (*Slot % 8 || *Slot != Function.Addr + 7 + int64_t(Offset))
          return invalid("kernel CFG bitmap symbol disagrees with accessor");
        Facts.GuardBitmapPointer = *Slot + Bias;
      }
      if (Function.Name == "MmUnsecureVirtualMemory") {
        if (Facts.SecureCookie || !InSection(Function.Addr, 41, true))
          return invalid("invalid or ambiguous secure-memory cookie accessor");
        auto Code = ReadOriginal(Function.Addr, 41);
        if (!Code)
          return Code.takeError();
        const uint8_t Prefix[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x88,
                                  0x01, 0x00, 0x00, 0x48, 0x8d, 0x54,
                                  0x24, 0x30, 0x48, 0x8b, 0x98, 0xb8,
                                  0x00, 0x00, 0x00, 0x48, 0x33, 0x1d};
        const uint8_t Suffix[] = {0x48, 0x33, 0xd9};
        if (!std::equal(std::begin(Prefix), std::end(Prefix),
                        Code->begin() + 10) ||
            !std::equal(std::begin(Suffix), std::end(Suffix),
                        Code->begin() + 38))
          return invalid("unsupported MmUnsecureVirtualMemory reference shape");
        const int32_t Offset =
            llvm::support::endian::read32le(Code->data() + 34);
        const uint64_t Target = Function.Addr + 38 + int64_t(Offset);
        if (Target < Image->Base || Target - Image->Base > Result.Size - 8 ||
            !InSection(Target, 8, false))
          return invalid("secure-memory cookie exceeds reference data");
        Facts.SecureCookie = Target + Bias;
      }
      if (Function.Name == "MiQuerySystemBase") {
        if (Facts.SystemVaAssignments || !InSection(Function.Addr, 18, true))
          return invalid("invalid or ambiguous system VA base accessor");
        auto Code = ReadOriginal(Function.Addr, 18);
        if (!Code)
          return Code.takeError();
        const uint8_t Prefix[] = {0x48, 0x63, 0xc1, 0x48, 0x8d, 0x0d};
        const uint8_t Suffix[] = {0x48, 0x03, 0xc0, 0x48,
                                  0x8b, 0x04, 0xc1, 0xc3};
        if (!std::equal(std::begin(Prefix), std::end(Prefix), Code->begin()) ||
            !std::equal(std::begin(Suffix), std::end(Suffix),
                        Code->begin() + 10))
          return invalid("unsupported MiQuerySystemBase reference shape");
        const int32_t Offset =
            llvm::support::endian::read32le(Code->data() + 6);
        const uint64_t Target = Function.Addr + 10 + int64_t(Offset);
        if (Target < Image->Base ||
            Target - Image->Base > Result.Size - systemva::AssignmentBytes ||
            !InSection(Target, systemva::AssignmentBytes, false))
          return invalid("system VA assignment prefix exceeds reference data");
        Facts.SystemVaAssignments = Target + Bias;
      }
      if (Function.Name != "MmGetPagedPoolCommitPointer")
        continue;
      if (!InSection(Function.Addr, 8, true))
        return invalid(
            "kernel pool commit accessor is outside executable code");
      auto Code = ReadOriginal(Function.Addr, 8);
      if (!Code)
        return Code.takeError();
      if ((*Code)[0] != 0x48 || (*Code)[1] != 0x8d || (*Code)[2] != 0x05 ||
          (*Code)[7] != 0xc3)
        return invalid(
            "unsupported MmGetPagedPoolCommitPointer reference shape");
      const int32_t Displacement =
          llvm::support::endian::read32le(Code->data() + 3);
      const uint64_t Target = Function.Addr + 7 + int64_t(Displacement);
      if (Target < Image->Base || Target - Image->Base >= Result.Size - 8 ||
          !InSection(Target, 8, false))
        return invalid(
            "kernel paged-pool commit address exceeds reference image");
      if (CommitFound)
        return invalid("ambiguous kernel paged-pool commit accessor");
      Facts.PagedPoolCommit = Target + Bias;
      CommitFound = true;
    }
    if (!CommitFound)
      return invalid("kernel_pdb lacks MmGetPagedPoolCommitPointer");
    std::set<uint32_t> ThreadOffsets;
    for (auto Offset : {Facts.ThreadIDOffset, Facts.ThreadProcessIDOffset,
                        Facts.ThreadProcessOffset, Facts.ThreadWin32StartOffset,
                        Facts.ThreadWin32ThreadOffset})
      if (Offset && !ThreadOffsets.insert(Offset).second)
        return invalid("overlapping authenticated thread fields");
    if (Facts.ProcessIDOffset && Facts.ActiveProcessLinksOffset &&
        Facts.ProcessIDOffset < Facts.ActiveProcessLinksOffset + 16 &&
        Facts.ActiveProcessLinksOffset < Facts.ProcessIDOffset + 8)
      return invalid("overlapping authenticated process fields");
    if (Facts.GuardBitmapPointer) {
      const auto *Directory =
          (*Object)->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
      using Config = llvm::object::coff_load_configuration64;
      constexpr auto RequiredSize = offsetof(Config, GuardFlags) + 4;
      if (!Directory || Directory->Size < RequiredSize)
        return invalid(
            "kernel CFG bitmap requires a complete load configuration");
      const uint64_t ConfigAddress =
          Image->Base + Directory->RelativeVirtualAddress;
      auto Configuration = ReadOriginal(ConfigAddress, RequiredSize);
      if (!Configuration)
        return Configuration.takeError();
      const auto *C = Configuration->data();
      const uint32_t DeclaredSize = llvm::support::endian::read32le(C);
      const uint32_t Flags =
          llvm::support::endian::read32le(C + offsetof(Config, GuardFlags));
      // Observation supports ordinary 19041 CFG/ES metadata. Other execution
      // mitigations are neither enabled nor emulated by reading this table.
      constexpr uint32_t Known =
          guard::Instrumented | guard::FunctionTablePresent |
          guard::SecurityCookieUnused | guard::LongJumpTablePresent |
          guard::FunctionTableSizeMask | guard::ExportSuppressionInfo |
          guard::EnableExportSuppression | guard::ProtectDelayLoadIAT |
          guard::DelayLoadIATOwnSection | guard::RetpolinePresent;
      if (DeclaredSize < RequiredSize || DeclaredSize > Directory->Size ||
          (Flags & ~Known) || !(Flags & guard::FunctionTablePresent) ||
          !(Flags & guard::Instrumented))
        return invalid("unsupported reference kernel CFG metadata policy");
      if ((Flags & guard::EnableExportSuppression) &&
          !(Flags & guard::ExportSuppressionInfo))
        return invalid(
            "reference kernel export suppression lacks metadata flag");
      auto CompleteConfiguration = ReadOriginal(ConfigAddress, DeclaredSize);
      if (!CompleteConfiguration)
        return CompleteConfiguration.takeError();
      const uint64_t Table = llvm::support::endian::read64le(
          C + offsetof(Config, GuardCFFunctionTable));
      const uint64_t Count = llvm::support::endian::read64le(
          C + offsetof(Config, GuardCFFunctionCount));
      const unsigned Extra = (Flags & guard::FunctionTableSizeMask) >>
                             guard::FunctionTableSizeShift;
      if (!Count || Count > guard::MaximumTargets || Extra > 1 ||
          Table < Image->Base || !InSection(Table, Count * (4 + Extra), false))
        return invalid("invalid reference kernel CFG function table");
      for (const auto &Segment : Image->Segments)
        if (Table >= Segment.VA &&
            Table - Segment.VA < std::max(Segment.Size, Segment.FileSz) &&
            (Segment.isWritable() || !Segment.isReadable()))
          return invalid(
              "reference kernel CFG function table must be read-only");
      auto Records = ReadOriginal(Table, Count * (4 + Extra));
      if (!Records)
        return Records.takeError();
      auto Targets = guard::decodeFunctionTable(
          *Records, Extra, guard::FunctionSuppressed | guard::ExportSuppressed,
          [&](uint32_t RVA) {
            if (!InSection(Image->Base + RVA, 1, true))
              return false;
            for (const auto &Segment : Image->Segments)
              if (Image->Base + RVA >= Segment.VA &&
                  Image->Base + RVA - Segment.VA < Segment.FileSz)
                return true;
            return false;
          });
      if (!Targets)
        return Targets.takeError();
      for (const auto &Target : *Targets) {
        if ((Target.Flags & guard::ExportSuppressed) &&
            !(Flags & guard::ExportSuppressionInfo))
          return invalid(
              "reference kernel export-suppressed target lacks metadata flag");
        if (Target.Flags & guard::FunctionSuppressed)
          continue;
        if ((Flags & guard::EnableExportSuppression) &&
            (Target.Flags & guard::ExportSuppressed))
          Facts.SuppressedExports.push_back(ReferenceBase + Target.RVA);
        else
          Facts.GuardTargets.push_back(ReferenceBase + Target.RVA);
      }
    }
    std::set<uint64_t> Relocated;
    std::map<std::string, uint64_t> ThreadCode;
    for (const auto &Function : Debug->allFunctions()) {
      bool Required = false;
#define NEVERD_THREAD_REFERENCE_CODE(SymbolName)                               \
  Required |= Function.Name == #SymbolName;
#define NEVERD_THREAD_REFERENCE_DATA(Name, Size)
#include "KernelThreadReferenceSymbols.def"
#undef NEVERD_THREAD_REFERENCE_CODE
#undef NEVERD_THREAD_REFERENCE_DATA
      if (!Required)
        continue;
      auto [I, New] = ThreadCode.emplace(Function.Name, Function.Addr);
      if ((!New && I->second != Function.Addr) ||
          !InSection(Function.Addr, 1, true))
        return invalid("invalid or ambiguous thread initializer code");
    }
    if (ThreadCode.contains("PspAllocateThread") &&
        ThreadCode.contains("PspInsertThread")) {
#define NEVERD_THREAD_REFERENCE_CODE(Name)                                     \
  if (!ThreadCode.contains(#Name))                                             \
    return invalid("missing original thread initialization routine: " #Name);  \
  Result.ThreadInitializationSymbols.emplace(#Name,                            \
                                             ThreadCode.at(#Name) + Bias);
#define NEVERD_THREAD_REFERENCE_DATA(Name, Size)                               \
  {                                                                            \
    auto Slot = Require(#Name, Size);                                          \
    if (!Slot)                                                                 \
      return Slot.takeError();                                                 \
    Result.ThreadInitializationSymbols.emplace(#Name, *Slot + Bias);           \
  }
#include "KernelThreadReferenceSymbols.def"
#undef NEVERD_THREAD_REFERENCE_CODE
#undef NEVERD_THREAD_REFERENCE_DATA
    }
    for (const auto &Reloc : Image->BaseRelocations) {
      if (Reloc.Address < *Address || Reloc.Address - *Address >= 0x380)
        continue;
      const uint64_t Offset = Reloc.Address - *Address;
      if (Reloc.Type != llvm::COFF::IMAGE_REL_BASED_DIR64 || Offset > 0x378 ||
          Offset % 8 || !Relocated.insert(Offset).second)
        return invalid("invalid debugger template relocation");
      auto *Field = Facts.Template.data() + Offset;
      llvm::support::endian::write64le(
          Field, llvm::support::endian::read64le(Field) + Bias);
    }
    for (uint64_t Offset = 0; Offset < 0x380; Offset += 8) {
      const auto Value =
          llvm::support::endian::read64le(Facts.Template.data() + Offset);
      if (Value >= Image->Base && Value - Image->Base < Result.Size &&
          !Relocated.contains(Offset))
        return invalid(
            "debugger template image pointer lacks DIR64 relocation");
    }
    Result.Debugger = std::move(Facts);
    for (const auto &Segment : Image->Segments) {
      if (Segment.isExecutable())
        continue;
      if (Segment.VA % 4096)
        return invalid("unaligned reference kernel data section");
      ImageMapping Region;
      Region.Address = Segment.VA + Bias;
      Region.Permissions = Read | Write;
      Region.Bytes.resize((std::max(Segment.Size, Segment.FileSz) + 4095) &
                          ~uint64_t(4095));
      Result.OpaqueData.push_back(std::move(Region));
    }
  }
  auto ExportData = KernelReferenceExports::load(*Image, **Object,
                                                 ReferenceBase, MemoryLimit);
  if (!ExportData)
    return ExportData.takeError();
  Result.ExportData = std::move(*ExportData);
  if (Result.Debugger) {
    const auto &Facts = *Result.Debugger;
    for (const auto &[Address, Length] :
         {std::pair<uint64_t, uint64_t>{Facts.Address, Facts.Template.size()},
          {Facts.ListHead, 16},
          {Facts.ActiveProcessHead, 16},
          {Facts.SystemVaAssignments, systemva::AssignmentBytes},
          {Facts.SecureCookie, 8},
          {Facts.GuardBitmapPointer, 8},
          {Facts.ServiceDescriptor, 32},
          {Facts.ServiceTable, Facts.CompactedServices.size()},
          {Facts.ServiceArguments, Facts.ServiceArgumentBytes.size()}})
      if (Address && Length && Result.ExportData.overlapsBytes(Address, Length))
        return invalid(
            "modeled kernel state overlaps immutable export metadata");
    for (const auto &[Address, Length] :
         {std::pair<uint64_t, uint64_t>{Facts.PciMcfgCount, 4},
          {Facts.PciMcfgPointer, 8}}) {
      if (!Address)
        continue;
      if (Result.ExportData.overlapsBytes(Address, Length))
        return invalid("MCFG state overlaps immutable export metadata");
      for (const auto &[Other, Size] :
           {std::pair<uint64_t, uint64_t>{Facts.Address, Facts.Template.size()},
            {Facts.ListHead, 16},
            {Facts.ActiveProcessHead, 16},
            {Facts.SystemVaAssignments, systemva::AssignmentBytes},
            {Facts.SecureCookie, 8},
            {Facts.GuardBitmapPointer, 8},
            {Facts.ServiceDescriptor, 32},
            {Facts.ServiceTable, Facts.CompactedServices.size()},
            {Facts.ServiceArguments, Facts.ServiceArgumentBytes.size()}})
        if (Other && Size && Address < Other + Size && Other < Address + Length)
          return invalid("MCFG state overlaps another kernel data owner");
    }
  }
  ImageMapping Headers;
  Headers.Address = ReferenceBase;
  Headers.Permissions = Read;
  Headers.Bytes.resize((uint64_t(Header->SizeOfHeaders) + 4095) &
                       ~uint64_t(4095));
  if (Headers.Bytes.size() > MemoryLimit)
    return invalid("kernel_image headers exceed the memory budget");
  std::copy_n(Image->Raw.begin(), uint32_t(Header->SizeOfHeaders),
              Headers.Bytes.begin());
  Result.Code.push_back(std::move(Headers));
  BinaryImage CodeImage;
  CodeImage.Base = Image->Base;
  CodeImage.Entry = Image->Entry;
  CodeImage.Raw = Image->Raw;
  for (const auto &Segment : Image->Segments)
    if (Segment.isExecutable()) {
      auto &Mapping = CodeImage.Segments.emplace_back();
      Mapping.VA = Segment.VA;
      // PE maps the larger raw/virtual extent, including file-alignment tails.
      Mapping.Size = std::max(Segment.Size, Segment.FileSz);
      Mapping.FileOff = Segment.FileOff;
      Mapping.FileSz = Segment.FileSz;
      Mapping.Flags = Segment.Flags;
    }
  auto Plan = ImageMappingPlan::create(CodeImage, Bias, 4096, MemoryLimit,
                                       false, ImagePagePadding::Zero,
                                       ImageByteSource::OriginalFile);
  if (!Plan)
    return Plan.takeError();
  for (auto &Region : Plan->Regions)
    if (Region.Permissions & Execute) {
      Region.Permissions = Read | Execute;
      Result.Code.push_back(std::move(Region));
    }
  if (Result.Code.size() == 1)
    return invalid("kernel_image contains no executable reference pages");
  // Map original relocated code only. Host data pointers are not guest facts.
  for (const auto &Reloc : Image->BaseRelocations) {
    if (Reloc.Address > UINT64_MAX - Bias)
      return invalid("kernel_image relocation address overflows");
    const uint64_t Address = Reloc.Address + Bias;
    for (auto &Region : Result.Code) {
      if (Address < Region.Address ||
          Address - Region.Address >= Region.Bytes.size())
        continue;
      const uint64_t Offset = Address - Region.Address;
      if (Reloc.Type != llvm::COFF::IMAGE_REL_BASED_DIR64 ||
          Region.Bytes.size() - Offset < 8)
        return invalid("unsupported kernel_image code relocation");
      auto *Bytes = Region.Bytes.data() + Offset;
      llvm::support::endian::write64le(
          Bytes, llvm::support::endian::read64le(Bytes) + Bias);
    }
  }
  std::map<uint64_t, size_t> Aliases;
  for (const auto &[Name, RVA] : Result.ExportData.names()) {
    const uint64_t ExportAddress = Image->Base + RVA;
    uint64_t Address = 0;
    const bool ProcessData = process_data::find(Name);
    const bool ProcessorData = processor::isData(Name);
    const bool NlsData = nls::isData(Name);
    const bool HalData = hal_dispatch::isData(Name);
    if (object_type::find(Name) || ProcessData || ProcessorData || NlsData ||
        HalData) {
      const uint64_t Width = HalData         ? hal_dispatch::Bytes
                             : ProcessorData ? processor::CountBytes
                             : NlsData       ? nls::DataBytes
                                             : 8;
      bool Valid = false;
      for (const auto &Segment : Image->Segments) {
        const uint64_t Extent = std::max(Segment.Size, Segment.FileSz);
        if (Segment.isExecutable() || ExportAddress < Segment.VA ||
            Extent < Width || ExportAddress - Segment.VA > Extent - Width ||
            ExportAddress % (HalData ? hal_dispatch::Alignment : Width))
          continue;
        if (ExportAddress > UINT64_MAX - Bias)
          return invalid("kernel typed data export address overflows");
        if (Result.ExportData.overlapsBytes(ExportAddress + Bias, Width))
          return invalid(
              ProcessData
                  ? "kernel process data overlaps immutable export metadata"
                  : "kernel object-type data overlaps immutable export "
                    "metadata");
        auto &Data = HalData         ? Result.HalDispatchExports
                     : NlsData       ? Result.NlsDataExports
                     : ProcessorData ? Result.ProcessorDataExports
                     : ProcessData   ? Result.ProcessDataExports
                                     : Result.ObjectTypeExports;
        Data.emplace(Name, ExportAddress + Bias);
        Valid = true;
        break;
      }
      if (!Valid)
        return invalid(
            NlsData ? "kernel NLS export requires a data byte"
            : ProcessorData
                ? "kernel processor count export requires a data byte"
            : ProcessData
                ? "kernel process data export requires an aligned data pointer"
                : "kernel object-type export requires an aligned data pointer");
    }
    for (const auto &Segment : Image->Segments)
      if (Segment.isExecutable() && Segment.contains(ExportAddress)) {
        if (ExportAddress > UINT64_MAX - Bias)
          return invalid("kernel_image export address overflows");
        Address = ExportAddress + Bias;
        ++Aliases[Address];
        break;
      }
    if (!Result.Exports.emplace(Name, Address).second)
      return invalid("duplicate kernel_image export name");
  }
  for (auto &[Name, Address] : Result.Exports)
    if (Address && Aliases[Address] != 1)
      Address = 0;
  // Without private KD metadata only the pages containing these public data
  // exports are instantiated. Their other bytes remain opaque to the model.
  if (!Result.Debugger) {
    std::set<uint64_t> Pages;
    for (const auto &[Name, Address] : Result.ObjectTypeExports)
      Pages.insert(Address & ~uint64_t(4095));
    for (const auto &[Name, Address] : Result.ProcessDataExports)
      Pages.insert(Address & ~uint64_t(4095));
    for (const auto &[Name, Address] : Result.ProcessorDataExports)
      Pages.insert(Address & ~uint64_t(4095));
    for (const auto &[Name, Address] : Result.NlsDataExports)
      Pages.insert(Address & ~uint64_t(4095));
    for (const auto &[Name, Address] : Result.HalDispatchExports)
      for (uint64_t P = Address & ~uint64_t(4095);
           P < Address + hal_dispatch::Bytes; P += 4096)
        Pages.insert(P);
    for (const auto Page : Pages) {
      ImageMapping Region;
      Region.Address = Page;
      Region.Permissions = Read | Write;
      Region.Bytes.resize(4096);
      Result.OpaqueData.push_back(std::move(Region));
    }
  }
  return Result;
}
bool KernelReferenceImage::contains(uint64_t Address) const {
  for (const auto &Region : Code)
    if (Address >= Region.Address &&
        Address - Region.Address < Region.Bytes.size())
      return true;
  return false;
}
bool KernelReferenceImage::matchesCode(uint64_t Address,
                                       llvm::ArrayRef<uint8_t> Bytes) const {
  if (Bytes.empty())
    return false;
  for (const auto &Region : Code) {
    if (!(Region.Permissions & Execute) || Address < Region.Address)
      continue;
    const uint64_t Offset = Address - Region.Address;
    if (Offset <= Region.Bytes.size() &&
        Bytes.size() <= Region.Bytes.size() - Offset)
      return std::equal(Bytes.begin(), Bytes.end(),
                        Region.Bytes.begin() + Offset);
  }
  return false;
}

llvm::Error KernelReferenceImage::map(GuestMemory &Memory) const {
  for (const auto &Region : OpaqueData)
    if (auto E =
            Memory.map(Region.Address, Region.Bytes.size(), Region.Permissions))
      return E;
  for (const auto &Region : ExportData.pages()) {
    for (uint64_t Offset = 0; Offset < Region.Bytes.size(); Offset += 4096) {
      const uint64_t Page = Region.Address + Offset;
      const bool Shared = std::any_of(
          OpaqueData.begin(), OpaqueData.end(), [&](const auto &Opaque) {
            return Page >= Opaque.Address &&
                   Page - Opaque.Address < Opaque.Bytes.size();
          });
      if (!Shared)
        if (auto E = Memory.map(Page, 4096, Read | Write))
          return E;
      if (auto E = Memory.write(
              Page, llvm::ArrayRef(Region.Bytes).slice(Offset, 4096)))
        return E;
      if (!Shared)
        if (auto E = Memory.protect(Page, 4096, Read))
          return E;
    }
  }
  for (const auto &Region : Code) {
    if (auto E = Memory.map(Region.Address, Region.Bytes.size(), Read | Write))
      return E;
    if (auto E = Memory.write(Region.Address, Region.Bytes))
      return E;
    if (auto E = Memory.protect(Region.Address, Region.Bytes.size(),
                                Region.Permissions))
      return E;
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
