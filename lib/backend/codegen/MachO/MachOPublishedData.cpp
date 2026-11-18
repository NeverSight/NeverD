//===- MachOPublishedData.cpp - Publish embedded Mach-O data -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOPublishedData.h"

#include "MachODependencyState.h"
#include "MachOStrictLayout.h"

#include "neverd/loader/MachO/MachOArm64e.h"

#include "llvm/Support/Errc.h"

namespace neverd {
using namespace llvm::MachO;
namespace {
#define MACHO_PUBLISHED_DATA_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MachOPublishedData.def"
llvm::Error invalid() {
  return llvm::createStringError(llvm::errc::not_supported, kPublishedLayout);
}
} // namespace

llvm::Error installMachOPublishedData(
    std::vector<uint8_t> &Binary, const CompiledImage &Image,
    llvm::ArrayRef<std::pair<size_t, MachODataInCodeRange>> Records) {
  if (Records.empty())
    return llvm::Error::success();
  MachOChainedFixups Chains;
  auto State = readMachODependencyState(Binary, &Chains);
  if (!State)
    return State.takeError();
  std::optional<size_t> Command;
  bool Invalid = false;
  linkedit_data_command Table{LC_DATA_IN_CODE, sizeof(Table), 0, 0};
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *P, uint32_t ID, uint32_t Size, bool Is64) {
        // Segment installation removed the invalidated source signature.
        Invalid |= !Is64 || ID == LC_CODE_SIGNATURE;
        if (ID != LC_DATA_IN_CODE)
          return;
        Invalid |= Command.has_value() || Size != sizeof(Table);
        Command = P - Binary.data();
        Table = *reinterpret_cast<const linkedit_data_command *>(P);
      });
  if (Invalid || !rangeInBounds(Table.dataoff, Table.datasize, Binary.size()))
    return invalid();
  auto Data = decodeMachODataInCode(
      llvm::ArrayRef(Binary).slice(Table.dataoff, Table.datasize),
      Chains.HeaderVA, llvm::endianness::little);
  if (!Data)
    return Data.takeError();
  for (const auto &[Index, R] : Records) {
    const auto &Section = Image.Sections[Index];
    Data->push_back({Section.VA + R.Begin, Section.VA + R.End, R.Kind});
  }
  llvm::sort(*Data,
             [](const auto &L, const auto &R) { return L.Begin < R.Begin; });
  auto Header = parseMachOHeader(Binary.data(), Binary.size());
  if (!Command) {
    const uint64_t Off = uint64_t(Header.HeaderSize) + Header.SizeOfCmds;
    uint64_t First = findFirstMachODataOffset(Binary.data(), Binary.size());
    std::vector<macho_patch_detail::MachOFileRange> Payloads;
    if (!macho_patch_detail::collectMachOPayloadRanges(Binary, Payloads))
      return invalid();
    for (const auto &Range : Payloads)
      First = std::min(First, Range.Begin);
    if (First > Binary.size() || !rangeInBounds(Off, sizeof(Table), First) ||
        !llvm::all_of(llvm::ArrayRef(Binary).slice(Off, sizeof(Table)),
                      [](uint8_t B) { return !B; }))
      return invalid();
    Command = Off;
  }
  const auto At = llvm::alignTo(Binary.size(), alignof(uint64_t));
  if (At > UINT32_MAX ||
      Data->size() > (UINT32_MAX - At) / sizeof(data_in_code_entry))
    return invalid();
  va_t Last = Chains.HeaderVA;
  for (const auto &R : *Data) {
    if (R.Begin < Last || R.Begin - Chains.HeaderVA > UINT32_MAX ||
        R.End <= R.Begin || R.End - R.Begin > UINT16_MAX)
      return invalid();
    Last = R.End;
  }
  if (*Command == uint64_t(Header.HeaderSize) + Header.SizeOfCmds)
    setMachOHeaderCmds(Binary.data(), true, Header.NCmds + 1,
                       Header.SizeOfCmds + sizeof(Table));
  Table.dataoff = At;
  Table.datasize = Data->size() * sizeof(data_in_code_entry);
  std::memcpy(Binary.data() + *Command, &Table, sizeof(Table));
  Binary.resize(At + Table.datasize);
  size_t Cursor = At;
  for (const auto &R : *Data) {
    writeLE<uint32_t>(Binary.data() + Cursor +
                          offsetof(data_in_code_entry, offset),
                      R.Begin - Chains.HeaderVA);
    writeLE<uint16_t>(Binary.data() + Cursor +
                          offsetof(data_in_code_entry, length),
                      R.End - R.Begin);
    writeLE<uint16_t>(
        Binary.data() + Cursor + offsetof(data_in_code_entry, kind), R.Kind);
    Cursor += sizeof(data_in_code_entry);
  }
  auto *P = Binary.data() + Chains.Segments[Chains.LinkeditIndex].CommandOffset;
  auto Segment = readMachOSegment(P, true);
  Segment.FileSize = Binary.size() - Segment.FileOff;
  Segment.VMSize = std::max(
      Segment.VMSize,
      llvm::alignTo(Segment.FileSize, macho_loader::arm64e::kLargePageBytes));
  writeMachOSegment(P, true, Segment);
  return llvm::Error::success();
}
} // namespace neverd
