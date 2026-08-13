//===- SBFSolanaModelTests.cpp - Solana domain table tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The tabulated half of the Solana model: addresses, Anchor names, the two
/// serialized layouts, the invocation ABIs and the error bands. Nothing here
/// analyzes a program; the recovery that reads these tables is tested in
/// SBFSolanaRecoveryTests.cpp and its neighbours.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/solana/SBFAnchor.h"
#include "neverd/sbf/solana/SBFCPI.h"
#include "neverd/sbf/solana/SBFKnownAddresses.h"
#include "neverd/sbf/solana/SBFPubkey.h"
#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace {

//===----------------------------------------------------------------------===//
// Base58 and addresses
//===----------------------------------------------------------------------===//

TEST(SBFPubkey, EncodesTheCanonicalSPLTokenAddress) {
  // The SPL Token program id is a fixed protocol constant, so its base58
  // spelling and its bytes pin the codec from both directions.
  constexpr llvm::StringLiteral Text(
      "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
  constexpr llvm::StringLiteral Hex(
      "06ddf6e1d765a193d9cbe146ceeb79ac1cb485ed5f5b37913a8cf5857eff00a9");

  llvm::Expected<Pubkey> Key = parsePubkey(Text);
  ASSERT_TRUE(static_cast<bool>(Key)) << llvm::toString(Key.takeError());
  EXPECT_EQ(llvm::toHex(Key->Bytes, /*LowerCase=*/true), Hex.str());
  EXPECT_EQ(formatPubkey(*Key), Text.str());
  EXPECT_FALSE(Key->isZero());
}

TEST(SBFPubkey, PreservesLeadingZeroBytesAsLeadingZeroDigits) {
  llvm::Expected<Pubkey> System =
      parsePubkey("11111111111111111111111111111111");
  ASSERT_TRUE(static_cast<bool>(System)) << llvm::toString(System.takeError());
  EXPECT_TRUE(System->isZero());
  EXPECT_EQ(formatPubkey(*System), "11111111111111111111111111111111");
  EXPECT_EQ(formatPubkey(*System).size(), kMinPubkeyStringLength);

  // One leading zero byte must survive as exactly one leading zero digit.
  Pubkey Key = *System;
  Key.Bytes.back() = 1;
  EXPECT_EQ(formatPubkey(Key).front(), kBase58ZeroDigit);
  llvm::Expected<Pubkey> RoundTrip = parsePubkey(formatPubkey(Key));
  ASSERT_TRUE(static_cast<bool>(RoundTrip))
      << llvm::toString(RoundTrip.takeError());
  EXPECT_EQ(*RoundTrip, Key);
}

TEST(SBFPubkey, RoundTripsEveryByteValueAndEveryLength) {
  std::vector<uint8_t> Bytes;
  for (size_t Length = 0; Length <= kPubkeyByteCount; ++Length) {
    SCOPED_TRACE(Length);
    const std::string Text = encodeBase58(Bytes);
    llvm::Expected<llvm::SmallVector<uint8_t>> Decoded = decodeBase58(Text);
    ASSERT_TRUE(static_cast<bool>(Decoded))
        << llvm::toString(Decoded.takeError());
    EXPECT_EQ(std::vector<uint8_t>(Decoded->begin(), Decoded->end()), Bytes);
    EXPECT_LE(Text.size(), maxBase58Length(Bytes.size()));
    // Walk the whole byte domain across the sweep rather than only small
    // values.
    Bytes.push_back(static_cast<uint8_t>(Length * 8 + 1));
  }
}

TEST(SBFPubkey, RejectsSpellingsThatAreNotAddresses) {
  // The alphabet deliberately omits these four ambiguous glyphs.
  for (char Ambiguous : {'0', 'O', 'I', 'l'}) {
    llvm::Expected<llvm::SmallVector<uint8_t>> Decoded =
        decodeBase58(std::string(1, Ambiguous));
    EXPECT_FALSE(static_cast<bool>(Decoded)) << Ambiguous;
    llvm::consumeError(Decoded.takeError());
  }

  for (llvm::StringRef Wrong :
       {"", "1", "abc", "TokenkegQfeZyiNwAJbNbGKPFXC"}) {
    llvm::Expected<Pubkey> Key = parsePubkey(Wrong);
    EXPECT_FALSE(static_cast<bool>(Key)) << Wrong.str();
    llvm::consumeError(Key.takeError());
  }

  llvm::Expected<Pubkey> Short = readPubkey(std::vector<uint8_t>(31, 0));
  EXPECT_FALSE(static_cast<bool>(Short));
  llvm::consumeError(Short.takeError());
}

TEST(SBFKnownAddresses, TableIsSelfConsistentAndLooksUpBothWays) {
  llvm::Error TableError = validateKnownAddressTable();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  for (const KnownAddressInfo &Info : knownAddressInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_TRUE(Info.Decoded);
    EXPECT_EQ(formatPubkey(Info.Key), Info.Text.str());
    EXPECT_EQ(findKnownAddress(Info.Key), &Info);
    EXPECT_EQ(getKnownAddressInfo(Info.ID), &Info);
    EXPECT_NE(knownAddressCategoryName(Info.Category), "unknown");
  }

  const KnownAddressInfo *System =
      getKnownAddressInfo(KnownAddress::SystemProgram);
  ASSERT_NE(System, nullptr);
  EXPECT_TRUE(System->Key.isZero());
  EXPECT_EQ(System->Category, KnownAddressCategory::NativeProgram);
  EXPECT_EQ(getKnownAddressInfo(KnownAddress::SysvarRent)->Category,
            KnownAddressCategory::Sysvar);
  EXPECT_EQ(getKnownAddressInfo(KnownAddress::SplToken)->Category,
            KnownAddressCategory::SplProgram);

  Pubkey Unlisted;
  Unlisted.Bytes.fill(0xab);
  EXPECT_EQ(findKnownAddress(Unlisted), nullptr);
}

//===----------------------------------------------------------------------===//
// Anchor
//===----------------------------------------------------------------------===//

TEST(SBFAnchor, DerivesTheReferenceDiscriminators) {
  // These are the values Anchor's own sighash helper produces; they are the
  // externally checkable anchor for the whole derivation.
  const auto Expect = [](AnchorNamespace Namespace, llvm::StringRef Name,
                         llvm::StringRef Hex) {
    const AnchorDiscriminator Value = anchorDiscriminator(Namespace, Name);
    EXPECT_EQ(llvm::toHex(Value.Bytes, /*LowerCase=*/true), Hex.str())
        << Name.str();
    EXPECT_EQ(AnchorDiscriminator::fromWord(Value.toWord()), Value);
  };
  Expect(AnchorNamespace::Instruction, "initialize", "afaf6d1f0d989bed");
  Expect(AnchorNamespace::Instruction, "deposit", "f223c68952e1f2b6");
  Expect(AnchorNamespace::Instruction, "withdraw", "b712469c946da122");
  Expect(AnchorNamespace::Instruction, "swap", "f8c69e91e17587c8");
  Expect(AnchorNamespace::Account, "GlobalConfig", "95089ccaa0fcb0d9");
  Expect(AnchorNamespace::Event, "TradeEvent", "bddb7fd34ee661ee");
}

TEST(SBFAnchor, DictionaryIsUniqueAndReversible) {
  std::set<std::string> Names;
  std::set<uint64_t> Words;
  for (const AnchorNameInfo &Info : anchorNameInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_TRUE(Names
                    .insert(anchorNamespaceSpelling(Info.Namespace).str() +
                            ":" + Info.Name.str())
                    .second);
    // A truncated SHA-256 collision inside the dictionary would silently
    // rename an instruction, so require the whole table to stay injective.
    EXPECT_TRUE(Words.insert(Info.Discriminator.toWord()).second);
    EXPECT_EQ(Info.Discriminator,
              anchorDiscriminator(Info.Namespace, Info.Name));
    EXPECT_EQ(findAnchorName(Info.Discriminator), &Info);
  }
  EXPECT_FALSE(anchorNameInfos().empty());
  EXPECT_EQ(findAnchorName(AnchorDiscriminator::fromWord(0)), nullptr);
}

TEST(SBFAnchor, ParsesModernAndLegacyIDLDocuments) {
  constexpr llvm::StringLiteral Modern(R"({
    "address": "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
    "metadata": {"name": "amm", "version": "0.1.0"},
    "instructions": [
      {"name": "custom_entry", "discriminator": [1, 2, 3, 4, 5, 6, 7, 8]}
    ],
    "accounts": [{"name": "Pool", "discriminator": [9, 9, 9, 9, 9, 9, 9, 9]}]
  })");

  llvm::Expected<AnchorIdl> Parsed = parseAnchorIdl(Modern);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->Name, "amm");
  EXPECT_EQ(Parsed->Version, "0.1.0");
  ASSERT_TRUE(Parsed->Address.has_value());
  EXPECT_EQ(formatPubkey(*Parsed->Address),
            "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
  ASSERT_EQ(Parsed->Items.size(), 2u);
  EXPECT_TRUE(Parsed->Skipped.empty());

  const AnchorDiscriminator Explicit{{1, 2, 3, 4, 5, 6, 7, 8}};
  const AnchorIdlItem *Found = Parsed->find(Explicit);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found->Name, "custom_entry");
  EXPECT_EQ(Found->Namespace, AnchorNamespace::Instruction);
  // An explicit discriminator must win over the name-derived one.
  EXPECT_NE(Found->Discriminator,
            anchorDiscriminator(AnchorNamespace::Instruction, "custom_entry"));

  // The legacy schema names items without declaring their discriminators.
  constexpr llvm::StringLiteral Legacy(R"({
    "version": "0.1.0", "name": "legacy",
    "instructions": [{"name": "initialize", "args": []}]
  })");
  llvm::Expected<AnchorIdl> Old = parseAnchorIdl(Legacy);
  ASSERT_TRUE(static_cast<bool>(Old)) << llvm::toString(Old.takeError());
  EXPECT_EQ(Old->Name, "legacy");
  EXPECT_FALSE(Old->Address.has_value());
  ASSERT_EQ(Old->Items.size(), 1u);
  EXPECT_EQ(Old->Items.front().Discriminator,
            anchorDiscriminator(AnchorNamespace::Instruction, "initialize"));
}

TEST(SBFAnchor, ReportsUnusableIDLEntriesInsteadOfDroppingThem) {
  constexpr llvm::StringLiteral Mixed(R"({
    "metadata": {"name": "mixed"},
    "instructions": [
      {"name": "wide", "discriminator": [1, 2, 3, 4, 5, 6, 7, 8, 9]},
      {"name": "usable", "discriminator": [1, 1, 1, 1, 1, 1, 1, 1]}
    ]
  })");
  llvm::Expected<AnchorIdl> Parsed = parseAnchorIdl(Mixed);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Items.size(), 1u);
  EXPECT_EQ(Parsed->Items.front().Name, "usable");
  ASSERT_EQ(Parsed->Skipped.size(), 1u);
  EXPECT_NE(Parsed->Skipped.front().find("wide"), std::string::npos);

  for (llvm::StringRef Invalid :
       {"[]", "{\"instructions\": [1]}", "{\"instructions\": [{}]}",
        "{\"address\": \"not-an-address\"}"}) {
    llvm::Expected<AnchorIdl> Bad = parseAnchorIdl(Invalid);
    EXPECT_FALSE(static_cast<bool>(Bad)) << Invalid.str();
    llvm::consumeError(Bad.takeError());
  }
}

//===----------------------------------------------------------------------===//
// Serialized input layout
//===----------------------------------------------------------------------===//

TEST(SBFAccountLayout, FixedFieldsTileTheirSpan) {
  llvm::Error LayoutError = validateAccountLayout();
  ASSERT_FALSE(static_cast<bool>(LayoutError))
      << llvm::toString(std::move(LayoutError));

  EXPECT_EQ(firstAccountOffset(),
            getInputFieldInfo(InputField::AccountCount).Size);

  for (const AccountABIInfo &ABI : accountABIInfos()) {
    SCOPED_TRACE(ABI.Name.str());
    const AccountLayoutInfo *Key =
        getAccountFieldInfo(ABI.ID, AccountField::Key);
    ASSERT_NE(Key, nullptr);
    EXPECT_EQ(Key->Size, kPubkeyByteCount);

    const AccountLayoutInfo *Data =
        getAccountFieldInfo(ABI.ID, AccountField::Data);
    ASSERT_NE(Data, nullptr);
    EXPECT_EQ(accountFixedSize(ABI.ID), Data->Offset);
    EXPECT_EQ(accountFieldAt(ABI.ID, Key->Offset), Key);
    EXPECT_EQ(accountFieldAt(ABI.ID, Data->Offset + 1), Data);
  }
}

TEST(SBFAccountLayout, TheTwoSerializationsDisagreeAboutWhatAnOffsetNames) {
  // This is the whole reason the layout is versioned. Both serializations
  // place a real field at offset three, and they are different fields, so a
  // reader that assumes one produces a plausible name for the other's bytes.
  const AccountLayoutInfo *Unaligned = accountFieldAt(AccountABI::V0, 3);
  const AccountLayoutInfo *Aligned = accountFieldAt(AccountABI::V1, 3);
  ASSERT_NE(Unaligned, nullptr);
  ASSERT_NE(Aligned, nullptr);
  EXPECT_EQ(Unaligned->Field, AccountField::Key);
  EXPECT_EQ(Aligned->Field, AccountField::Executable);

  // Only the aligned form places the owner at a fixed offset; the other writes
  // it after the account's data, where nothing static can find it.
  EXPECT_EQ(getAccountFieldInfo(AccountABI::V0, AccountField::Owner), nullptr);
  EXPECT_NE(getAccountFieldInfo(AccountABI::V1, AccountField::Owner), nullptr);

  // A repeated account occupies a different number of bytes in each, which is
  // what misaligns a walk over the entries rather than one field.
  EXPECT_NE(getAccountABIInfo(AccountABI::V0).DuplicateEntrySize,
            getAccountABIInfo(AccountABI::V1).DuplicateEntrySize);
  EXPECT_LT(accountFixedSize(AccountABI::V0), accountFixedSize(AccountABI::V1));
}

TEST(SBFLints, CatalogIsIndexedByItsEnumerator) {
  for (const LintInfo &Info : lintInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getLintInfo(Info.ID), &Info);
    EXPECT_NE(lintSeverityName(Info.Severity), "unknown");
    EXPECT_NE(lintConfidenceName(Info.Confidence), "unknown");
    EXPECT_FALSE(Info.Summary.empty());
  }
}

//===----------------------------------------------------------------------===//
// Cross-program invocation ABI
//===----------------------------------------------------------------------===//

TEST(SBFCPIABI, DescribesBothLayoutsCompletely) {
  llvm::Error TableError = validateCPIABITables();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  for (const CPIABIInfo &Info : cpiABIInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getCPIABIInfo(Info.ID), &Info);
    EXPECT_EQ(findCPIABI(Info.Which), &Info);
    // Both layouts must name every field, or a reader would silently take an
    // offset of zero for one of them.
    for (const CPIFieldInfo &Field : Info.Fields)
      EXPECT_FALSE(Field.Name.empty());
    EXPECT_EQ(Info.field(CPIField::AccountCount).Form, CPIFieldForm::Count);
    EXPECT_EQ(Info.field(CPIField::DataLength).Form, CPIFieldForm::Length);
    EXPECT_EQ(Info.accountMetaField(CPIAccountMetaField::IsSigner).Form,
              CPIFieldForm::Flag);
  }
}

TEST(SBFCPIABI, PinsWhereEachLayoutKeepsTheInvokedProgramId) {
  // These two offsets are the whole reason the layouts cannot be shared, and
  // upstream asserts the Rust one in its own test. Reading either structure
  // with the other's offsets yields a plausible address rather than an error,
  // so nothing but this check would catch a swap.
  const CPIABIInfo &Rust = getCPIABIInfo(CPIABI::Rust);
  EXPECT_EQ(Rust.Which, Syscall::InvokeSignedRust);
  EXPECT_EQ(Rust.field(CPIField::ProgramId).Form, CPIFieldForm::InlineKey);
  EXPECT_EQ(Rust.field(CPIField::ProgramId).Offset, 48u);
  EXPECT_EQ(Rust.field(CPIField::AccountArray).Offset, 0u);
  EXPECT_EQ(Rust.AccountMetaSize, 34u);

  const CPIABIInfo &C = getCPIABIInfo(CPIABI::C);
  EXPECT_EQ(C.Which, Syscall::InvokeSignedC);
  EXPECT_EQ(C.field(CPIField::ProgramId).Form, CPIFieldForm::KeyAddress);
  EXPECT_EQ(C.field(CPIField::ProgramId).Offset, 0u);
  EXPECT_EQ(C.AccountMetaSize, 16u);
  // SolAccountMeta orders its two flags the opposite way from Rust's.
  EXPECT_LT(C.accountMetaField(CPIAccountMetaField::IsWritable).Offset,
            C.accountMetaField(CPIAccountMetaField::IsSigner).Offset);
  EXPECT_GT(Rust.accountMetaField(CPIAccountMetaField::IsWritable).Offset,
            Rust.accountMetaField(CPIAccountMetaField::IsSigner).Offset);

  // A seed descriptor is a pointer and a length, which both ABIs agree on.
  EXPECT_EQ(cpiSeedSize(), 2 * sizeof(uint64_t));
  EXPECT_EQ(getCPISeedFieldInfo(CPISeedField::Address).Offset, 0u);
}

TEST(SBFProgramInstructions, NameTheOperationsOfTabulatedPrograms) {
  llvm::Error TableError = validateProgramInstructionTables();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  // The token programs pack a one-byte selector; the system program's is the
  // four-byte variant index bincode writes.
  const std::vector<uint8_t> TokenTransfer{3, 1, 0, 0, 0, 0, 0, 0, 0};
  const ProgramInstructionInfo *Transfer =
      findProgramInstruction(KnownAddress::SplToken, TokenTransfer);
  ASSERT_NE(Transfer, nullptr);
  EXPECT_EQ(Transfer->Name, "transfer");
  EXPECT_EQ(Transfer->Status, InstructionStatus::Deprecated);
  // Token-2022 answers to the same numbering.
  EXPECT_EQ(findProgramInstruction(KnownAddress::SplToken2022, TokenTransfer),
            Transfer);

  const std::vector<uint8_t> SystemTransfer{2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
  const ProgramInstructionInfo *Lamports =
      findProgramInstruction(KnownAddress::SystemProgram, SystemTransfer);
  ASSERT_NE(Lamports, nullptr);
  EXPECT_EQ(Lamports->Name, "transfer");
  EXPECT_EQ(Lamports->Status, InstructionStatus::Current);
  // Reading that same payload as a one-byte selector would say "transfer" for
  // the token programs, which is why the encoding is per-program.
  EXPECT_EQ(instructionTagSize(InstructionTagEncoding::Word), 4u);

  // A selector the table does not list stays unnamed rather than being mapped
  // to a neighbour, and a program with no tabulated set names nothing.
  EXPECT_EQ(findProgramInstruction(KnownAddress::SystemProgram,
                                   std::vector<uint8_t>{200, 0, 0, 0}),
            nullptr);
  EXPECT_EQ(findProgramInstruction(KnownAddress::MplTokenMetadata,
                                   std::vector<uint8_t>{0}),
            nullptr);
  EXPECT_EQ(findProgramInstruction(KnownAddress::SplToken, {}), nullptr);
}

//===----------------------------------------------------------------------===//
// Anchor error codes
//===----------------------------------------------------------------------===//

TEST(SBFAnchorErrors, CatalogIsConsistentWithItsBands) {
  llvm::Error TableError = validateAnchorErrorTable();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  for (const AnchorErrorInfo &Info : anchorErrorInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_FALSE(Info.Message.empty());
    EXPECT_EQ(classifyAnchorError(Info.Code).Known, &Info);
  }
}

TEST(SBFAnchorErrors, ClassifiesListedUnlistedAndCustomCodes) {
  const AnchorErrorClassification Seeds = classifyAnchorError(2006);
  ASSERT_NE(Seeds.Known, nullptr);
  EXPECT_EQ(Seeds.Known->Name, "ConstraintSeeds");
  EXPECT_EQ(Seeds.Range->ID, AnchorErrorRange::Constraint);
  EXPECT_FALSE(Seeds.CustomOrdinal.has_value());

  // A code the framework has added since this table was written must still be
  // placed by its band rather than dropped, and a band ends exactly where the
  // next one begins.
  const AnchorErrorClassification Future = classifyAnchorError(2400);
  EXPECT_EQ(Future.Known, nullptr);
  ASSERT_NE(Future.Range, nullptr);
  EXPECT_EQ(Future.Range->ID, AnchorErrorRange::Constraint);

  const uint32_t RequireFirst =
      getAnchorErrorRangeInfo(AnchorErrorRange::Require).First;
  EXPECT_EQ(classifyAnchorError(RequireFirst - 1).Range->ID,
            AnchorErrorRange::Constraint);
  EXPECT_EQ(classifyAnchorError(RequireFirst).Range->ID,
            AnchorErrorRange::Require);

  const AnchorErrorClassification Custom =
      classifyAnchorError(anchorCustomErrorOffset() + 7);
  EXPECT_EQ(Custom.Known, nullptr);
  ASSERT_TRUE(Custom.CustomOrdinal.has_value());
  EXPECT_EQ(*Custom.CustomOrdinal, 7u);

  // Anything below the first band is an ordinary number, not a failure code.
  for (uint64_t Ordinary : {uint64_t{0}, uint64_t{1}, uint64_t{99}}) {
    SCOPED_TRACE(Ordinary);
    EXPECT_FALSE(classifyAnchorError(Ordinary).isMeaningful());
  }
}
} // namespace
} // namespace neverd::sbf
