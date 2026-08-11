//===- SolanaRecovery.cpp - Solana program fact recovery ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/SolanaRecovery.h"

#include "neverd/sbf/CPI.h"
#include "neverd/sbf/Dataflow.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/SBFIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace neverd::sbf {
namespace {

/// A constant register value that provably addresses \c kPubkeyByteCount
/// readable bytes.
struct AddressedKey {
  va_t Address;
  Pubkey Key;
};

/// Ordinals of the comparison signature recovery reads:
///
///   sol_memcmp_(left, right, length, result)
enum class CompareArgument : unsigned { Left = 0, Right, Length, Result };

constexpr unsigned argumentRegister(CompareArgument Argument) {
  return kFirstArgumentRegister + static_cast<unsigned>(Argument);
}

/// Everything a program point has proven about memory: the bytes the image
/// maps, plus the bytes the program wrote into its own frame and heap.
///
/// A program assembles the argument of an invocation or a derivation in its
/// own scratch memory, so reading only the image would see the pointer and
/// none of what it points at.
class BlockMemory final : public MemorySource {
public:
  BlockMemory(const ProgramImage &Image, const MemoryModel &Scratch)
      : Mapped(Image), Scratch(Scratch) {}

  std::optional<uint64_t> readWord(va_t Address) const override {
    if (const std::optional<uint64_t> Written = Scratch.readWord(Address))
      return Written;
    return Mapped.readWord(Address);
  }

  llvm::ArrayRef<uint8_t> readBytes(va_t Address, size_t Size) const override {
    if (const llvm::ArrayRef<uint8_t> Written = Scratch.read(Address, Size);
        !Written.empty())
      return Written;
    return Mapped.readBytes(Address, Size);
  }

private:
  ImageMemorySource Mapped;
  const MemoryModel &Scratch;
};

class Recovery {
public:
  Recovery(const SBFProgram &Program, const SolanaRecoveryOptions &Options)
      : Program(Program), Options(Options), Index(Program.Med),
        Flow(Program, Index) {}

  SolanaModel run();

private:
  /// Read a key from \p Address when the image maps that many data bytes.
  std::optional<AddressedKey> readKeyAt(va_t Address) const;

  /// The constant a register holds, if it holds one.
  static std::optional<uint64_t> constantOf(const RegisterValue &Value);

  /// The address a register designates, whether by constant or by frame
  /// offset.
  static std::optional<va_t> addressOf(const RegisterValue &Value);

  void noteKey(va_t Address, const Pubkey &Key, bool ReferencedByCode);

  void visitMemoryAccess(const MedInstruction &Instruction,
                         const MachineState &State);
  void visitComparison(const MedInstruction &Instruction,
                       const MachineState &State);
  void visitSyscall(const MedInstruction &Instruction,
                    const MachineState &State);
  void visitInvocation(const MedInstruction &Instruction,
                       const MachineState &State, const SyscallInfo &Info);
  void visitDerivation(const MedInstruction &Instruction,
                       const MachineState &State, const SyscallInfo &Info);

  void visitExit(const MedInstruction &Instruction, const MachineState &State);

  /// Name the operation the payload of \p Site selects.
  void nameInvokedOperation(CPISite &Site, llvm::ArrayRef<uint8_t> Payload);

  /// Attach a name to a discriminator, from the supplied IDL first and the
  /// built-in dictionary second. Returns false when neither knows it.
  bool nameDiscriminator(const AnchorDiscriminator &Value, std::string &Name,
                         AnchorNamespace &Namespace,
                         std::optional<RecoveryEvidence> &Evidence) const;

  void scanReadOnlyData();
  void resolveHandlers();
  void resolveErrors();
  void addLints();

  const SBFProgram &Program;
  const SolanaRecoveryOptions &Options;
  MedInstructionIndex Index;
  ScratchFlow Flow;
  SolanaModel Model;

  /// Discriminator candidates in program order, before name resolution.
  struct Candidate {
    uint64_t Word;
    size_t CompareSlot;
    std::optional<size_t> TargetSlot;
  };
  std::vector<Candidate> Candidates;

  /// Constants that reach a return, before they are judged to be error codes.
  std::vector<ReturnedError> ErrorCandidates;

  /// Keys already recorded, so one address reported twice stays one entry.
  llvm::DenseMap<uint64_t, size_t> KeyIndexByFirstWord;
};

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
      for (CompareArgument Side : {CompareArgument::Left, CompareArgument::Right})
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
  const bool IdlDescribesTarget =
      Options.Idl && Options.Idl->Address && Site.ProgramId &&
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

void Recovery::scanReadOnlyData() {
  // Index both tables by their leading word so a region scan costs one hash
  // probe per offset instead of one comparison per table entry.
  llvm::DenseMap<uint64_t, const KnownAddressInfo *> AddressByFirstWord;
  for (const KnownAddressInfo &Info : knownAddressInfos()) {
    // The all-zero System Program address would match every zero run in
    // read-only data, so it is only recognized when code references it.
    if (!Info.Decoded || Info.Key.isZero())
      continue;
    AddressByFirstWord.try_emplace(
        llvm::support::endian::read64le(Info.Key.Bytes.data()), &Info);
  }

  // A discriminator is only eight bytes, so one probe is the whole comparison.
  // Anchor derives them by truncating SHA-256, which makes an accidental match
  // as unlikely as a collision in that hash.
  llvm::DenseSet<uint64_t> NamedWords;
  for (const AnchorNameInfo &Info : anchorNameInfos())
    NamedWords.insert(Info.Discriminator.toWord());
  if (Options.Idl)
    for (const AnchorIdlItem &Item : Options.Idl->Items)
      NamedWords.insert(Item.Discriminator.toWord());

  for (const ProgramRegion &Region : Program.ExecutableImage.regions()) {
    if (!Region.DataVisible || Region.Bytes.size() < kAnchorDiscriminatorLength)
      continue;
    const size_t LastWord = Region.Bytes.size() - kAnchorDiscriminatorLength;
    const bool HoldsKey = Region.Bytes.size() >= kPubkeyByteCount;
    const size_t LastKey = HoldsKey ? Region.Bytes.size() - kPubkeyByteCount : 0;

    for (size_t Offset = 0; Offset <= LastWord; ++Offset) {
      const uint64_t Word =
          llvm::support::endian::read64le(Region.Bytes.data() + Offset);

      if (NamedWords.contains(Word)) {
        RecoveredDiscriminator Found;
        Found.Address = Region.Address + Offset;
        Found.Value = AnchorDiscriminator::fromWord(Word);
        std::optional<RecoveryEvidence> Evidence;
        if (nameDiscriminator(Found.Value, Found.Name, Found.Namespace,
                              Evidence)) {
          Found.Evidence = *Evidence;
          Model.Discriminators.push_back(std::move(Found));
        }
      }

      if (!HoldsKey || Offset > LastKey)
        continue;
      auto Address = AddressByFirstWord.find(Word);
      if (Address == AddressByFirstWord.end())
        continue;
      const llvm::ArrayRef<uint8_t> Window =
          llvm::ArrayRef(Region.Bytes).slice(Offset, kPubkeyByteCount);
      if (!llvm::equal(Window, Address->second->Key.Bytes))
        continue;
      noteKey(Region.Address + Offset, Address->second->Key,
              /*ReferencedByCode=*/false);
    }
  }
}

void Recovery::resolveHandlers() {
  std::vector<AnchorHandler> Resolved;
  Resolved.reserve(Candidates.size());
  bool AnyNamed = false;

  for (const Candidate &Entry : Candidates) {
    AnchorHandler Handler;
    Handler.Discriminator = AnchorDiscriminator::fromWord(Entry.Word);
    Handler.CompareSlot = Entry.CompareSlot;
    Handler.TargetSlot = Entry.TargetSlot;

    AnyNamed |= nameDiscriminator(Handler.Discriminator, Handler.Name,
                                  Handler.Namespace, Handler.NameEvidence);
    Resolved.push_back(std::move(Handler));
  }

  // A 64-bit constant comparison only means "discriminator" once at least one
  // of them is a known Anchor name. Without that anchor, these are ordinary
  // constant comparisons and reporting them would be a guess.
  if (!AnyNamed)
    return;
  Model.IsAnchor = true;
  Model.Handlers = std::move(Resolved);
  if (Options.Idl)
    Model.IdlName = Options.Idl->Name;
}

void Recovery::resolveErrors() {
  // A number in a framework band is only a framework error code when the
  // program uses that framework. In anything else the same value is an
  // ordinary constant, and reporting it would invent a diagnosis.
  for (const ReturnedError &Candidate : ErrorCandidates) {
    if (!Candidate.Classification.Known && !Model.IsAnchor)
      continue;
    const bool Duplicate =
        llvm::any_of(Model.Errors, [&](const ReturnedError &Recorded) {
          return Recorded.Code == Candidate.Code;
        });
    if (!Duplicate)
      Model.Errors.push_back(Candidate);
  }
  llvm::stable_sort(Model.Errors,
                    [](const ReturnedError &Left, const ReturnedError &Right) {
                      return Left.Code < Right.Code;
                    });
}

void Recovery::addLints() {
  auto Report = [&](Lint ID, std::optional<size_t> Slot, std::string Detail) {
    Model.Findings.push_back({ID, Slot, std::move(Detail)});
  };

  for (const CPISite &Site : Model.CPISites) {
    if (!Site.ProgramId)
      Report(Lint::UnresolvedCPITarget, Site.Slot,
             "the invoked program id is computed at run time");
    if (Site.Selected &&
        Site.Selected->Status == InstructionStatus::Deprecated)
      Report(Lint::DeprecatedProgramInstruction, Site.Slot,
             (Site.KnownProgram->Name + "::" + Site.Selected->Name).str());
  }

  // The signer and owner checks need at least one resolvable account access;
  // without one there is no evidence either way, and silence is the honest
  // report.
  if (!Model.AccountAccesses.empty()) {
    auto Reads = [&](AccountField Field) {
      return llvm::any_of(
          Model.AccountAccesses,
          [&](const AccountAccess &Access) { return Access.Field == Field; });
    };
    if (!Model.CPISites.empty() && !Reads(AccountField::IsSigner))
      Report(Lint::MissingSignerCheck, std::nullopt,
             "no recovered access reads is_signer");
    if (!Reads(AccountField::Owner))
      Report(Lint::MissingOwnerCheck, std::nullopt,
             "no recovered access reads owner");
  }

  if (Model.ProgramId && !Model.IsAnchor)
    Report(Lint::MissingDiscriminatorCheck, std::nullopt,
           "the entry point compares its own program id but dispatches no "
           "recognized discriminator");

  for (const SyscallUse &Use : Program.High.Syscalls) {
    if (!Use.Info)
      continue;
    // "This name is on its way out" and "this call does not resolve on the
    // chain you asked about" are different warnings, and a reader acts on them
    // differently: one is a rewrite to schedule, the other is a program that
    // will not run.
    if (Use.Info->Lifecycle == SyscallLifecycle::Deprecated)
      Report(Lint::DeprecatedSyscall, Use.Slot, Use.Info->Name.str());
    switch (syscallRegistration(Use.Info->ID, Options.Profile)) {
    case SyscallRegistration::Registered:
      break;
    case SyscallRegistration::GateUnmet:
      Report(Lint::FeatureGatedSyscall, Use.Slot,
             Use.Info->Name.str() + " is not registered on " +
                 clusterName(Options.Profile.OnCluster).str());
      break;
    case SyscallRegistration::EnvironmentExcluded:
      Report(Lint::FeatureGatedSyscall, Use.Slot,
             Use.Info->Name.str() + " is not in the " +
                 runtimePurposeName(Options.Profile.Purpose).str() +
                 " registry");
      break;
    }
  }

  if (Program.Low.TheVersion < Version::V3)
    Report(Lint::LegacyDeploymentVersion, std::nullopt,
           versionDisplayName(Program.Low.TheVersion).str());
}

SolanaModel Recovery::run() {
  scanReadOnlyData();

  for (const MedBlock &Block : Program.Med.Blocks)
    replayBlock(
        Index, Block, Flow.entryState(Block.ID), Program.ExecutableImage,
        [&](const MedInstruction &Instruction, const MachineState &State) {
          if (Instruction.Op == Operation::Load ||
              Instruction.Op == Operation::Store)
            visitMemoryAccess(Instruction, State);
          else if (Instruction.Op == Operation::Eq ||
                   Instruction.Op == Operation::Ne)
            visitComparison(Instruction, State);
          else if (Instruction.Op == Operation::Exit)
            visitExit(Instruction, State);
          if (Instruction.Call == CallKind::Syscall && Instruction.Syscall)
            visitSyscall(Instruction, State);
        });

  resolveHandlers();
  resolveErrors();
  addLints();

  llvm::stable_sort(Model.Pubkeys, [](const RecoveredPubkey &Left,
                                      const RecoveredPubkey &Right) {
    return Left.Address < Right.Address;
  });
  return std::move(Model);
}

} // namespace

SolanaModel recoverSolanaModel(const SBFProgram &Program,
                               const SolanaRecoveryOptions &Options) {
  if (Program.Med.Blocks.empty())
    return {};
  return Recovery(Program, Options).run();
}

std::string dumpSolanaModel(const SolanaModel &Model) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);

  if (Model.ProgramId)
    OS << "program-id " << formatPubkey(*Model.ProgramId) << "\n";
  if (Model.IsAnchor)
    OS << "framework anchor" << (Model.IdlName.empty() ? "" : " idl=")
       << Model.IdlName << "\n";

  for (const RecoveredPubkey &Key : Model.Pubkeys) {
    OS << "pubkey 0x" << llvm::utohexstr(Key.Address) << " "
       << formatPubkey(Key.Key);
    if (Key.Known)
      OS << " " << Key.Known->Name << " ("
         << knownAddressCategoryName(Key.Known->Category) << ")";
    if (Key.ReferencedByCode)
      OS << " referenced";
    OS << "\n";
  }

  for (const AnchorHandler &Handler : Model.Handlers) {
    OS << "handler slot " << Handler.CompareSlot << " 0x"
       << llvm::toHex(Handler.Discriminator.Bytes, /*LowerCase=*/true);
    if (!Handler.Name.empty())
      OS << " " << anchorNamespaceSpelling(Handler.Namespace) << " "
         << Handler.Name << " (" << recoveryEvidenceName(*Handler.NameEvidence)
         << ")";
    if (Handler.TargetSlot)
      OS << " -> slot " << *Handler.TargetSlot;
    OS << "\n";
  }

  for (const RecoveredDiscriminator &Found : Model.Discriminators)
    OS << "discriminator 0x" << llvm::utohexstr(Found.Address) << " "
       << anchorNamespaceSpelling(Found.Namespace) << " " << Found.Name << " ("
       << recoveryEvidenceName(Found.Evidence) << ")\n";

  for (const CPISite &Site : Model.CPISites) {
    OS << "cpi slot " << Site.Slot;
    if (const SyscallInfo *Info = getSyscallInfo(Site.Which))
      OS << " " << Info->Name;
    if (Site.ProgramId)
      OS << " -> " << formatPubkey(*Site.ProgramId);
    if (Site.KnownProgram)
      OS << " " << Site.KnownProgram->Name;
    if (Site.Selected)
      OS << "::" << Site.Selected->Name;
    else if (!Site.Name.empty())
      OS << "::" << Site.Name << " ("
         << recoveryEvidenceName(*Site.NameEvidence) << ")";
    else if (Site.Discriminator)
      OS << "::0x"
         << llvm::toHex(Site.Discriminator->Bytes, /*LowerCase=*/true);
    if (Site.AccountCount)
      OS << " accounts=" << *Site.AccountCount;
    if (Site.DataLength)
      OS << " data=" << *Site.DataLength;
    OS << "\n";
  }

  for (const PDADerivation &Derivation : Model.Derivations) {
    OS << "pda slot " << Derivation.Slot;
    if (const SyscallInfo *Info = getSyscallInfo(Derivation.Which))
      OS << " " << Info->Name;
    if (Derivation.DeclaredSeedCount)
      OS << " seeds=" << Derivation.Seeds.size() << "/"
         << *Derivation.DeclaredSeedCount;
    if (Derivation.KnownProgram)
      OS << " " << Derivation.KnownProgram->Name;
    else if (Derivation.ProgramId)
      OS << " " << formatPubkey(*Derivation.ProgramId);
    OS << "\n";
    for (const RecoveredSeed &Seed : Derivation.Seeds) {
      OS << "  seed";
      if (Seed.Address)
        OS << " 0x" << llvm::utohexstr(*Seed.Address);
      if (Seed.Length)
        OS << " len " << *Seed.Length;
      if (Seed.isText())
        OS << " \"" << llvm::StringRef(reinterpret_cast<const char *>(
                            Seed.Bytes.data()),
                                       Seed.Bytes.size())
           << "\"";
      else if (!Seed.Bytes.empty())
        OS << " 0x" << llvm::toHex(Seed.Bytes, /*LowerCase=*/true);
      OS << "\n";
    }
  }

  for (const ReturnedError &Error : Model.Errors) {
    OS << "error " << Error.Code;
    if (const AnchorErrorInfo *Known = Error.Classification.Known)
      OS << " " << Known->Name << ": " << Known->Message;
    else if (Error.Classification.CustomOrdinal)
      OS << " " << Error.Classification.Range->Name << " #"
         << *Error.Classification.CustomOrdinal;
    else
      OS << " " << Error.Classification.Range->Name;
    OS << "\n";
  }

  for (const AccountAccess &Access : Model.AccountAccesses) {
    OS << (Access.IsWrite ? "input-store slot " : "input-load slot ")
       << Access.Slot << " +" << Access.InputOffset;
    if (Access.Header)
      OS << " " << getInputFieldInfo(*Access.Header).Name;
    if (Access.Field)
      OS << " accounts[0]." << getAccountFieldName(*Access.Field).Name;
    OS << "\n";
  }

  for (const LintFinding &Finding : Model.Findings) {
    const LintInfo &Info = getLintInfo(Finding.ID);
    OS << lintSeverityName(Info.Severity) << " " << Info.Name << " ["
       << lintConfidenceName(Info.Confidence) << "]";
    if (Finding.Slot)
      OS << " slot " << *Finding.Slot;
    if (!Finding.Detail.empty())
      OS << ": " << Finding.Detail;
    OS << "\n";
  }

  return Buffer;
}

} // namespace neverd::sbf
