//===- SBFSolanaPDATests.cpp - Derived address recovery tests -----------===//
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

#include <array>
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
} // namespace
} // namespace neverd::sbf
