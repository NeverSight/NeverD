//===- SBFCPI.h - Cross-program invocation ABI and decoding -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describes how the two invocation syscalls lay out the instruction they are
/// handed, and how to name the operation a canonical program was asked for.
///
/// The two layouts are not interchangeable. `sol_invoke_signed_c` puts a
/// pointer to the target program id in the first word; `sol_invoke_signed_rust`
/// puts the account vector's pointer there and stores the key inline further
/// on. Reading one with the other's offsets yields a plausible but wrong
/// address rather than an error, so every reader selects a layout by syscall.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFCPI_H
#define NEVERD_SBF_SOLANA_SBFCPI_H

#include "neverd/Common.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/solana/SBFKnownAddresses.h"
#include "neverd/sbf/solana/SBFPubkey.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::sbf {

/// Which serialized instruction layout an invocation syscall expects.
enum class CPIABI : uint8_t {
#define SBF_CPI_ABI(ID, NAME, SYSCALL, SIZE, ACCOUNT_META_SIZE) ID,
#include "neverd/sbf/solana/SBFCPIABI.def"
};

/// Fields of a serialized instruction, named by role rather than by either
/// structure's spelling.
enum class CPIField : uint8_t {
#define SBF_CPI_FIELD_ID(ID, NAME) ID,
#include "neverd/sbf/solana/SBFCPIABI.def"
};

/// Fields of one serialized account reference.
enum class CPIAccountMetaField : uint8_t {
#define SBF_CPI_META_FIELD_ID(ID, NAME) ID,
#include "neverd/sbf/solana/SBFCPIABI.def"
};

/// Fields of one program-derived-address seed.
enum class CPISeedField : uint8_t {
#define SBF_CPI_SEED_FIELD(ID, NAME, OFFSET, FORM) ID,
#include "neverd/sbf/solana/SBFCPIABI.def"
};

/// How to read a field, which is what stops a caller from following a count as
/// though it were an address.
enum class CPIFieldForm : uint8_t {
  /// Key bytes stored in place.
  InlineKey,
  /// A VM address of key bytes.
  KeyAddress,
  /// A VM address of an array or buffer.
  Address,
  /// A number of elements.
  Count,
  /// A number of bytes.
  Length,
  /// A one-byte boolean.
  Flag,
};

llvm::StringRef cpiFieldFormName(CPIFieldForm Form);

/// One field of a serialized structure.
///
/// The tables that hold these are filled by offset rather than in declaration
/// order, because the two ABIs order the same fields differently, so this type
/// is default-constructible and \c Name is empty until a row supplies it.
struct CPIFieldInfo {
  llvm::StringRef Name;
  uint64_t Offset = 0;
  CPIFieldForm Form = CPIFieldForm::Length;

  /// How many bytes this field occupies, which its form determines.
  uint64_t size() const;
};

inline constexpr size_t kCPIFieldCount = 0
#define SBF_CPI_FIELD_ID(ID, NAME) +1
#include "neverd/sbf/solana/SBFCPIABI.def"
    ;

inline constexpr size_t kCPIAccountMetaFieldCount = 0
#define SBF_CPI_META_FIELD_ID(ID, NAME) +1
#include "neverd/sbf/solana/SBFCPIABI.def"
    ;

inline constexpr size_t kCPISeedFieldCount = 0
#define SBF_CPI_SEED_FIELD(ID, NAME, OFFSET, FORM) +1
#include "neverd/sbf/solana/SBFCPIABI.def"
    ;

inline constexpr size_t kCPIABICount = 0
#define SBF_CPI_ABI(ID, NAME, SYSCALL, SIZE, ACCOUNT_META_SIZE) +1
#include "neverd/sbf/solana/SBFCPIABI.def"
    ;

/// The logical name of a field, which both layouts share.
llvm::StringRef cpiFieldName(CPIField ID);
llvm::StringRef cpiAccountMetaFieldName(CPIAccountMetaField ID);

struct CPIABIInfo {
  CPIABI ID = CPIABI::Rust;
  llvm::StringRef Name;
  /// The invocation syscall that selects this layout.
  Syscall Which = Syscall::Unknown;
  /// Size of the serialized instruction structure.
  uint64_t Size = 0;
  /// Size of one serialized account reference.
  uint64_t AccountMetaSize = 0;
  std::array<CPIFieldInfo, kCPIFieldCount> Fields;
  std::array<CPIFieldInfo, kCPIAccountMetaFieldCount> AccountMetaFields;

  const CPIFieldInfo &field(CPIField ID) const {
    return Fields[static_cast<size_t>(ID)];
  }
  const CPIFieldInfo &accountMetaField(CPIAccountMetaField ID) const {
    return AccountMetaFields[static_cast<size_t>(ID)];
  }
};

llvm::ArrayRef<CPIABIInfo> cpiABIInfos();
const CPIABIInfo &getCPIABIInfo(CPIABI ID);

/// The layout the given syscall expects, or null when the syscall does not
/// invoke another program.
const CPIABIInfo *findCPIABI(Syscall Which);

/// One seed descriptor, which both ABIs spell the same way.
llvm::ArrayRef<CPIFieldInfo> cpiSeedFieldInfos();
const CPIFieldInfo &getCPISeedFieldInfo(CPISeedField ID);
/// Size of one seed descriptor.
uint64_t cpiSeedSize();

/// Report a field the tables leave undescribed, or one that runs past the
/// structure that contains it.
llvm::Error validateCPIABITables();

//===----------------------------------------------------------------------===//
// Naming the operation a canonical program was asked for
//===----------------------------------------------------------------------===//

/// How a program spells the selector at the start of its instruction data.
enum class InstructionTagEncoding : uint8_t {
  /// One leading byte.
  Byte,
  /// Four leading little-endian bytes, as bincode encodes an enum.
  Word,
};

uint64_t instructionTagSize(InstructionTagEncoding Encoding);

enum class InstructionSet : uint8_t {
#define SBF_INSTRUCTION_SET(ID, NAME, TAG_ENCODING) ID,
#include "neverd/sbf/solana/SBFProgramInstructions.def"
  Unknown,
};

/// Whether the owning program still offers an operation.
enum class InstructionStatus : uint8_t { Current, Deprecated };

llvm::StringRef instructionStatusName(InstructionStatus Status);

struct ProgramInstructionInfo {
  InstructionSet Set;
  llvm::StringLiteral Name;
  uint32_t Tag;
  InstructionStatus Status;
};

struct InstructionSetInfo {
  InstructionSet ID;
  llvm::StringLiteral Name;
  InstructionTagEncoding TagEncoding;
};

/// One program's membership in one instruction set.
///
/// The relation is many-to-many in both directions: the two token programs
/// share one base numbering, and Token-2022 answers to that base plus its own
/// extension range.
struct InstructionSetProgramInfo {
  KnownAddress Program;
  InstructionSet Set;
};

llvm::ArrayRef<InstructionSetInfo> instructionSetInfos();
const InstructionSetInfo *getInstructionSetInfo(InstructionSet ID);
llvm::ArrayRef<ProgramInstructionInfo> programInstructionInfos();
llvm::ArrayRef<InstructionSetProgramInfo> instructionSetProgramInfos();

/// The first instruction set a canonical program answers to, or null when the
/// program's selector encoding is not tabulated. Every set one program answers
/// to shares its selector encoding, so this also answers how wide the
/// program's selector is.
const InstructionSetInfo *findInstructionSet(KnownAddress Program);

/// Name the operation \p Data selects for \p Program.
///
/// Returns null when the program is not tabulated, when the data is too short
/// to hold a selector, or when the selector is outside the tabulated range. An
/// unlisted selector is left for the caller to report as a number, because a
/// nearby name would be a guess.
const ProgramInstructionInfo *
findProgramInstruction(KnownAddress Program, llvm::ArrayRef<uint8_t> Data);

/// Report a duplicated selector within one instruction set, or a set that no
/// address selects.
llvm::Error validateProgramInstructionTables();

//===----------------------------------------------------------------------===//
// Reading serialized arguments out of proven memory
//===----------------------------------------------------------------------===//

/// Where a reader finds the bytes a serialized structure occupies.
///
/// A program builds its invocation arguments either in read-only data, which
/// the image maps, or in its own frame, which only the block that wrote it
/// knows. Decoding is identical either way, so it reads through this interface
/// rather than being written twice.
class MemorySource {
public:
  virtual ~MemorySource();

  /// The machine word at \p Address, when it is proven.
  virtual std::optional<uint64_t> readWord(va_t Address) const = 0;

  /// The bytes at \p Address, empty unless every one of them is proven.
  virtual llvm::ArrayRef<uint8_t> readBytes(va_t Address,
                                            size_t Size) const = 0;

  /// The key at \p Address, when its bytes are proven.
  std::optional<Pubkey> readKey(va_t Address) const;
};

/// A source that reads only what a loaded image maps.
class ImageMemorySource final : public MemorySource {
public:
  explicit ImageMemorySource(const ProgramImage &Image) : Image(Image) {}

  std::optional<uint64_t> readWord(va_t Address) const override;
  llvm::ArrayRef<uint8_t> readBytes(va_t Address, size_t Size) const override;

private:
  const ProgramImage &Image;
};

/// What a serialized instruction says, to the extent memory proves it.
///
/// Every field is optional because an instruction whose own address is known
/// may still point its accounts or its payload at memory nothing proves, such
/// as the heap.
struct DecodedInstruction {
  const CPIABIInfo *ABI = nullptr;
  std::optional<Pubkey> ProgramId;
  std::optional<uint64_t> AccountCount;
  std::optional<uint64_t> DataLength;
  /// The instruction payload, when every byte of it is proven.
  llvm::ArrayRef<uint8_t> Data;
};

/// Read the instruction at \p Address using the layout \p ABI prescribes.
///
/// Reports what memory proves and leaves the rest unset; failing to read one
/// field never invalidates the others.
DecodedInstruction readInstruction(const MemorySource &Memory,
                                   const CPIABIInfo &ABI, va_t Address);

/// Where the invoked program id's bytes live, given the instruction's own
/// address. The C layout stores a pointer to them, the Rust layout stores them
/// in place, and this is the one place that difference is resolved.
std::optional<va_t> programIdAddress(const MemorySource &Memory,
                                     const CPIABIInfo &ABI, va_t Address);

//===----------------------------------------------------------------------===//
// Argument positions of the signatures recovery reads
//===----------------------------------------------------------------------===//

/// Ordinals of the invocation signature both invoke syscalls share:
///
///   sol_invoke_signed_c(instruction, account_infos, account_info_count,
///                       signer_seeds, signer_seed_count)
///   sol_invoke_signed_rust(...the same five...)
enum class InvokeArgument : unsigned {
  Instruction = 0,
  AccountInfos,
  AccountInfoCount,
  SignerSeeds,
  SignerSeedCount,
};

/// Ordinals of the address-derivation signature both derivation syscalls
/// share, up to the extra output the searching form adds:
///
///   sol_create_program_address(seeds, seed_count, program_id, address)
///   sol_try_find_program_address(seeds, seed_count, program_id, address,
///                                bump_seed)
enum class DeriveArgument : unsigned {
  Seeds = 0,
  SeedCount,
  ProgramId,
  Address,
  BumpSeed,
};

constexpr unsigned argumentRegister(InvokeArgument Argument) {
  return kFirstArgumentRegister + static_cast<unsigned>(Argument);
}

constexpr unsigned argumentRegister(DeriveArgument Argument) {
  return kFirstArgumentRegister + static_cast<unsigned>(Argument);
}

/// Where one seed's bytes live, as its descriptor states.
struct SeedDescriptor {
  va_t Address = 0;
  uint64_t Length = 0;
};

/// Read up to \p Count seed descriptors from the array at \p Address.
///
/// Stops at the first descriptor memory cannot prove, so a short result means
/// the remaining seeds were built where this analysis cannot see them, and
/// never that they do not exist. \c kMaxSeeds bounds the walk because the
/// runtime rejects any call that passes more.
std::vector<SeedDescriptor> readSeedArray(const MemorySource &Memory,
                                          va_t Address, uint64_t Count);

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFCPI_H
