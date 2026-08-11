//===- CPI.cpp - Cross-program invocation ABI and decoding --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/CPI.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd::sbf {
namespace {

llvm::Error abiError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(("sbf: cpi abi: " + Message).str(),
                                             llvm::inconvertibleErrorCode());
}

llvm::Error instructionSetError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("sbf: instruction set: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

/// The key a field designates, following one indirection when the field holds
/// an address rather than the bytes themselves.
std::optional<Pubkey> readKeyField(const MemorySource &Memory, va_t Base,
                                   const CPIFieldInfo &Field) {
  va_t At = Base + Field.Offset;
  if (Field.Form == CPIFieldForm::KeyAddress) {
    const std::optional<uint64_t> Target = Memory.readWord(At);
    if (!Target)
      return std::nullopt;
    At = *Target;
  }
  return Memory.readKey(At);
}

} // namespace

llvm::StringRef cpiFieldFormName(CPIFieldForm Form) {
  switch (Form) {
  case CPIFieldForm::InlineKey:
    return "inline-key";
  case CPIFieldForm::KeyAddress:
    return "key-address";
  case CPIFieldForm::Address:
    return "address";
  case CPIFieldForm::Count:
    return "count";
  case CPIFieldForm::Length:
    return "length";
  case CPIFieldForm::Flag:
    return "flag";
  }
  return "unknown";
}

uint64_t CPIFieldInfo::size() const {
  switch (Form) {
  case CPIFieldForm::InlineKey:
    return kPubkeyByteCount;
  case CPIFieldForm::KeyAddress:
  case CPIFieldForm::Address:
  case CPIFieldForm::Count:
  case CPIFieldForm::Length:
    return sizeof(uint64_t);
  case CPIFieldForm::Flag:
    return 1;
  }
  return 0;
}

llvm::StringRef cpiFieldName(CPIField ID) {
  switch (ID) {
#define SBF_CPI_FIELD_ID(FIELD, NAME)                                          \
  case CPIField::FIELD:                                                        \
    return NAME;
#include "neverd/sbf/SBFCPIABI.def"
  }
  return "unknown";
}

llvm::StringRef cpiAccountMetaFieldName(CPIAccountMetaField ID) {
  switch (ID) {
#define SBF_CPI_META_FIELD_ID(FIELD, NAME)                                     \
  case CPIAccountMetaField::FIELD:                                             \
    return NAME;
#include "neverd/sbf/SBFCPIABI.def"
  }
  return "unknown";
}

llvm::ArrayRef<CPIABIInfo> cpiABIInfos() {
  static const std::array Table = [] {
    std::array<CPIABIInfo, kCPIABICount> Built{};
// The macro parameters are deliberately not spelled like the members they
// initialize: a parameter named ID would also rewrite `Entry.ID`.
#define SBF_CPI_ABI(ABI_ID, ABI_NAME, ABI_SYSCALL, ABI_SIZE, META_SIZE)        \
  {                                                                            \
    CPIABIInfo &Entry = Built[static_cast<size_t>(CPIABI::ABI_ID)];            \
    Entry.ID = CPIABI::ABI_ID;                                                 \
    Entry.Name = ABI_NAME;                                                     \
    Entry.Which = Syscall::ABI_SYSCALL;                                        \
    Entry.Size = ABI_SIZE;                                                     \
    Entry.AccountMetaSize = META_SIZE;                                         \
  }
#define SBF_CPI_FIELD(ABI_ID, FIELD, FIELD_OFFSET, FIELD_FORM)                 \
  Built[static_cast<size_t>(CPIABI::ABI_ID)]                                   \
      .Fields[static_cast<size_t>(CPIField::FIELD)] = {                        \
      cpiFieldName(CPIField::FIELD), FIELD_OFFSET,                             \
      CPIFieldForm::FIELD_FORM};
#define SBF_CPI_META_FIELD(ABI_ID, FIELD, FIELD_OFFSET, FIELD_FORM)            \
  Built[static_cast<size_t>(CPIABI::ABI_ID)]                                   \
      .AccountMetaFields[static_cast<size_t>(CPIAccountMetaField::FIELD)] = {  \
      cpiAccountMetaFieldName(CPIAccountMetaField::FIELD), FIELD_OFFSET,       \
      CPIFieldForm::FIELD_FORM};
#include "neverd/sbf/SBFCPIABI.def"
    return Built;
  }();
  return Table;
}

const CPIABIInfo &getCPIABIInfo(CPIABI ID) {
  return cpiABIInfos()[static_cast<size_t>(ID)];
}

const CPIABIInfo *findCPIABI(Syscall Which) {
  for (const CPIABIInfo &Info : cpiABIInfos())
    if (Info.Which == Which)
      return &Info;
  return nullptr;
}

llvm::ArrayRef<CPIFieldInfo> cpiSeedFieldInfos() {
  static const std::array Table = {
#define SBF_CPI_SEED_FIELD(ID, NAME, OFFSET, FORM)                             \
  CPIFieldInfo{NAME, OFFSET, CPIFieldForm::FORM},
#include "neverd/sbf/SBFCPIABI.def"
  };
  return Table;
}

const CPIFieldInfo &getCPISeedFieldInfo(CPISeedField ID) {
  return cpiSeedFieldInfos()[static_cast<size_t>(ID)];
}

uint64_t cpiSeedSize() {
  uint64_t End = 0;
  for (const CPIFieldInfo &Field : cpiSeedFieldInfos())
    End = std::max(End, Field.Offset + Field.size());
  return End;
}

llvm::Error validateCPIABITables() {
  for (const CPIABIInfo &Info : cpiABIInfos()) {
    const auto Check = [&](llvm::StringRef Structure, const CPIFieldInfo &Field,
                           uint64_t Limit) -> llvm::Error {
      if (Field.Name.empty())
        return abiError("the " + Info.Name + " " + Structure +
                        " leaves a declared field undescribed");
      if (Field.Offset + Field.size() > Limit)
        return abiError("the " + Info.Name + " " + Structure + " field '" +
                        Field.Name + "' ends at " +
                        llvm::Twine(Field.Offset + Field.size()) +
                        " but the structure is only " + llvm::Twine(Limit) +
                        " bytes");
      return llvm::Error::success();
    };

    for (const CPIFieldInfo &Field : Info.Fields)
      if (llvm::Error E = Check("instruction", Field, Info.Size))
        return E;
    for (const CPIFieldInfo &Field : Info.AccountMetaFields)
      if (llvm::Error E = Check("account reference", Field, Info.AccountMetaSize))
        return E;

    // A layout that no syscall selects could never be reached, and one that
    // two syscalls select would make the choice ambiguous.
    if (findCPIABI(Info.Which) != &Info)
      return abiError("the " + Info.Name +
                      " layout is not the single layout its syscall selects");
  }
  return llvm::Error::success();
}

uint64_t instructionTagSize(InstructionTagEncoding Encoding) {
  switch (Encoding) {
  case InstructionTagEncoding::Byte:
    return sizeof(uint8_t);
  case InstructionTagEncoding::Word:
    return sizeof(uint32_t);
  }
  return 0;
}

llvm::ArrayRef<InstructionSetInfo> instructionSetInfos() {
  static const std::array Table = {
#define SBF_INSTRUCTION_SET(SET, SET_NAME, TAG_ENCODING)                       \
  InstructionSetInfo{InstructionSet::SET, SET_NAME,                            \
                     InstructionTagEncoding::TAG_ENCODING},
#include "neverd/sbf/SBFProgramInstructions.def"
  };
  return Table;
}

const InstructionSetInfo *getInstructionSetInfo(InstructionSet ID) {
  for (const InstructionSetInfo &Info : instructionSetInfos())
    if (Info.ID == ID)
      return &Info;
  return nullptr;
}

llvm::StringRef instructionStatusName(InstructionStatus Status) {
  switch (Status) {
  case InstructionStatus::Current:
    return "current";
  case InstructionStatus::Deprecated:
    return "deprecated";
  }
  return "unknown";
}

llvm::ArrayRef<ProgramInstructionInfo> programInstructionInfos() {
  static const std::array Table = {
#define SBF_PROGRAM_INSTRUCTION(SET, OPERATION_NAME, TAG, STATUS)              \
  ProgramInstructionInfo{InstructionSet::SET, OPERATION_NAME, TAG,             \
                         InstructionStatus::STATUS},
#include "neverd/sbf/SBFProgramInstructions.def"
  };
  return Table;
}

llvm::ArrayRef<InstructionSetProgramInfo> instructionSetProgramInfos() {
  static const std::array Table = {
#define SBF_INSTRUCTION_SET_PROGRAM(SET, KNOWN_ADDRESS)                        \
  InstructionSetProgramInfo{KnownAddress::KNOWN_ADDRESS, InstructionSet::SET},
#include "neverd/sbf/SBFProgramInstructions.def"
  };
  return Table;
}

const InstructionSetInfo *findInstructionSet(KnownAddress Program) {
  for (const InstructionSetProgramInfo &Entry : instructionSetProgramInfos())
    if (Entry.Program == Program)
      return getInstructionSetInfo(Entry.Set);
  return nullptr;
}

const ProgramInstructionInfo *
findProgramInstruction(KnownAddress Program, llvm::ArrayRef<uint8_t> Data) {
  for (const InstructionSetProgramInfo &Entry : instructionSetProgramInfos()) {
    if (Entry.Program != Program)
      continue;
    const InstructionSetInfo *Set = getInstructionSetInfo(Entry.Set);
    if (!Set)
      continue;

    const uint64_t TagSize = instructionTagSize(Set->TagEncoding);
    if (Data.size() < TagSize)
      continue;
    const uint32_t Tag = Set->TagEncoding == InstructionTagEncoding::Byte
                             ? Data.front()
                             : llvm::support::endian::read32le(Data.data());

    for (const ProgramInstructionInfo &Info : programInstructionInfos())
      if (Info.Set == Set->ID && Info.Tag == Tag)
        return &Info;
  }
  return nullptr;
}

llvm::Error validateProgramInstructionTables() {
  for (const InstructionSetInfo &Set : instructionSetInfos()) {
    llvm::DenseSet<uint32_t> Tags;
    for (const ProgramInstructionInfo &Info : programInstructionInfos()) {
      if (Info.Set != Set.ID)
        continue;
      // A repeated tag would make one of the two names unreachable, and which
      // one survived would depend on table order.
      if (!Tags.insert(Info.Tag).second)
        return instructionSetError("'" + Set.Name + "' repeats selector " +
                                   llvm::Twine(Info.Tag));
    }
    if (Tags.empty())
      return instructionSetError("'" + Set.Name + "' names no operation");

    const bool Reachable =
        llvm::any_of(instructionSetProgramInfos(),
                     [&](const InstructionSetProgramInfo &Entry) {
                       return Entry.Set == Set.ID;
                     });
    if (!Reachable)
      return instructionSetError("'" + Set.Name +
                                 "' is selected by no known address");
  }

  // A program that answers to more than one set must read one selector, and
  // that selector must mean one thing: two sets disagreeing about its width,
  // or both claiming the same number, would make the name table order decide
  // what an invocation is called.
  for (const KnownAddressInfo &Address : knownAddressInfos()) {
    const InstructionSetInfo *First = findInstructionSet(Address.ID);
    if (!First)
      continue;
    llvm::DenseSet<uint32_t> Tags;
    for (const InstructionSetProgramInfo &Entry : instructionSetProgramInfos()) {
      if (Entry.Program != Address.ID)
        continue;
      const InstructionSetInfo *Set = getInstructionSetInfo(Entry.Set);
      if (!Set || Set->TagEncoding != First->TagEncoding)
        return instructionSetError("'" + Address.Name +
                                   "' answers to sets that spell their "
                                   "selector differently");
      for (const ProgramInstructionInfo &Info : programInstructionInfos())
        if (Info.Set == Entry.Set && !Tags.insert(Info.Tag).second)
          return instructionSetError("'" + Address.Name + "' gives selector " +
                                     llvm::Twine(Info.Tag) + " two names");
    }
  }
  return llvm::Error::success();
}

MemorySource::~MemorySource() = default;

std::optional<Pubkey> MemorySource::readKey(va_t Address) const {
  const llvm::ArrayRef<uint8_t> Bytes = readBytes(Address, kPubkeyByteCount);
  if (Bytes.empty())
    return std::nullopt;
  llvm::Expected<Pubkey> Key = readPubkey(Bytes);
  if (!Key) {
    llvm::consumeError(Key.takeError());
    return std::nullopt;
  }
  return *Key;
}

std::optional<uint64_t> ImageMemorySource::readWord(va_t Address) const {
  const llvm::ArrayRef<uint8_t> Bytes = readBytes(Address, sizeof(uint64_t));
  if (Bytes.empty())
    return std::nullopt;
  return llvm::support::endian::read64le(Bytes.data());
}

llvm::ArrayRef<uint8_t> ImageMemorySource::readBytes(va_t Address,
                                                     size_t Size) const {
  llvm::Expected<llvm::ArrayRef<uint8_t>> Bytes =
      Image.slice(Address, Size, /*DataAccess=*/true);
  if (!Bytes) {
    llvm::consumeError(Bytes.takeError());
    return {};
  }
  return *Bytes;
}

DecodedInstruction readInstruction(const MemorySource &Memory,
                                   const CPIABIInfo &ABI, va_t Address) {
  DecodedInstruction Decoded;
  Decoded.ABI = &ABI;
  Decoded.ProgramId =
      readKeyField(Memory, Address, ABI.field(CPIField::ProgramId));
  Decoded.AccountCount =
      Memory.readWord(Address + ABI.field(CPIField::AccountCount).Offset);
  Decoded.DataLength =
      Memory.readWord(Address + ABI.field(CPIField::DataLength).Offset);

  const std::optional<uint64_t> DataAddress =
      Memory.readWord(Address + ABI.field(CPIField::Data).Offset);
  if (DataAddress && Decoded.DataLength && *Decoded.DataLength != 0)
    Decoded.Data = Memory.readBytes(*DataAddress, *Decoded.DataLength);
  return Decoded;
}

std::optional<va_t> programIdAddress(const MemorySource &Memory,
                                     const CPIABIInfo &ABI, va_t Address) {
  const CPIFieldInfo &Field = ABI.field(CPIField::ProgramId);
  if (Field.Form != CPIFieldForm::KeyAddress)
    return Address + Field.Offset;
  return Memory.readWord(Address + Field.Offset);
}

std::vector<SeedDescriptor> readSeedArray(const MemorySource &Memory,
                                          va_t Address, uint64_t Count) {
  const CPIFieldInfo &AddressField = getCPISeedFieldInfo(CPISeedField::Address);
  const CPIFieldInfo &LengthField = getCPISeedFieldInfo(CPISeedField::Length);
  const uint64_t Stride = cpiSeedSize();

  std::vector<SeedDescriptor> Seeds;
  for (uint64_t Index = 0; Index < std::min<uint64_t>(Count, kMaxSeeds);
       ++Index) {
    const va_t Entry = Address + Index * Stride;
    const std::optional<uint64_t> Bytes =
        Memory.readWord(Entry + AddressField.Offset);
    const std::optional<uint64_t> Length =
        Memory.readWord(Entry + LengthField.Offset);
    if (!Bytes || !Length || *Length > kMaxSeedLength)
      break;
    Seeds.push_back({*Bytes, *Length});
  }
  return Seeds;
}

} // namespace neverd::sbf
