//===- SBFSolanaModel.h - Recovered Solana program facts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines what NeverD recovers about a program as a Solana program rather than
/// as SBF bytecode: its declared address, the accounts it reads, the Anchor
/// instructions it dispatches, the programs it invokes, and lint observations.
///
/// Every recorded fact carries the evidence that produced it. Recovery reports
/// what the bytes prove and leaves everything else unset, so a reader can tell
/// a decoded constant apart from a dictionary match.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFSOLANAMODEL_H
#define NEVERD_SBF_SOLANA_SBFSOLANAMODEL_H

#include "neverd/Common.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/solana/SBFAccountLayout.h"
#include "neverd/sbf/solana/SBFAnchor.h"
#include "neverd/sbf/solana/SBFCPI.h"
#include "neverd/sbf/solana/SBFKnownAddresses.h"
#include "neverd/sbf/solana/SBFPubkey.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {

/// How a recovered fact was established.
enum class RecoveryEvidence : uint8_t {
  /// Proven by constant propagation over decoded instructions.
  ConstantDataflow,
  /// The bytes equal an entry of the built-in well-known address table.
  KnownAddressTable,
  /// The discriminator equals one derived from the built-in Anchor dictionary.
  AnchorDictionary,
  /// The discriminator equals one declared by the supplied IDL.
  SuppliedIdl,
};

llvm::StringRef recoveryEvidenceName(RecoveryEvidence Evidence);

/// A 32-byte address found in the program's read-only data.
struct RecoveredPubkey {
  va_t Address = 0;
  Pubkey Key;
  /// Set when the address is a protocol or canonical-program address.
  const KnownAddressInfo *Known = nullptr;
  /// True when some instruction materializes this address as a constant.
  bool ReferencedByCode = false;
  RecoveryEvidence Evidence = RecoveryEvidence::KnownAddressTable;
};

/// One arm of a recovered Anchor instruction dispatch.
struct AnchorHandler {
  AnchorDiscriminator Discriminator;
  /// Empty when neither the dictionary nor the supplied IDL knows the name.
  std::string Name;
  AnchorNamespace Namespace = AnchorNamespace::Instruction;
  std::optional<RecoveryEvidence> NameEvidence;
  /// Slot of the comparison that selects this arm.
  size_t CompareSlot = 0;
  /// Slot the comparison branches to when it matches.
  std::optional<size_t> TargetSlot;
};

/// A cross-program invocation site.
///
/// Every field past the slot is optional because the serialized instruction
/// lives wherever the program built it. When that is the frame, and the frame
/// is still described, the whole invocation is readable; when it is the heap,
/// only the call itself is.
struct CPISite {
  size_t Slot = 0;
  Syscall Which = Syscall::Unknown;
  /// Which serialized layout the syscall reads, which is what decides where
  /// the invoked program id is.
  const CPIABIInfo *ABI = nullptr;
  /// Set only when the invoked program id is a constant in this binary.
  std::optional<Pubkey> ProgramId;
  const KnownAddressInfo *KnownProgram = nullptr;
  std::optional<uint64_t> AccountCount;
  std::optional<uint64_t> DataLength;
  /// The operation the payload selects, when the target is a canonical program
  /// whose selector encoding is tabulated.
  const ProgramInstructionInfo *Selected = nullptr;
  /// The eight-byte discriminator the payload starts with, when the target is
  /// not one of those programs.
  std::optional<AnchorDiscriminator> Discriminator;
  /// The name that discriminator resolves to, empty when none does.
  std::string Name;
  std::optional<RecoveryEvidence> NameEvidence;
};

/// One seed of a program-derived address.
struct RecoveredSeed {
  /// Where the seed's bytes live, when the descriptor gave a constant address.
  std::optional<va_t> Address;
  std::optional<uint64_t> Length;
  /// The seed's bytes, when the image maps all of them. A seed built at run
  /// time from account data has an address and a length but no bytes.
  std::vector<uint8_t> Bytes;

  /// True when the recovered bytes are entirely printable, which is how the
  /// literal seeds a developer writes appear.
  bool isText() const;
};

/// An address the program asks the runtime to derive from seeds.
///
/// These are the names behind a program's accounts, so recovering the literal
/// seeds recovers the account layout the program intends.
struct PDADerivation {
  size_t Slot = 0;
  Syscall Which = Syscall::Unknown;
  /// The seed count the call passes, when it is a constant.
  std::optional<uint64_t> DeclaredSeedCount;
  std::vector<RecoveredSeed> Seeds;
  /// The program the address is derived against, when it is a constant.
  std::optional<Pubkey> ProgramId;
  const KnownAddressInfo *KnownProgram = nullptr;

  /// True when a seed descriptor was read for every declared seed. A partial
  /// derivation is still reported, so a reader can see what was proven without
  /// mistaking it for the whole set.
  bool complete() const;
};

/// An eight-byte Anchor discriminator sitting in read-only data.
///
/// Anchor stores an account's discriminator as a constant and compares the
/// leading bytes of account data against it, so these constants enumerate the
/// account types a program declares even when no dispatch arm mentions them.
struct RecoveredDiscriminator {
  va_t Address = 0;
  AnchorDiscriminator Value;
  AnchorNamespace Namespace = AnchorNamespace::Account;
  std::string Name;
  RecoveryEvidence Evidence = RecoveryEvidence::AnchorDictionary;
};

/// A failure code the program provably returns.
///
/// For a stripped Anchor program these enumerate the checks it performs: a
/// returned ConstraintSeeds says the program validates a derived address, and
/// nothing else in the binary says so as plainly.
struct ReturnedError {
  size_t Slot = 0;
  uint64_t Code = 0;
  AnchorErrorClassification Classification;
};

/// A load or store whose address provably lands in the serialized input.
struct AccountAccess {
  size_t Slot = 0;
  va_t Address = 0;
  /// Offset from the start of the input buffer.
  uint64_t InputOffset = 0;
  /// Set when the offset falls in the first account entry's fixed part.
  std::optional<AccountField> Field;
  /// Set when the offset falls in the input header.
  std::optional<InputField> Header;
  bool IsWrite = false;
};

enum class LintSeverity : uint8_t { Note, Warning };
enum class LintConfidence : uint8_t { Advisory, Likely, Certain };

enum class Lint : uint8_t {
#define SBF_LINT(ID, NAME, SEVERITY, CONFIDENCE, SUMMARY) ID,
#include "neverd/sbf/solana/SBFLints.def"
};

struct LintInfo {
  Lint ID;
  llvm::StringLiteral Name;
  LintSeverity Severity;
  LintConfidence Confidence;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<LintInfo> lintInfos();
const LintInfo &getLintInfo(Lint ID);
llvm::StringRef lintSeverityName(LintSeverity Severity);
llvm::StringRef lintConfidenceName(LintConfidence Confidence);

/// A lint the program triggered, with the detail that justifies it.
struct LintFinding {
  Lint ID = Lint::MissingSignerCheck;
  std::optional<size_t> Slot;
  std::string Detail;
};

/// Everything recovery established about the program as a Solana program.
struct SolanaModel {
  /// The program's own declared address, when a comparison against a constant
  /// in read-only data proves it.
  std::optional<Pubkey> ProgramId;
  /// True once at least one Anchor discriminator has been matched.
  bool IsAnchor = false;
  /// Name and version of the IDL supplied by the operator, when one was.
  std::string IdlName;

  std::vector<RecoveredPubkey> Pubkeys;
  std::vector<AnchorHandler> Handlers;
  std::vector<RecoveredDiscriminator> Discriminators;
  std::vector<CPISite> CPISites;
  std::vector<PDADerivation> Derivations;
  std::vector<ReturnedError> Errors;
  std::vector<AccountAccess> AccountAccesses;
  std::vector<LintFinding> Findings;

  bool empty() const;
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFSOLANAMODEL_H
