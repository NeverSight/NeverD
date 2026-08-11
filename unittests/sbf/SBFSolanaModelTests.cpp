//===- SBFSolanaModelTests.cpp - Solana domain recovery tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/Anchor.h"
#include "neverd/sbf/CPI.h"
#include "neverd/sbf/KnownAddresses.h"
#include "neverd/sbf/Pubkey.h"
#include "neverd/sbf/SolanaRecovery.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <ostream>
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

//===----------------------------------------------------------------------===//
// End-to-end recovery
//===----------------------------------------------------------------------===//

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

EncodedInstruction encode(Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
                          int16_t Offset = 0, int32_t Immediate = 0) {
  EncodedInstruction Bytes{};
  const OpcodeInfo *Info = getOpcodeInfo(ID);
  EXPECT_NE(Info, nullptr);
  if (!Info)
    return Bytes;
  Bytes[kOpcodeOffset] = Info->Encoding;
  Bytes[kRegisterByteOffset] =
      static_cast<uint8_t>((Src << kRegisterEncodingBits) | Dst);
  llvm::support::endian::write16le(Bytes.data() + kBranchOffsetOffset,
                                   static_cast<uint16_t>(Offset));
  llvm::support::endian::write32le(Bytes.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  return Bytes;
}

/// Emit a 64-bit constant load as its two encoded slots.
std::vector<EncodedInstruction> loadImm64(uint8_t Dst, uint64_t Value) {
  EncodedInstruction High{};
  llvm::support::endian::write32le(High.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Value >> 32));
  return {encode(Opcode::LDDW, Dst, 0, 0, static_cast<int32_t>(Value)), High};
}

/// A strict v3 image whose read-only region holds \p Rodata.
BinaryImage makeImage(llvm::ArrayRef<EncodedInstruction> Instructions,
                      llvm::ArrayRef<uint8_t> Rodata) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  for (const EncodedInstruction &Instruction : Instructions)
    Image.Raw.insert(Image.Raw.end(), Instruction.begin(), Instruction.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = Image.Raw.size();
  Text.FileSz = Image.Raw.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  Section ReadOnly;
  ReadOnly.Name = kRodataSectionName.str();
  ReadOnly.VA = kRodataStartV3;
  ReadOnly.Size = Rodata.size();
  ReadOnly.FileSz = Rodata.size();
  ReadOnly.Alignment = kInstructionSize;
  ReadOnly.Data.assign(Rodata.begin(), Rodata.end());
  Image.Sections.push_back(std::move(ReadOnly));

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(Version::V3);
  Meta.Version = Version::V3;
  Meta.StrictLayout = true;
  Meta.TextFile = {0, Image.Raw.size()};
  Meta.TextVM = {kBytecodeStart, Image.Raw.size()};
  Meta.RodataVM = {kRodataStartV3, Rodata.size()};
  Image.SBF = Meta;
  return Image;
}

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
// Invocation recovery
//===----------------------------------------------------------------------===//

/// Read-only data assembled a piece at a time, tracking where each piece went.
class RodataBuilder {
public:
  va_t append(llvm::ArrayRef<uint8_t> Piece) {
    const va_t Address = kRodataStartV3 + Bytes.size();
    Bytes.insert(Bytes.end(), Piece.begin(), Piece.end());
    return Address;
  }

  va_t appendKey(const Pubkey &Key) { return append(Key.Bytes); }

  va_t appendText(llvm::StringRef Text) {
    return append({reinterpret_cast<const uint8_t *>(Text.data()),
                   Text.size()});
  }

  /// Reserve \p Size zero bytes and return where they start, so a structure
  /// can be filled in once the addresses it refers to are known.
  va_t reserve(size_t Size) {
    const va_t Address = kRodataStartV3 + Bytes.size();
    Bytes.resize(Bytes.size() + Size);
    return Address;
  }

  void putWord(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(&Bytes[Address - kRodataStartV3], Value);
  }

  void putKey(va_t Address, const Pubkey &Key) {
    std::copy(Key.Bytes.begin(), Key.Bytes.end(),
              Bytes.begin() + (Address - kRodataStartV3));
  }

  llvm::ArrayRef<uint8_t> bytes() const { return Bytes; }

private:
  std::vector<uint8_t> Bytes;
};

Pubkey filledKey(uint8_t Byte) {
  Pubkey Key;
  Key.Bytes.fill(Byte);
  return Key;
}

/// A serialized instruction layout as upstream declares it.
///
/// These offsets are written out here rather than read from the table under
/// test. A fixture assembled from that table would agree with any table,
/// including one whose two layouts had been swapped, which is the one mistake
/// these tests exist to catch.
struct ReferenceLayout {
  llvm::StringLiteral Name;
  Syscall Which;
  size_t Size;
  size_t AccountArray;
  size_t AccountCount;
  size_t Data;
  size_t DataLength;
  size_t ProgramId;
  bool ProgramIdIsInline;
  size_t AccountMetaSize;
  size_t MetaKey;
  bool MetaKeyIsInline;
  size_t MetaIsSigner;
  size_t MetaIsWritable;

  /// Without this a failure reports the parameter as a hex byte dump.
  friend void PrintTo(const ReferenceLayout &Layout, std::ostream *OS) {
    *OS << Layout.Name.str() << " layout";
  }
};

/// `solana-stable-layout`'s StableInstruction, whose own layout test pins the
/// key to offset 48: two StableVec headers of (pointer, capacity, length) come
/// first, and the key is stored in place after them.
constexpr ReferenceLayout kRustLayout{"rust",
                                      Syscall::InvokeSignedRust,
                                      /*Size=*/80,
                                      /*AccountArray=*/0,
                                      /*AccountCount=*/16,
                                      /*Data=*/24,
                                      /*DataLength=*/40,
                                      /*ProgramId=*/48,
                                      /*ProgramIdIsInline=*/true,
                                      /*AccountMetaSize=*/34,
                                      /*MetaKey=*/0,
                                      /*MetaKeyIsInline=*/true,
                                      /*MetaIsSigner=*/32,
                                      /*MetaIsWritable=*/33};

/// agave's SolInstruction: five words, with both keys behind pointers. Note
/// that SolAccountMeta orders its flags the opposite way from Rust's.
constexpr ReferenceLayout kCLayout{"c",
                                   Syscall::InvokeSignedC,
                                   /*Size=*/40,
                                   /*AccountArray=*/8,
                                   /*AccountCount=*/16,
                                   /*Data=*/24,
                                   /*DataLength=*/32,
                                   /*ProgramId=*/0,
                                   /*ProgramIdIsInline=*/false,
                                   /*AccountMetaSize=*/16,
                                   /*MetaKey=*/0,
                                   /*MetaKeyIsInline=*/false,
                                   /*MetaIsSigner=*/9,
                                   /*MetaIsWritable=*/8};

/// Lay out a serialized instruction as \p Layout describes, with an account
/// list whose first entry deliberately holds a different address from the
/// invoked program's, and return where the structure starts.
va_t buildInstruction(RodataBuilder &Rodata, const ReferenceLayout &Layout,
                      const Pubkey &ProgramId, const Pubkey &FirstAccount,
                      llvm::ArrayRef<uint8_t> Payload) {
  const va_t Meta = Rodata.reserve(Layout.AccountMetaSize);
  if (Layout.MetaKeyIsInline)
    Rodata.putKey(Meta + Layout.MetaKey, FirstAccount);
  else
    Rodata.putWord(Meta + Layout.MetaKey, Rodata.appendKey(FirstAccount));

  const va_t Data = Rodata.append(Payload);
  const va_t Instruction = Rodata.reserve(Layout.Size);

  Rodata.putWord(Instruction + Layout.AccountArray, Meta);
  Rodata.putWord(Instruction + Layout.AccountCount, 1);
  Rodata.putWord(Instruction + Layout.Data, Data);
  Rodata.putWord(Instruction + Layout.DataLength, Payload.size());

  if (Layout.ProgramIdIsInline)
    Rodata.putKey(Instruction + Layout.ProgramId, ProgramId);
  else
    Rodata.putWord(Instruction + Layout.ProgramId,
                   Rodata.appendKey(ProgramId));
  return Instruction;
}

/// Invoke the instruction at \p Address through \p Which.
std::vector<EncodedInstruction> invoke(Syscall Which, va_t Address) {
  const SyscallInfo *Info = getSyscallInfo(Which);
  EXPECT_NE(Info, nullptr);
  std::vector<EncodedInstruction> Text =
      loadImm64(kFirstArgumentRegister, Address);
  Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                        static_cast<int32_t>(hashSymbolName(Info->Name))));
  Text.push_back(encode(Opcode::EXIT));
  return Text;
}

class InvocationLayout : public testing::TestWithParam<ReferenceLayout> {};

TEST_P(InvocationLayout, TableAgreesWithTheDeclaredStructure) {
  const ReferenceLayout &Layout = GetParam();
  const CPIABIInfo *ABI = findCPIABI(Layout.Which);
  ASSERT_NE(ABI, nullptr);

  EXPECT_EQ(ABI->Size, Layout.Size);
  EXPECT_EQ(ABI->AccountMetaSize, Layout.AccountMetaSize);
  EXPECT_EQ(ABI->field(CPIField::AccountArray).Offset, Layout.AccountArray);
  EXPECT_EQ(ABI->field(CPIField::AccountCount).Offset, Layout.AccountCount);
  EXPECT_EQ(ABI->field(CPIField::Data).Offset, Layout.Data);
  EXPECT_EQ(ABI->field(CPIField::DataLength).Offset, Layout.DataLength);
  EXPECT_EQ(ABI->field(CPIField::ProgramId).Offset, Layout.ProgramId);
  EXPECT_EQ(ABI->field(CPIField::ProgramId).Form == CPIFieldForm::InlineKey,
            Layout.ProgramIdIsInline);
  EXPECT_EQ(ABI->accountMetaField(CPIAccountMetaField::Key).Offset,
            Layout.MetaKey);
  EXPECT_EQ(ABI->accountMetaField(CPIAccountMetaField::IsSigner).Offset,
            Layout.MetaIsSigner);
  EXPECT_EQ(ABI->accountMetaField(CPIAccountMetaField::IsWritable).Offset,
            Layout.MetaIsWritable);
}

TEST_P(InvocationLayout, ReadsTheInvokedProgramIdFromTheRightField) {
  // The two layouts disagree about what the instruction's first word is. A
  // reader that takes the Rust structure for the C one follows the account
  // vector and reports the first account as the invoked program, which is a
  // wrong answer rather than a missing one. Giving the two keys different
  // bytes is what makes that confusion visible here.
  const ReferenceLayout &Layout = GetParam();
  const Pubkey Target = filledKey(0xC0);
  const Pubkey FirstAccount = filledKey(0xA1);
  ASSERT_NE(Target, FirstAccount);

  RodataBuilder Rodata;
  const std::vector<uint8_t> Payload{9, 9, 9, 9};
  const va_t Instruction =
      buildInstruction(Rodata, Layout, Target, FirstAccount, Payload);

  auto Program =
      analyze(makeImage(invoke(Layout.Which, Instruction), Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const SolanaModel &Model = Program->High.Solana;
  ASSERT_EQ(Model.CPISites.size(), 1u);
  const CPISite &Site = Model.CPISites.front();
  ASSERT_NE(Site.ABI, nullptr);
  EXPECT_EQ(Site.ABI->Which, Layout.Which);
  ASSERT_TRUE(Site.ProgramId.has_value());
  EXPECT_EQ(*Site.ProgramId, Target);
  EXPECT_NE(*Site.ProgramId, FirstAccount);
  EXPECT_EQ(Site.AccountCount, 1u);
  EXPECT_EQ(Site.DataLength, Payload.size());

  // A resolved target leaves nothing to report as unresolved.
  EXPECT_FALSE(llvm::any_of(Model.Findings, [](const LintFinding &Finding) {
    return Finding.ID == Lint::UnresolvedCPITarget;
  }));
}

INSTANTIATE_TEST_SUITE_P(
    BothABIs, InvocationLayout, testing::Values(kRustLayout, kCLayout),
    [](const testing::TestParamInfo<ReferenceLayout> &Info) {
      return Info.param.Name.upper();
    });

TEST(SBFSolanaRecovery, NamesTheOperationAnInvokedTokenProgramIsAskedFor) {
  const KnownAddressInfo *Token = getKnownAddressInfo(KnownAddress::SplToken);
  ASSERT_NE(Token, nullptr);
  RodataBuilder Rodata;
  // Selector 3 with an eight-byte amount: the transfer the token program has
  // deprecated in favour of the checked form.
  const std::vector<uint8_t> Payload{3, 64, 0, 0, 0, 0, 0, 0, 0};
  const va_t Instruction =
      buildInstruction(Rodata, kRustLayout, Token->Key, filledKey(0xA1),
                       Payload);

  auto Program =
      analyze(makeImage(invoke(kRustLayout.Which, Instruction), Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const SolanaModel &Model = Program->High.Solana;
  ASSERT_EQ(Model.CPISites.size(), 1u);
  const CPISite &Site = Model.CPISites.front();
  ASSERT_NE(Site.KnownProgram, nullptr);
  EXPECT_EQ(Site.KnownProgram->ID, KnownAddress::SplToken);
  ASSERT_NE(Site.Selected, nullptr);
  EXPECT_EQ(Site.Selected->Name, "transfer");
  // A tabulated program's selector is a number, never a hashed discriminator.
  EXPECT_FALSE(Site.Discriminator.has_value());

  const auto Finding =
      llvm::find_if(Model.Findings, [](const LintFinding &Candidate) {
        return Candidate.ID == Lint::DeprecatedProgramInstruction;
      });
  ASSERT_NE(Finding, Model.Findings.end());
  EXPECT_EQ(Finding->Detail, "spl_token::transfer");

  EXPECT_NE(dumpSolanaModel(Model).find("spl_token::transfer"),
            std::string::npos);
}

TEST(SBFSolanaRecovery, NamesAnInvokedAnchorInstructionFromItsDiscriminator) {
  const AnchorDiscriminator Deposit =
      anchorDiscriminator(AnchorNamespace::Instruction, "deposit");

  RodataBuilder Rodata;
  std::vector<uint8_t> Payload(Deposit.Bytes.begin(), Deposit.Bytes.end());
  Payload.push_back(0);
  // A program that is not one of the canonical ones leads with a hashed
  // discriminator rather than a selector number.
  const va_t Instruction =
      buildInstruction(Rodata, kRustLayout, filledKey(0xC0),
                       filledKey(0xA1), Payload);

  auto Program =
      analyze(makeImage(invoke(kRustLayout.Which, Instruction), Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Solana.CPISites.size(), 1u);
  const CPISite &Site = Program->High.Solana.CPISites.front();
  EXPECT_EQ(Site.Selected, nullptr);
  ASSERT_TRUE(Site.Discriminator.has_value());
  EXPECT_EQ(*Site.Discriminator, Deposit);
  EXPECT_EQ(Site.Name, "deposit");
  ASSERT_TRUE(Site.NameEvidence.has_value());
  EXPECT_EQ(*Site.NameEvidence, RecoveryEvidence::AnchorDictionary);
}

//===----------------------------------------------------------------------===//
// Program-derived address recovery
//===----------------------------------------------------------------------===//

TEST(SBFSolanaRecovery, RecoversTheLiteralSeedsOfADerivedAddress) {
  // A program builds its seed array in its own frame and hands the runtime a
  // pointer to it, so the seeds are never in a register at the call. Reading
  // only the image would see the pointer and nothing it points at.
  constexpr llvm::StringLiteral Seed("vault");
  RodataBuilder Rodata;
  const va_t SeedText = Rodata.appendText(Seed);
  const Pubkey Owner = filledKey(0x33);
  const va_t OwnerAddress = Rodata.appendKey(Owner);

  const CPIFieldInfo &Pointer = getCPISeedFieldInfo(CPISeedField::Address);
  const CPIFieldInfo &Length = getCPISeedFieldInfo(CPISeedField::Length);
  const auto Base = static_cast<int16_t>(-64);

  std::vector<EncodedInstruction> Text;
  const auto Append = [&](std::vector<EncodedInstruction> Slots) {
    Text.insert(Text.end(), Slots.begin(), Slots.end());
  };

  // Write one seed descriptor into the frame.
  constexpr uint8_t Scratch = 6;
  Append(loadImm64(Scratch, SeedText));
  Text.push_back(encode(Opcode::ST_DW_REG, kFramePointerRegister, Scratch,
                        static_cast<int16_t>(Base + Pointer.Offset)));
  Text.push_back(encode(Opcode::MOV64_IMM, Scratch, 0, 0, Seed.size()));
  Text.push_back(encode(Opcode::ST_DW_REG, kFramePointerRegister, Scratch,
                        static_cast<int16_t>(Base + Length.Offset)));

  // sol_try_find_program_address(seeds, seed_count, program_id, out, bump)
  Text.push_back(encode(Opcode::MOV64_REG,
                        argumentRegister(DeriveArgument::Seeds),
                        kFramePointerRegister));
  Text.push_back(encode(Opcode::ADD64_IMM,
                        argumentRegister(DeriveArgument::Seeds), 0, 0, Base));
  Text.push_back(encode(Opcode::MOV64_IMM,
                        argumentRegister(DeriveArgument::SeedCount), 0, 0, 1));
  Append(loadImm64(argumentRegister(DeriveArgument::ProgramId), OwnerAddress));
  Text.push_back(
      encode(Opcode::CALL_IMM, 0, 0, 0,
             static_cast<int32_t>(hashSymbolName("sol_try_find_program_address"))));
  Text.push_back(encode(Opcode::EXIT));

  auto Program = analyze(makeImage(Text, Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const SolanaModel &Model = Program->High.Solana;
  ASSERT_EQ(Model.Derivations.size(), 1u);
  const PDADerivation &Derivation = Model.Derivations.front();
  EXPECT_EQ(Derivation.Which, Syscall::TryFindProgramAddress);
  EXPECT_EQ(Derivation.DeclaredSeedCount, 1u);
  EXPECT_TRUE(Derivation.complete());
  ASSERT_TRUE(Derivation.ProgramId.has_value());
  EXPECT_EQ(*Derivation.ProgramId, Owner);

  ASSERT_EQ(Derivation.Seeds.size(), 1u);
  const RecoveredSeed &Recovered = Derivation.Seeds.front();
  EXPECT_EQ(Recovered.Address, SeedText);
  EXPECT_EQ(Recovered.Length, Seed.size());
  EXPECT_TRUE(Recovered.isText());
  EXPECT_EQ(llvm::StringRef(reinterpret_cast<const char *>(
                                Recovered.Bytes.data()),
                            Recovered.Bytes.size()),
            Seed);

  EXPECT_NE(dumpSolanaModel(Model).find("\"vault\""), std::string::npos);
}

/// Write one seed descriptor at \p Base and derive an address from it, with
/// \p Interruption executed in between.
///
/// The interruption is always a call, so the descriptor and the derivation
/// necessarily land in different basic blocks: whether the seed survives is
/// decided by what that call can reach, not by where the block boundary fell.
std::vector<EncodedInstruction>
deriveAcross(va_t SeedText, size_t SeedLength, int16_t Base,
             llvm::ArrayRef<EncodedInstruction> Interruption) {
  std::vector<EncodedInstruction> Text;
  constexpr uint8_t Scratch = 6;
  for (const EncodedInstruction &Slot : loadImm64(Scratch, SeedText))
    Text.push_back(Slot);
  Text.push_back(
      encode(Opcode::ST_DW_REG, kFramePointerRegister, Scratch, Base));
  Text.push_back(encode(Opcode::MOV64_IMM, Scratch, 0, 0,
                        static_cast<int32_t>(SeedLength)));
  Text.push_back(encode(Opcode::ST_DW_REG, kFramePointerRegister, Scratch,
                        static_cast<int16_t>(Base + sizeof(uint64_t))));
  Text.insert(Text.end(), Interruption.begin(), Interruption.end());
  Text.push_back(encode(Opcode::MOV64_REG,
                        argumentRegister(DeriveArgument::Seeds),
                        kFramePointerRegister));
  Text.push_back(encode(Opcode::ADD64_IMM,
                        argumentRegister(DeriveArgument::Seeds), 0, 0, Base));
  Text.push_back(encode(Opcode::MOV64_IMM,
                        argumentRegister(DeriveArgument::SeedCount), 0, 0, 1));
  Text.push_back(encode(
      Opcode::CALL_IMM, 0, 0, 0,
      static_cast<int32_t>(hashSymbolName("sol_create_program_address"))));
  Text.push_back(encode(Opcode::EXIT));
  return Text;
}

TEST(SBFSolanaRecovery, KeepsTheFrameAcrossACallThatCannotWriteIt) {
  // sol_log_ reads the buffer it is given and writes nothing the caller can
  // see, so a seed descriptor written before it is still that descriptor at
  // the derivation that follows. Forgetting here would cost the seeds of every
  // program that logs on the way to deriving an address, which is most of them.
  constexpr llvm::StringLiteral Seed("vault");
  RodataBuilder Rodata;
  const va_t SeedText = Rodata.appendText(Seed);

  const std::array Interruption{
      encode(Opcode::CALL_IMM, 0, 0, 0,
             static_cast<int32_t>(hashSymbolName("sol_log_")))};
  auto Program = analyze(makeImage(
      deriveAcross(SeedText, Seed.size(), -64, Interruption), Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Solana.Derivations.size(), 1u);
  const PDADerivation &Derivation = Program->High.Solana.Derivations.front();
  EXPECT_TRUE(Derivation.complete());
  ASSERT_EQ(Derivation.Seeds.size(), 1u);
  EXPECT_EQ(Derivation.Seeds.front().Address, SeedText);
  EXPECT_TRUE(Derivation.Seeds.front().isText());
}

TEST(SBFSolanaRecovery, ForgetsTheFrameAcrossACallThatCouldReachIt) {
  // A function this analysis has not described, handed a pointer into this
  // frame, could have rewritten any of it. The descriptor written before it is
  // then no longer evidence about what the derivation reads.
  constexpr llvm::StringLiteral Seed("vault");
  RodataBuilder Rodata;
  const va_t SeedText = Rodata.appendText(Seed);

  // A call through a register names a function no static target identifies,
  // and the frame pointer in the first argument is a route into this frame.
  const std::array Interruption{
      encode(Opcode::MOV64_REG, kFirstArgumentRegister, kFramePointerRegister),
      encode(Opcode::CALL_REG, /*Dst=*/8)};
  auto Program = analyze(makeImage(
      deriveAcross(SeedText, Seed.size(), -64, Interruption), Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Solana.Derivations.size(), 1u);
  const PDADerivation &Derivation = Program->High.Solana.Derivations.front();
  EXPECT_EQ(Derivation.DeclaredSeedCount, 1u);
  EXPECT_TRUE(Derivation.Seeds.empty());
  EXPECT_FALSE(Derivation.complete());
}

TEST(SBFSolanaRecovery, ForgetsTheFrameAcrossAWriteOfUnprovenLength) {
  // A sysvar is written through a pointer with no length in the signature, so
  // everything at or above that address stops being described. Aiming it at
  // the descriptor is what makes the difference from the read-only case
  // visible.
  constexpr llvm::StringLiteral Seed("vault");
  RodataBuilder Rodata;
  const va_t SeedText = Rodata.appendText(Seed);

  const std::array Interruption{
      encode(Opcode::MOV64_REG, kFirstArgumentRegister, kFramePointerRegister),
      encode(Opcode::ADD64_IMM, kFirstArgumentRegister, 0, 0, -64),
      encode(Opcode::CALL_IMM, 0, 0, 0,
             static_cast<int32_t>(hashSymbolName("sol_get_clock_sysvar")))};
  auto Program = analyze(makeImage(
      deriveAcross(SeedText, Seed.size(), -64, Interruption), Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Solana.Derivations.size(), 1u);
  EXPECT_TRUE(Program->High.Solana.Derivations.front().Seeds.empty());
}

TEST(SBFSolanaRecovery, FollowsACopiedPayloadIntoTheInvocationThatSendsIt) {
  // The instruction a program invokes is rarely a constant in its image: it
  // assembles the structure in its frame and copies the payload into place.
  // Recovering the operation therefore means following sol_memcpy_ across the
  // block boundary the call itself introduces. Reading only mapped data would
  // report an invocation of the token program with no operation at all.
  const KnownAddressInfo *Token = getKnownAddressInfo(KnownAddress::SplToken);
  ASSERT_NE(Token, nullptr);

  RodataBuilder Rodata;
  const std::vector<uint8_t> Payload{12, 64, 0, 0, 0, 0, 0, 0, 0, 6};
  const va_t PayloadText = Rodata.append(Payload);
  const va_t TokenAddress = Rodata.appendKey(Token->Key);

  constexpr int16_t Instruction = -256;
  constexpr int16_t Buffer = -128;
  constexpr uint8_t Scratch = 6;
  const CPIABIInfo *ABI = findCPIABI(Syscall::InvokeSignedC);
  ASSERT_NE(ABI, nullptr);

  std::vector<EncodedInstruction> Text;
  const auto Append = [&](std::vector<EncodedInstruction> Slots) {
    Text.insert(Text.end(), Slots.begin(), Slots.end());
  };
  const auto FrameAddress = [&](uint8_t Register, int16_t Offset) {
    Text.push_back(
        encode(Opcode::MOV64_REG, Register, kFramePointerRegister));
    Text.push_back(encode(Opcode::ADD64_IMM, Register, 0, 0, Offset));
  };
  const auto StoreField = [&](CPIField Field) {
    Text.push_back(encode(
        Opcode::ST_DW_REG, kFramePointerRegister, Scratch,
        static_cast<int16_t>(Instruction +
                             static_cast<int64_t>(ABI->field(Field).Offset))));
  };

  // sol_memcpy_(frame + Buffer, PayloadText, Payload.size())
  FrameAddress(argumentRegister(SyscallArgument::Arg1), Buffer);
  Append(loadImm64(argumentRegister(SyscallArgument::Arg2), PayloadText));
  Text.push_back(encode(Opcode::MOV64_IMM,
                        argumentRegister(SyscallArgument::Arg3), 0, 0,
                        static_cast<int32_t>(Payload.size())));
  Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                        static_cast<int32_t>(hashSymbolName("sol_memcpy_"))));

  Append(loadImm64(Scratch, TokenAddress));
  StoreField(CPIField::ProgramId);
  Text.push_back(encode(Opcode::MOV64_IMM, Scratch, 0, 0, 0));
  StoreField(CPIField::AccountArray);
  Text.push_back(encode(Opcode::MOV64_IMM, Scratch, 0, 0, 1));
  StoreField(CPIField::AccountCount);
  FrameAddress(Scratch, Buffer);
  StoreField(CPIField::Data);
  Text.push_back(encode(Opcode::MOV64_IMM, Scratch, 0, 0,
                        static_cast<int32_t>(Payload.size())));
  StoreField(CPIField::DataLength);

  FrameAddress(argumentRegister(InvokeArgument::Instruction), Instruction);
  Text.push_back(
      encode(Opcode::CALL_IMM, 0, 0, 0,
             static_cast<int32_t>(hashSymbolName("sol_invoke_signed_c"))));
  Text.push_back(encode(Opcode::EXIT));

  auto Program = analyze(makeImage(Text, Rodata.bytes()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const SolanaModel &Model = Program->High.Solana;
  ASSERT_EQ(Model.CPISites.size(), 1u);
  const CPISite &Site = Model.CPISites.front();
  ASSERT_TRUE(Site.ProgramId.has_value());
  EXPECT_EQ(*Site.ProgramId, Token->Key);
  EXPECT_EQ(Site.AccountCount, 1u);
  EXPECT_EQ(Site.DataLength, Payload.size());
  ASSERT_NE(Site.Selected, nullptr);
  EXPECT_EQ(Site.Selected->Name, "transfer_checked");
  EXPECT_EQ(Site.Selected->Status, InstructionStatus::Current);
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
