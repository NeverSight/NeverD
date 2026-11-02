//===- SBFWordRecoveryTests.cpp - Full-domain binary recovery words -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Real binary words and externally supplied IDL discriminators include both
/// values reserved by scalar DenseMap keys. Recovery must preserve them safely.
///
//===----------------------------------------------------------------------===//

#include "SBFSolanaTestsDetail.h"

#include "neverd/sbf/solana/SBFAnchor.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace neverd::sbf {
namespace {

TEST(SBFSolanaRecovery, ArbitraryReadOnlyWordsAreNotDictionaryEntries) {
  std::array<uint8_t, 80> Bytes;
  Bytes.fill(0xff);
  llvm::support::endian::write64le(Bytes.data() + 16,
                                   std::numeric_limits<uint64_t>::max() - 1);
  auto Program = analyze(test::makeImage({test::encode(Opcode::EXIT)}, Bytes));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  EXPECT_TRUE(Program->High.Solana.Discriminators.empty());
  EXPECT_TRUE(Program->High.Solana.Pubkeys.empty());
}

TEST(SBFSolanaRecovery, SuppliedIDLCanNameEveryDiscriminatorWord) {
  for (const uint64_t Word : {std::numeric_limits<uint64_t>::max(),
                              std::numeric_limits<uint64_t>::max() - 1}) {
    const AnchorDiscriminator Value = AnchorDiscriminator::fromWord(Word);
    AnchorIdl Idl;
    AnchorIdlItem Item;
    Item.Name = "opaque_input";
    Item.Namespace = AnchorNamespace::Instruction;
    Item.Discriminator = Value;
    Idl.Items.push_back(Item);
    AnalyzeOptions Options;
    Options.Idl = &Idl;
    auto Program = analyze(
        test::makeImage({test::encode(Opcode::EXIT)}, Value.Bytes), Options);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->High.Solana.Discriminators.size(), 1u);
    EXPECT_EQ(Program->High.Solana.Discriminators[0].Name, "opaque_input");
    EXPECT_EQ(Program->High.Solana.Discriminators[0].Value, Value);
  }
}

TEST(SBFAnchor, ReservedScalarWordsHaveNoBuiltInName) {
  EXPECT_EQ(findAnchorName(AnchorDiscriminator::fromWord(
                std::numeric_limits<uint64_t>::max())),
            nullptr);
  EXPECT_EQ(findAnchorName(AnchorDiscriminator::fromWord(
                std::numeric_limits<uint64_t>::max() - 1)),
            nullptr);
  const auto Deposit =
      anchorDiscriminator(AnchorNamespace::Instruction, "deposit");
  const auto *Known = findAnchorName(Deposit);
  ASSERT_NE(Known, nullptr);
  EXPECT_EQ(Known->Name, "deposit");
}

TEST(SBFSolanaRecovery, PubkeyPrefixUsesTheFullWordDomainAndFullKeyEquality) {
  for (const uint64_t Prefix : {std::numeric_limits<uint64_t>::max(),
                                std::numeric_limits<uint64_t>::max() - 1}) {
    Pubkey First;
    First.Bytes.fill(0x31);
    llvm::support::endian::write64le(First.Bytes.data(), Prefix);
    Pubkey Second = First;
    Second.Bytes.back() = 0x32;
    test::RodataBuilder Rodata;
    const va_t Left = Rodata.appendKey(First);
    const va_t Right = Rodata.appendKey(Second);
    std::vector<test::EncodedInstruction> Text;
    const auto Append = [&](std::vector<test::EncodedInstruction> Slots) {
      Text.insert(Text.end(), Slots.begin(), Slots.end());
    };
    for (const va_t Address : {Left, Right, Left}) {
      Append(test::loadImm64(2, Address));
      Text.push_back(
          test::encode(Opcode::MOV64_IMM, 3, 0, 0, kPubkeyByteCount));
      Text.push_back(
          test::encode(Opcode::CALL_IMM, 0, 0, 0,
                       static_cast<int32_t>(hashSymbolName("sol_memcmp_"))));
    }
    Text.push_back(test::encode(Opcode::EXIT));
    auto Program = analyze(test::makeImage(Text, Rodata.bytes()));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->High.Solana.Pubkeys.size(), 2u);
    EXPECT_EQ(Program->High.Solana.Pubkeys[0].Key, First);
    EXPECT_EQ(Program->High.Solana.Pubkeys[1].Key, Second);
  }
}

} // namespace
} // namespace neverd::sbf
