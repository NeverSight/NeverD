//===- SBFSolanaCPITests.cpp - Invocation recovery tests ----------------===//
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

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"

#include <ostream>
#include <vector>

namespace neverd::sbf {
namespace {

using test::encode;
using test::EncodedInstruction;
using test::filledKey;
using test::loadImm64;
using test::makeImage;
using test::RodataBuilder;

//===----------------------------------------------------------------------===//
// Invocation recovery
//===----------------------------------------------------------------------===//

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
} // namespace
} // namespace neverd::sbf
