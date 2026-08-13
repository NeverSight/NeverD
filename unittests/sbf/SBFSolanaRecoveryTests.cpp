//===- SBFSolanaRecoveryTests.cpp - End-to-end Solana recovery ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFSolanaTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/solana/SBFAnchor.h"
#include "neverd/sbf/solana/SBFCPI.h"
#include "neverd/sbf/solana/SBFKnownAddresses.h"
#include "neverd/sbf/solana/SBFPubkey.h"
#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace {

using test::encode;
using test::EncodedInstruction;
using test::loadImm64;
using test::makeImage;
using test::RodataBuilder;

//===----------------------------------------------------------------------===//
// End-to-end recovery
//===----------------------------------------------------------------------===//

/// Read-only data holding the program's own address, a sysvar, and an SPL
/// program address, laid out one key after another.
struct RodataLayout {
  std::vector<uint8_t> Bytes;
  va_t OwnIdAddress = 0;
  Pubkey OwnId;
};

RodataLayout makeRodata() {
  RodataLayout Layout;
  const auto Append = [&](const Pubkey &Key) {
    va_t Address = kRodataStartV3 + Layout.Bytes.size();
    Layout.Bytes.insert(Layout.Bytes.end(), Key.Bytes.begin(), Key.Bytes.end());
    return Address;
  };

  // An address that is deliberately not in the well-known table, standing in
  // for the address a program declares for itself.
  Layout.OwnId.Bytes.fill(0x11);
  Layout.OwnIdAddress = Append(Layout.OwnId);
  Append(getKnownAddressInfo(KnownAddress::SysvarRent)->Key);
  Append(getKnownAddressInfo(KnownAddress::SplToken)->Key);
  return Layout;
}

TEST(SBFSolanaRecovery, RecoversAnAnchorStyleDispatchAndItsAddresses) {
  const RodataLayout Rodata = makeRodata();
  const AnchorDiscriminator Deposit =
      anchorDiscriminator(AnchorNamespace::Instruction, "deposit");

  std::vector<EncodedInstruction> Text;
  const auto Append = [&](std::vector<EncodedInstruction> Slots) {
    Text.insert(Text.end(), Slots.begin(), Slots.end());
  };

  // The loader passes the serialized input in r1, so these two reads land on
  // named fields of the first account entry.
  const uint64_t Account = firstAccountOffset();
  const auto FieldOffset = [&](AccountField Field) {
    const AccountLayoutInfo *Info =
        getAccountFieldInfo(AccountABI::V1, Field);
    return static_cast<int16_t>(Account + (Info ? Info->Offset : 0));
  };
  Text.push_back(encode(Opcode::LD_DW_REG, 8, kFirstArgumentRegister,
                        FieldOffset(AccountField::Lamports)));
  Text.push_back(encode(Opcode::LD_B_REG, 9, kFirstArgumentRegister,
                        FieldOffset(AccountField::IsSigner)));

  // Compare the incoming program id against the address in read-only data,
  // which is how a generated entry point proves its own identity.
  Append(loadImm64(2, Rodata.OwnIdAddress));
  Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, kPubkeyByteCount));
  Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                        static_cast<int32_t>(hashSymbolName("sol_memcmp_"))));

  // Dispatch on a 64-bit discriminator held in a callee-saved register.
  Append(loadImm64(6, Deposit.toWord()));
  const size_t CompareSlot = Text.size();
  Text.push_back(encode(Opcode::JEQ64_REG, 7, 6, 1));
  Text.push_back(encode(Opcode::EXIT));
  Text.push_back(encode(Opcode::EXIT));

  auto Program = analyze(makeImage(Text, Rodata.Bytes));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const SolanaModel &Model = Program->High.Solana;

  ASSERT_TRUE(Model.ProgramId.has_value());
  EXPECT_EQ(*Model.ProgramId, Rodata.OwnId);

  EXPECT_TRUE(Model.IsAnchor);
  ASSERT_EQ(Model.Handlers.size(), 1u);
  const AnchorHandler &Handler = Model.Handlers.front();
  EXPECT_EQ(Handler.Discriminator, Deposit);
  EXPECT_EQ(Handler.Name, "deposit");
  EXPECT_EQ(Handler.Namespace, AnchorNamespace::Instruction);
  ASSERT_TRUE(Handler.NameEvidence.has_value());
  EXPECT_EQ(*Handler.NameEvidence, RecoveryEvidence::AnchorDictionary);
  EXPECT_EQ(Handler.CompareSlot, CompareSlot);
  // An equality branch selects its arm by being taken.
  EXPECT_EQ(Handler.TargetSlot, CompareSlot + 2);

  // The table scan finds protocol addresses whether or not code references
  // them; the memcmp argument additionally proves a code reference.
  const auto FindKey = [&](const Pubkey &Key) -> const RecoveredPubkey * {
    for (const RecoveredPubkey &Recovered : Model.Pubkeys)
      if (Recovered.Key == Key)
        return &Recovered;
    return nullptr;
  };
  const RecoveredPubkey *Own = FindKey(Rodata.OwnId);
  ASSERT_NE(Own, nullptr);
  EXPECT_EQ(Own->Address, Rodata.OwnIdAddress);
  EXPECT_TRUE(Own->ReferencedByCode);
  EXPECT_EQ(Own->Known, nullptr);
  EXPECT_EQ(Own->Evidence, RecoveryEvidence::ConstantDataflow);

  const RecoveredPubkey *Rent =
      FindKey(getKnownAddressInfo(KnownAddress::SysvarRent)->Key);
  ASSERT_NE(Rent, nullptr);
  ASSERT_NE(Rent->Known, nullptr);
  EXPECT_EQ(Rent->Known->Name, "sysvar::rent");
  EXPECT_FALSE(Rent->ReferencedByCode);
  EXPECT_EQ(Rent->Evidence, RecoveryEvidence::KnownAddressTable);
  ASSERT_NE(FindKey(getKnownAddressInfo(KnownAddress::SplToken)->Key), nullptr);

  // Reported addresses are ordered by where they live.
  EXPECT_TRUE(std::is_sorted(
      Model.Pubkeys.begin(), Model.Pubkeys.end(),
      [](const RecoveredPubkey &Left, const RecoveredPubkey &Right) {
        return Left.Address < Right.Address;
      }));

  ASSERT_EQ(Model.AccountAccesses.size(), 2u);
  EXPECT_EQ(Model.AccountAccesses[0].Field, AccountField::Lamports);
  EXPECT_FALSE(Model.AccountAccesses[0].IsWrite);
  EXPECT_EQ(Model.AccountAccesses[1].Field, AccountField::IsSigner);

  // Reading is_signer with no cross-program invocation leaves nothing to warn
  // about, but nothing reads owner.
  const auto Triggered = [&](Lint ID) {
    return std::any_of(Model.Findings.begin(), Model.Findings.end(),
                       [&](const LintFinding &F) { return F.ID == ID; });
  };
  EXPECT_TRUE(Triggered(Lint::MissingOwnerCheck));
  EXPECT_FALSE(Triggered(Lint::MissingSignerCheck));
  EXPECT_FALSE(Triggered(Lint::LegacyDeploymentVersion));

  const std::string Dump = dumpSolanaModel(Model);
  EXPECT_NE(Dump.find("program-id " + formatPubkey(Rodata.OwnId)),
            std::string::npos);
  EXPECT_NE(Dump.find("framework anchor"), std::string::npos);
  EXPECT_NE(Dump.find("deposit"), std::string::npos);
  EXPECT_NE(Dump.find("sysvar::rent"), std::string::npos);
  EXPECT_NE(Dump.find("accounts[0].lamports"), std::string::npos);
}

TEST(SBFSolanaRecovery, PrefersASuppliedIDLOverTheBuiltInDictionary) {
  const RodataLayout Rodata = makeRodata();
  const AnchorDiscriminator Deposit =
      anchorDiscriminator(AnchorNamespace::Instruction, "deposit");

  std::vector<EncodedInstruction> Text;
  for (const EncodedInstruction &Slot : loadImm64(6, Deposit.toWord()))
    Text.push_back(Slot);
  Text.push_back(encode(Opcode::JEQ64_REG, 7, 6, 1));
  Text.push_back(encode(Opcode::EXIT));
  Text.push_back(encode(Opcode::EXIT));

  auto Program = analyze(makeImage(Text, Rodata.Bytes));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  // The dictionary alone names this arm "deposit".
  ASSERT_EQ(Program->High.Solana.Handlers.size(), 1u);
  EXPECT_EQ(Program->High.Solana.Handlers.front().Name, "deposit");

  const std::string Document =
      R"({"metadata": {"name": "vault"}, "instructions": [{"name": ")" +
      std::string("deposit") + R"("}]})";
  llvm::Expected<AnchorIdl> Idl = parseAnchorIdl(Document);
  ASSERT_TRUE(static_cast<bool>(Idl)) << llvm::toString(Idl.takeError());

  SolanaRecoveryOptions Options;
  Options.Idl = &*Idl;
  const SolanaModel Model = recoverSolanaModel(*Program, Options);
  ASSERT_EQ(Model.Handlers.size(), 1u);
  EXPECT_EQ(Model.Handlers.front().Name, "deposit");
  ASSERT_TRUE(Model.Handlers.front().NameEvidence.has_value());
  EXPECT_EQ(*Model.Handlers.front().NameEvidence,
            RecoveryEvidence::SuppliedIdl);
  EXPECT_EQ(Model.IdlName, "vault");
}

TEST(SBFSolanaRecovery, DoesNotInventAnchorDispatchFromOrdinaryConstants) {
  // A 64-bit comparison that matches no known name is just a comparison.
  std::vector<EncodedInstruction> Text;
  for (const EncodedInstruction &Slot : loadImm64(6, 0x0123456789abcdefULL))
    Text.push_back(Slot);
  Text.push_back(encode(Opcode::JEQ64_REG, 7, 6, 1));
  Text.push_back(encode(Opcode::EXIT));
  Text.push_back(encode(Opcode::EXIT));

  auto Program = analyze(makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_FALSE(Program->High.Solana.IsAnchor);
  EXPECT_TRUE(Program->High.Solana.Handlers.empty());
  EXPECT_FALSE(Program->High.Solana.ProgramId.has_value());
  EXPECT_TRUE(Program->High.Solana.Pubkeys.empty());
}

TEST(SBFSolanaRecovery, ReportsAnUnresolvableInvocationTarget) {
  std::vector<EncodedInstruction> Text{
      encode(Opcode::CALL_IMM, 0, 0, 0,
             static_cast<int32_t>(hashSymbolName("sol_invoke_signed_rust"))),
      encode(Opcode::EXIT)};

  auto Program = analyze(makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const SolanaModel &Model = Program->High.Solana;

  ASSERT_EQ(Model.CPISites.size(), 1u);
  EXPECT_EQ(Model.CPISites.front().Which, Syscall::InvokeSignedRust);
  // The instruction pointer is the entry input pointer, which is not mapped in
  // the image, so the target must stay unresolved rather than be guessed.
  EXPECT_FALSE(Model.CPISites.front().ProgramId.has_value());
  ASSERT_FALSE(Model.Findings.empty());
  EXPECT_EQ(Model.Findings.front().ID, Lint::UnresolvedCPITarget);
  EXPECT_EQ(Model.Findings.front().Slot, 0u);
}

//===----------------------------------------------------------------------===//
// Returned error codes and read-only discriminators
//===----------------------------------------------------------------------===//

TEST(SBFSolanaRecovery, ReportsReturnedAnchorErrorCodesOnlyForAnchorPrograms) {
  const AnchorDiscriminator Deposit =
      anchorDiscriminator(AnchorNamespace::Instruction, "deposit");
  const AnchorErrorInfo *Seeds = classifyAnchorError(2006).Known;
  ASSERT_NE(Seeds, nullptr);

  const auto Build = [&](bool WithDispatch) {
    std::vector<EncodedInstruction> Text;
    if (WithDispatch) {
      for (const EncodedInstruction &Slot : loadImm64(6, Deposit.toWord()))
        Text.push_back(Slot);
      Text.push_back(encode(Opcode::JEQ64_REG, 7, 6, 1));
      Text.push_back(encode(Opcode::EXIT));
    }
    Text.push_back(
        encode(Opcode::MOV64_IMM, kReturnRegister, 0, 0, Seeds->Code));
    Text.push_back(encode(Opcode::EXIT));
    return Text;
  };

  auto Anchor = analyze(makeImage(Build(/*WithDispatch=*/true), {}));
  ASSERT_TRUE(static_cast<bool>(Anchor)) << llvm::toString(Anchor.takeError());
  ASSERT_EQ(Anchor->High.Solana.Errors.size(), 1u);
  const ReturnedError &Error = Anchor->High.Solana.Errors.front();
  EXPECT_EQ(Error.Code, Seeds->Code);
  ASSERT_NE(Error.Classification.Known, nullptr);
  EXPECT_EQ(Error.Classification.Known->Name, "ConstraintSeeds");
  EXPECT_NE(dumpSolanaModel(Anchor->High.Solana).find("ConstraintSeeds"),
            std::string::npos);

  // The framework declares this code, so it is recognizable on its own. A code
  // in the custom band only means something once the program is known to be an
  // Anchor program.
  auto Plain = analyze(makeImage(Build(/*WithDispatch=*/false), {}));
  ASSERT_TRUE(static_cast<bool>(Plain)) << llvm::toString(Plain.takeError());
  EXPECT_FALSE(Plain->High.Solana.IsAnchor);
  EXPECT_EQ(Plain->High.Solana.Errors.size(), 1u);
}

TEST(SBFSolanaRecovery, IgnoresOrdinaryReturnValues) {
  // Success and small constants are not failure codes, and reporting them
  // would bury the codes that are.
  std::vector<EncodedInstruction> Text{
      encode(Opcode::MOV64_IMM, kReturnRegister, 0, 0, 0),
      encode(Opcode::EXIT)};
  auto Program = analyze(makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Solana.Errors.empty());
}

TEST(SBFSolanaRecovery, FindsAccountDiscriminatorsSittingInReadOnlyData) {
  // Anchor stores an account's discriminator as a constant and compares the
  // head of account data against it, so these name the account types a program
  // declares even when no dispatch arm mentions them.
  const AnchorNameInfo *Account = nullptr;
  for (const AnchorNameInfo &Info : anchorNameInfos())
    if (Info.Namespace == AnchorNamespace::Account) {
      Account = &Info;
      break;
    }
  ASSERT_NE(Account, nullptr);

  RodataBuilder Rodata;
  const va_t At = Rodata.append(Account->Discriminator.Bytes);

  auto Program =
      analyze(makeImage({encode(Opcode::EXIT)}, Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const auto &Found = Program->High.Solana.Discriminators;
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found.front().Address, At);
  EXPECT_EQ(Found.front().Value, Account->Discriminator);
  EXPECT_EQ(Found.front().Namespace, AnchorNamespace::Account);
  EXPECT_EQ(Found.front().Name, Account->Name);
  EXPECT_EQ(Found.front().Evidence, RecoveryEvidence::AnchorDictionary);
}

TEST(SBFSolanaRecovery, StaysSilentWhenNothingIsProven) {
  auto Program = analyze(makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Solana.empty());
  EXPECT_TRUE(dumpSolanaModel(Program->High.Solana).empty());
}
} // namespace
} // namespace neverd::sbf
