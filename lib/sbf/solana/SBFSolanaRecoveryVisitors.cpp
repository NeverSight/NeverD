//===- SBFSolanaRecoveryVisitors.cpp - Per-instruction fact recovery ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What each kind of instruction proves about a program as a Solana program:
/// an input-buffer access names an account field, a wide equality compare
/// offers a dispatch discriminator, a syscall offers an invocation or a
/// derivation, and a return offers a failure code.
///
//===----------------------------------------------------------------------===//

#include "SBFSolanaRecoveryDetail.h"

#include "neverd/sbf/SBFConstants.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace neverd::sbf {
namespace solana_recovery_detail {

std::optional<uint64_t> Recovery::constantOf(const RegisterValue &Value) {
  if (Value.ValueKind != RegisterValue::Kind::Constant)
    return std::nullopt;
  return Value.Value;
}

std::optional<va_t> Recovery::addressOf(const RegisterValue &Value) {
  return effectiveAddress(Value, 0);
}

std::optional<AddressedKey> Recovery::readKeyAt(va_t Address) const {
  llvm::Expected<llvm::ArrayRef<uint8_t>> Bytes =
      Program.ExecutableImage.slice(Address, kPubkeyByteCount,
                                    /*DataAccess=*/true);
  if (!Bytes) {
    llvm::consumeError(Bytes.takeError());
    return std::nullopt;
  }
  llvm::Expected<Pubkey> Key = readPubkey(*Bytes);
  if (!Key) {
    llvm::consumeError(Key.takeError());
    return std::nullopt;
  }
  return AddressedKey{Address, *Key};
}

void Recovery::noteKey(va_t Address, const Pubkey &Key, bool ReferencedByCode) {
  // Deduplicate on the key rather than the address so a value repeated across
  // read-only data appears once, at the first place it was found.
  const uint64_t FirstWord = llvm::support::endian::read64le(Key.Bytes.data());
  auto [It, Inserted] =
      KeyIndexByFirstWord.try_emplace(FirstWord, Model.Pubkeys.size());
  if (!Inserted) {
    RecoveredPubkey &Existing = Model.Pubkeys[It->second];
    if (Existing.Key == Key) {
      Existing.ReferencedByCode |= ReferencedByCode;
      if (ReferencedByCode)
        Existing.Evidence = Existing.Known ? RecoveryEvidence::KnownAddressTable
                                           : RecoveryEvidence::ConstantDataflow;
      return;
    }
    // A different key sharing a leading word is possible in principle; fall
    // through and record it separately rather than merging distinct addresses.
  }

  RecoveredPubkey Recovered;
  Recovered.Address = Address;
  Recovered.Key = Key;
  Recovered.Known = findKnownAddress(Key);
  Recovered.ReferencedByCode = ReferencedByCode;
  Recovered.Evidence = Recovered.Known ? RecoveryEvidence::KnownAddressTable
                                       : RecoveryEvidence::ConstantDataflow;
  Model.Pubkeys.push_back(std::move(Recovered));
}

void Recovery::visitMemoryAccess(const MedInstruction &Instruction,
                                 const MachineState &State) {
  const bool IsWrite = Instruction.Op == Operation::Store;
  // A load addresses through its source register, a store through its
  // destination register.
  const uint8_t BaseRegister = IsWrite ? Instruction.Dst : Instruction.Src;
  const std::optional<va_t> Address =
      effectiveAddress(State[BaseRegister], Instruction.Offset);
  if (!Address || *Address < kInputStart ||
      *Address >= kInputStart + kMemoryRegionSize)
    return;

  AccountAccess Access;
  Access.Slot = Instruction.Slot;
  Access.Address = *Address;
  Access.InputOffset = *Address - kInputStart;
  Access.IsWrite = IsWrite;

  // Which field an offset names depends on the serialization the owning loader
  // produces, so the profile is what answers it. Guessing would not fail
  // loudly: under the other serialization the same offset is a different real
  // field, and the recovered name would look entirely plausible.
  const AccountABI ABI = Options.Profile.accountABI();
  const uint64_t FirstAccount = firstAccountOffset();
  if (Access.InputOffset < FirstAccount) {
    for (auto [Ordinal, Field] : llvm::enumerate(inputFieldInfos()))
      if (Access.InputOffset >= Field.Offset &&
          Access.InputOffset < Field.Offset + Field.Size)
        Access.Header = static_cast<InputField>(Ordinal);
  } else if (Access.InputOffset < FirstAccount + accountFixedSize(ABI)) {
    // Only the first account entry begins at a statically known offset; later
    // entries follow variable-length data.
    const uint64_t InAccount = Access.InputOffset - FirstAccount;
    if (const AccountLayoutInfo *Field = accountFieldAt(ABI, InAccount);
        Field && Field->Size != 0)
      Access.Field = Field->Field;
  }

  Model.AccountAccesses.push_back(Access);
}

void Recovery::visitComparison(const MedInstruction &Instruction,
                               const MachineState &State) {
  // Anchor dispatch compares the leading eight bytes of instruction data
  // against a 64-bit constant. A 32-bit compare or an immediate operand cannot
  // carry a discriminator, because an SBF immediate is only 32 bits wide.
  if (Instruction.Form != OperandForm::BranchReg ||
      Instruction.Width != kDoubleWordBitWidth)
    return;
  if (Instruction.Op != Operation::Eq && Instruction.Op != Operation::Ne)
    return;

  std::optional<uint64_t> Word = constantOf(State[Instruction.Src]);
  if (!Word)
    Word = constantOf(State[Instruction.Dst]);
  if (!Word)
    return;

  Candidate Entry;
  Entry.Word = *Word;
  Entry.CompareSlot = Instruction.Slot;
  // An equality branch reaches its arm by being taken; an inequality branch
  // reaches it by falling through.
  Entry.TargetSlot =
      Instruction.Op == Operation::Eq
          ? Instruction.BranchTarget
          : std::optional<size_t>(Instruction.Slot + Instruction.SlotWidth);
  Candidates.push_back(Entry);
}

void Recovery::visitSyscall(const MedInstruction &Instruction,
                            const MachineState &State) {
  const SyscallInfo &Info = *Instruction.Syscall;

  // A pointer argument holding a constant that addresses a key's worth of
  // readable bytes is an address the program hands to the runtime. Scalar
  // arguments are skipped: a length and a low read-only address are the same
  // number once rodata is mapped at zero.
  std::array<std::optional<AddressedKey>, kArgumentRegisterCount> Arguments{};
  for (unsigned Ordinal = 0;
       Ordinal < std::min<unsigned>(Info.ArgumentCount, kArgumentRegisterCount);
       ++Ordinal) {
    if (!isPointerArgument(Info.PointerArguments, Ordinal))
      continue;
    const std::optional<va_t> Value =
        addressOf(State[kFirstArgumentRegister + Ordinal]);
    if (!Value)
      continue;
    Arguments[Ordinal] = readKeyAt(*Value);
    if (Arguments[Ordinal])
      noteKey(Arguments[Ordinal]->Address, Arguments[Ordinal]->Key,
              /*ReferencedByCode=*/true);
  }

  if (hasEffect(Info.Effects, SyscallEffect::CPI))
    visitInvocation(Instruction, State, Info);
  else if (Info.Category == SyscallCategory::PDA)
    visitDerivation(Instruction, State, Info);

  // Anchor's generated entrypoint proves a program's own address by comparing
  // the incoming program id against a constant, for exactly a key's length.
  if (Info.ID == Syscall::Memcmp && !Model.ProgramId) {
    const std::optional<uint64_t> Length =
        constantOf(State[argumentRegister(CompareArgument::Length)]);
    if (Length && *Length == kPubkeyByteCount)
      for (CompareArgument Side :
           {CompareArgument::Left, CompareArgument::Right})
        if (const std::optional<AddressedKey> &Argument =
                Arguments[static_cast<unsigned>(Side)];
            Argument && !findKnownAddress(Argument->Key))
          Model.ProgramId = Argument->Key;
  }
}

void Recovery::visitInvocation(const MedInstruction &Instruction,
                               const MachineState &State,
                               const SyscallInfo &Info) {
  CPISite Site;
  Site.Slot = Instruction.Slot;
  Site.Which = Info.ID;
  // Which layout to read is decided by the syscall, never by what the bytes
  // happen to look like. The two disagree about where the program id is, and
  // reading the wrong one reports the first account as the invoked program.
  Site.ABI = findCPIABI(Info.ID);

  const std::optional<va_t> Address =
      addressOf(State[argumentRegister(InvokeArgument::Instruction)]);
  if (Site.ABI && Address) {
    const BlockMemory Memory(Program.ExecutableImage, State.Scratch.Memory);
    const DecodedInstruction Decoded =
        readInstruction(Memory, *Site.ABI, *Address);
    Site.AccountCount = Decoded.AccountCount;
    Site.DataLength = Decoded.DataLength;

    if (Decoded.ProgramId) {
      Site.ProgramId = *Decoded.ProgramId;
      Site.KnownProgram = findKnownAddress(*Decoded.ProgramId);
      // Record the constant only when it lives in the image. A key the frame
      // assembled has no read-only address to report it at.
      if (const std::optional<va_t> KeyAt =
              programIdAddress(Memory, *Site.ABI, *Address))
        if (std::optional<AddressedKey> Key = readKeyAt(*KeyAt))
          noteKey(Key->Address, Key->Key, /*ReferencedByCode=*/true);
    }
    nameInvokedOperation(Site, Decoded.Data);
  }
  Model.CPISites.push_back(std::move(Site));
}

void Recovery::nameInvokedOperation(CPISite &Site,
                                    llvm::ArrayRef<uint8_t> Payload) {
  if (Payload.empty())
    return;

  if (Site.KnownProgram) {
    if (const ProgramInstructionInfo *Selected =
            findProgramInstruction(Site.KnownProgram->ID, Payload)) {
      Site.Selected = Selected;
      return;
    }
    // A program whose selector encoding is tabulated said a number the table
    // does not list. Falling through to the Anchor dictionary would read four
    // bincode bytes as though they began a hash.
    if (findInstructionSet(Site.KnownProgram->ID))
      return;
  }

  if (Payload.size() < kAnchorDiscriminatorLength)
    return;
  AnchorDiscriminator Value;
  llvm::copy(Payload.take_front(kAnchorDiscriminatorLength),
             Value.Bytes.begin());
  Site.Discriminator = Value;

  // The supplied IDL describes one program. It names an invoked instruction
  // only when the invocation targets that program, which a program does when
  // it re-enters itself.
  const bool IdlDescribesTarget = Options.Idl && Options.Idl->Address &&
                                  Site.ProgramId &&
                                  *Options.Idl->Address == *Site.ProgramId;
  if (const AnchorIdlItem *Item =
          IdlDescribesTarget ? Options.Idl->find(Value) : nullptr) {
    Site.Name = Item->Name;
    Site.NameEvidence = RecoveryEvidence::SuppliedIdl;
  } else if (const AnchorNameInfo *Known = findAnchorName(Value)) {
    Site.Name = Known->Name.str();
    Site.NameEvidence = RecoveryEvidence::AnchorDictionary;
  }
}

void Recovery::visitDerivation(const MedInstruction &Instruction,
                               const MachineState &State,
                               const SyscallInfo &Info) {
  PDADerivation Derivation;
  Derivation.Slot = Instruction.Slot;
  Derivation.Which = Info.ID;
  Derivation.DeclaredSeedCount =
      constantOf(State[argumentRegister(DeriveArgument::SeedCount)]);

  const BlockMemory Memory(Program.ExecutableImage, State.Scratch.Memory);

  if (const std::optional<va_t> Id =
          addressOf(State[argumentRegister(DeriveArgument::ProgramId)]))
    if (std::optional<Pubkey> Key = Memory.readKey(*Id)) {
      Derivation.ProgramId = *Key;
      Derivation.KnownProgram = findKnownAddress(*Key);
    }

  const std::optional<va_t> Array =
      addressOf(State[argumentRegister(DeriveArgument::Seeds)]);
  if (Array && Derivation.DeclaredSeedCount)
    for (const SeedDescriptor &Descriptor :
         readSeedArray(Memory, *Array, *Derivation.DeclaredSeedCount)) {
      RecoveredSeed Seed;
      Seed.Address = Descriptor.Address;
      Seed.Length = Descriptor.Length;
      const llvm::ArrayRef<uint8_t> Bytes =
          Memory.readBytes(Descriptor.Address, Descriptor.Length);
      Seed.Bytes.assign(Bytes.begin(), Bytes.end());
      Derivation.Seeds.push_back(std::move(Seed));
    }

  Model.Derivations.push_back(std::move(Derivation));
}

void Recovery::visitExit(const MedInstruction &Instruction,
                         const MachineState &State) {
  const std::optional<uint64_t> Code = constantOf(State[kReturnRegister]);
  // Zero is success, and a program returns it from every path that worked.
  if (!Code || *Code == 0)
    return;
  const AnchorErrorClassification Classified = classifyAnchorError(*Code);
  if (!Classified.isMeaningful())
    return;
  ErrorCandidates.push_back({Instruction.Slot, *Code, Classified});
}

bool Recovery::nameDiscriminator(
    const AnchorDiscriminator &Value, std::string &Name,
    AnchorNamespace &Namespace,
    std::optional<RecoveryEvidence> &Evidence) const {
  if (const AnchorIdlItem *Item =
          Options.Idl ? Options.Idl->find(Value) : nullptr) {
    Name = Item->Name;
    Namespace = Item->Namespace;
    Evidence = RecoveryEvidence::SuppliedIdl;
    return true;
  }
  if (const AnchorNameInfo *Known = findAnchorName(Value)) {
    Name = Known->Name.str();
    Namespace = Known->Namespace;
    Evidence = RecoveryEvidence::AnchorDictionary;
    return true;
  }
  return false;
}

} // namespace solana_recovery_detail
} // namespace neverd::sbf
