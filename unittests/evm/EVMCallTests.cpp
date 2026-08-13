//===- EVMCallTests.cpp - Outgoing call and precompile recovery ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/runtime/EVMCalls.h"
#include "neverd/evm/analysis/EVMStorageSlots.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

#include <vector>

namespace neverd::evm {
namespace {

llvm::APInt word(llvm::StringRef Hex) {
  return llvm::APInt(kWordBits, Hex, kHexRadix);
}

void append(std::vector<uint8_t> &Code, std::vector<uint8_t> Tail) {
  Code.insert(Code.end(), Tail.begin(), Tail.end());
}

std::vector<uint8_t> pushWord(const llvm::APInt &Value) {
  std::vector<uint8_t> Code{opcodeByte(Opcode::PUSH32)};
  for (unsigned I = kWordBytes; I-- > 0;)
    Code.push_back(static_cast<uint8_t>(
        Value.extractBitsAsZExtValue(kBitsPerByte, I * kBitsPerByte)));
  return Code;
}

std::vector<uint8_t> pushWord(uint64_t Value) {
  return pushWord(llvm::APInt(kWordBits, Value));
}

/// Stores \p Selector left-aligned at memory offset zero, which is the shape a
/// compiler gives the head of an outgoing call's calldata.
std::vector<uint8_t> storesSelector(uint32_t Selector) {
  std::vector<uint8_t> Code =
      pushWord(llvm::APInt(kWordBits, Selector).shl(kWordBits - kSelectorBits));
  append(Code, {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::MSTORE)});
  return Code;
}

/// A call whose operands are pushed in the order the opcode pops them.
///
/// \p Callee supplies the address, \p ArgumentLength how much of memory is
/// handed over, and \p Value the transferred amount for the members of the
/// family that carry one.
std::vector<uint8_t> callsThrough(Opcode Op, std::vector<uint8_t> Callee,
                                  uint64_t ArgumentLength,
                                  std::vector<uint8_t> Value = {}) {
  std::vector<uint8_t> Code{
      opcodeByte(Opcode::PUSH0), // return size
      opcodeByte(Opcode::PUSH0), // return offset
  };
  append(Code, pushWord(ArgumentLength)); // argument size
  Code.push_back(opcodeByte(Opcode::PUSH0)); // argument offset
  if (findCallFamily(Op)->HasValueOperand)
    append(Code, Value.empty() ? std::vector<uint8_t>{opcodeByte(Opcode::PUSH0)}
                               : std::move(Value));
  append(Code, std::move(Callee));
  Code.push_back(opcodeByte(Opcode::GAS));
  Code.push_back(opcodeByte(Op));
  Code.push_back(opcodeByte(Opcode::STOP));
  return Code;
}

std::vector<uint8_t> loadsSlot(const llvm::APInt &Slot) {
  std::vector<uint8_t> Code = pushWord(Slot);
  Code.push_back(opcodeByte(Opcode::SLOAD));
  return Code;
}

llvm::Expected<EVMProgram> analyzeAt(llvm::ArrayRef<uint8_t> Code,
                                     Hardfork Fork) {
  AnalyzeOptions Options;
  Options.Fork = Fork;
  return analyze(Code, Options);
}

/// An address well outside the range the protocol reserves, so that a test
/// about selectors is not also a test about precompiles.
llvm::APInt ordinaryAddress() {
  return word("1122334411223344112233441122334411223344");
}

//===----------------------------------------------------------------------===//
// The tables
//===----------------------------------------------------------------------===//

TEST(EVMCallTable, DescribesEveryCallInstructionConsistently) {
  llvm::Error TableError = validateCallTables();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  // The family is exactly the instructions that call an address. CREATE and
  // CREATE2 run code that has no address yet, so they have no callee to
  // recover and must not appear.
  EXPECT_EQ(callFamilyInfos().size(), 4u);
  EXPECT_EQ(findCallFamily(Opcode::CREATE), nullptr);
  EXPECT_EQ(findCallFamily(Opcode::CREATE2), nullptr);
  EXPECT_EQ(findCallFamily(Opcode::JUMP), nullptr);

  for (const CallFamilyInfo &Info : callFamilyInfos()) {
    SCOPED_TRACE(opcodeName(Info.Op).str());
    EXPECT_EQ(&getCallFamilyInfo(Info.ID), &Info);
    EXPECT_EQ(findCallFamily(Info.Op), &Info);
    EXPECT_EQ(Info.calleeOperand(), kCallCalleeOperand);
    EXPECT_EQ(Info.valueOperand().has_value(), Info.HasValueOperand);
    EXPECT_EQ(Info.argumentsLengthOperand(),
              Info.argumentsOffsetOperand() + 1);
    EXPECT_FALSE(Info.Summary.empty());
  }

  // The value operand is what shifts the argument window, so these two
  // layouts are the whole reason the table exists.
  EXPECT_EQ(getCallFamilyInfo(CallFamily::Call).argumentsOffsetOperand(), 3u);
  EXPECT_EQ(
      getCallFamilyInfo(CallFamily::DelegateCall).argumentsOffsetOperand(), 2u);
  EXPECT_TRUE(getCallFamilyInfo(CallFamily::DelegateCall).Delegates);
  EXPECT_TRUE(getCallFamilyInfo(CallFamily::CallCode).Delegates);
  EXPECT_FALSE(getCallFamilyInfo(CallFamily::Call).Delegates);
  EXPECT_TRUE(getCallFamilyInfo(CallFamily::StaticCall).IsStatic);

  for (const CalleeKindInfo &Info : calleeKindInfos()) {
    EXPECT_EQ(calleeKindName(Info.ID), Info.Name);
    EXPECT_FALSE(Info.Summary.empty());
  }
}

TEST(EVMPrecompileTable, NamesEachReservedAddressOnce) {
  for (const PrecompileInfo &Info : precompileInfos()) {
    SCOPED_TRACE(Info.Name.str());
    EXPECT_EQ(&getPrecompileInfo(Info.ID), &Info);
    EXPECT_EQ(findPrecompile(Info.Address, kNewestKnownHardfork), &Info);
    EXPECT_FALSE(Info.Summary.empty());
    // The name reaches recovered output, so it has to spell an identifier.
    EXPECT_FALSE(Info.Name.empty());
    for (char C : Info.Name)
      EXPECT_TRUE(llvm::isAlnum(C) || C == '_') << Info.Name.str();
  }
}

TEST(EVMPrecompileTable, MatchesThePublishedAddresses) {
  const auto At = [](Precompile ID) { return getPrecompileInfo(ID).Address; };
  EXPECT_EQ(At(Precompile::ECRecover), 0x01u);
  EXPECT_EQ(At(Precompile::SHA256), 0x02u);
  EXPECT_EQ(At(Precompile::RIPEMD160), 0x03u);
  EXPECT_EQ(At(Precompile::Identity), 0x04u);
  EXPECT_EQ(At(Precompile::ModExp), 0x05u);
  EXPECT_EQ(At(Precompile::BN256Pairing), 0x08u);
  EXPECT_EQ(At(Precompile::Blake2F), 0x09u);
  EXPECT_EQ(At(Precompile::KZGPointEvaluation), 0x0au);
  EXPECT_EQ(At(Precompile::BLS12381G1Add), 0x0bu);
  EXPECT_EQ(At(Precompile::BLS12381MapFp2ToG2), 0x11u);
  // The one reserved address that does not fit in a byte.
  EXPECT_EQ(At(Precompile::P256Verify), 0x0100u);
}

TEST(EVMPrecompileTable, ReservesNothingBeforeTheForkThatIntroducedIt) {
  // Calling the address of a precompile a later fork introduces reaches an
  // account with no code, which succeeds and returns nothing. Naming it would
  // report an operation the program provably did not perform.
  EXPECT_EQ(findPrecompile(0x05u, Hardfork::Homestead), nullptr);
  EXPECT_NE(findPrecompile(0x05u, Hardfork::Byzantium), nullptr);
  EXPECT_EQ(findPrecompile(0x0au, Hardfork::Shanghai), nullptr);
  EXPECT_NE(findPrecompile(0x0au, Hardfork::Cancun), nullptr);
  EXPECT_EQ(findPrecompile(0x0bu, Hardfork::Cancun), nullptr);
  EXPECT_NE(findPrecompile(0x0bu, Hardfork::Pectra), nullptr);
  EXPECT_EQ(findPrecompile(0x0100u, Hardfork::Pectra), nullptr);
  EXPECT_NE(findPrecompile(0x0100u, Hardfork::Fusaka), nullptr);

  // Zero is not reserved, and neither is any ordinary contract address that
  // happens to share the low bytes of one.
  EXPECT_EQ(findPrecompile(llvm::APInt(kWordBits, 0)), nullptr);
  EXPECT_EQ(findPrecompile(word("1122334411223344112233441122334411220001")),
            nullptr);
}

//===----------------------------------------------------------------------===//
// What a call site says about its callee
//===----------------------------------------------------------------------===//

TEST(EVMCall, NamesAPrecompileTheForkReserves) {
  // A signature check: store nothing, hand the hash window to address one.
  auto Program = analyzeAt(callsThrough(Opcode::STATICCALL, pushWord(1), 0x80),
                           Hardfork::Cancun);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Calls.size(), 1u);
  const CallFact &Call = Program->High.Calls.front();
  EXPECT_EQ(Call.Op, Opcode::STATICCALL);
  EXPECT_EQ(Call.TargetKind, CalleeKind::Fixed);
  ASSERT_NE(Call.Precompiled, nullptr);
  EXPECT_EQ(Call.Precompiled->ID, Precompile::ECRecover);
  EXPECT_EQ(Call.SuggestedName, "ecrecover");
  // A precompile takes raw words, not a selector, so nothing is claimed about
  // one.
  EXPECT_FALSE(Call.Selector.has_value());
  EXPECT_EQ(Call.Known, nullptr);
  // Only a delegating call runs against this program's storage.
  EXPECT_TRUE(Program->High.Proxies.empty());
}

TEST(EVMCall, LeavesAReservedAddressUnnamedBeforeItsFork) {
  const std::vector<uint8_t> Code =
      callsThrough(Opcode::STATICCALL, pushWord(0x0a), 0x60);

  auto Before = analyzeAt(Code, Hardfork::Shanghai);
  ASSERT_TRUE(static_cast<bool>(Before)) << llvm::toString(Before.takeError());
  ASSERT_EQ(Before->High.Calls.size(), 1u);
  EXPECT_EQ(Before->High.Calls.front().Precompiled, nullptr);
  EXPECT_EQ(Before->High.Calls.front().SuggestedName, "call_unknown");

  auto After = analyzeAt(Code, Hardfork::Cancun);
  ASSERT_TRUE(static_cast<bool>(After)) << llvm::toString(After.takeError());
  ASSERT_EQ(After->High.Calls.size(), 1u);
  ASSERT_NE(After->High.Calls.front().Precompiled, nullptr);
  EXPECT_EQ(After->High.Calls.front().Precompiled->ID,
            Precompile::KZGPointEvaluation);
}

TEST(EVMCall, RecoversTheSignatureSentToAFixedAddress) {
  const KnownSignatureInfo *Transfer = nullptr;
  for (const KnownSignatureInfo &Info : knownSignatureInfos())
    if (Info.Kind == SignatureKind::Function &&
        Info.Signature == "transfer(address,uint256)")
      Transfer = &Info;
  ASSERT_NE(Transfer, nullptr);

  const llvm::APInt Token = ordinaryAddress();
  std::vector<uint8_t> Code = storesSelector(Transfer->Selector);
  // Four selector bytes plus two argument words.
  append(Code, callsThrough(Opcode::CALL, pushWord(Token),
                            kSelectorBytes + 2 * kWordBytes));

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Calls.size(), 1u);
  const CallFact &Call = Program->High.Calls.front();
  EXPECT_EQ(Call.Op, Opcode::CALL);
  EXPECT_EQ(Call.TargetKind, CalleeKind::Fixed);
  ASSERT_TRUE(Call.Target.has_value());
  EXPECT_EQ(*Call.Target, Token);
  ASSERT_TRUE(Call.Selector.has_value());
  EXPECT_EQ(*Call.Selector, Transfer->Selector);
  EXPECT_EQ(Call.Known, Transfer);
  EXPECT_EQ(Call.SuggestedName, "transfer");
  ASSERT_TRUE(Call.Value.has_value());
  EXPECT_TRUE(Call.Value->isZero());

  // Calling a token does not make this program one, so the outgoing signature
  // never joins the standards the program answers to.
  EXPECT_TRUE(Program->High.Standards.empty());
}

TEST(EVMCall, ReadsTheArgumentWindowThroughTheValueOperand) {
  // The same selector store reached through two layouts. A DELEGATECALL has no
  // value operand, so its argument offset sits one place earlier; reading it
  // with CALL's layout would find the gas allowance instead.
  const uint32_t Selector = 0xa9059cbbu;
  for (Opcode Op : {Opcode::CALL, Opcode::CALLCODE, Opcode::DELEGATECALL,
                    Opcode::STATICCALL}) {
    SCOPED_TRACE(opcodeName(Op).str());
    std::vector<uint8_t> Code = storesSelector(Selector);
    append(Code, callsThrough(Op, pushWord(ordinaryAddress()), kSelectorBytes));

    auto Program = analyze(Code);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->High.Calls.size(), 1u);
    const CallFact &Call = Program->High.Calls.front();
    ASSERT_TRUE(Call.Selector.has_value());
    EXPECT_EQ(*Call.Selector, Selector);
    EXPECT_EQ(Call.Value.has_value(), findCallFamily(Op)->HasValueOperand);
  }
}

TEST(EVMCall, ClaimsNoSelectorWhenTheWindowIsTooSmallToHoldOne) {
  // Paying an address hands it no calldata at all, and that address may have
  // no code to run.
  std::vector<uint8_t> Code = storesSelector(0xa9059cbbu);
  append(Code, callsThrough(Opcode::CALL, pushWord(ordinaryAddress()), 0,
                            pushWord(llvm::APInt(kWordBits, 1))));

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Calls.size(), 1u);
  const CallFact &Call = Program->High.Calls.front();
  EXPECT_FALSE(Call.Selector.has_value());
  EXPECT_EQ(Call.Known, nullptr);
  EXPECT_EQ(Call.SuggestedName, "call_unknown");
  ASSERT_TRUE(Call.Value.has_value());
  EXPECT_EQ(*Call.Value, llvm::APInt(kWordBits, 1));
}

TEST(EVMCall, NamesTheSlotAnOrdinaryCallReadsItsCalleeFrom) {
  const KnownSlotInfo &Implementation =
      getKnownSlotInfo(KnownSlot::ERC1967Implementation);
  auto Program = analyze(callsThrough(
      Opcode::STATICCALL, loadsSlot(Implementation.Slot), kSelectorBytes));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Calls.size(), 1u);
  const CallFact &Call = Program->High.Calls.front();
  EXPECT_EQ(Call.TargetKind, CalleeKind::NamedSlot);
  EXPECT_EQ(Call.NamedSlot, &Implementation);
  ASSERT_TRUE(Call.Slot.has_value());
  EXPECT_EQ(*Call.Slot, Implementation.Slot);
  EXPECT_FALSE(Call.Target.has_value());
  EXPECT_EQ(Call.Precompiled, nullptr);
}

TEST(EVMCall, ReportsADelegatingCallAsBothACallAndAProxy) {
  const KnownSlotInfo &Implementation =
      getKnownSlotInfo(KnownSlot::ERC1967Implementation);
  auto Program = analyze(callsThrough(Opcode::DELEGATECALL,
                                      loadsSlot(Implementation.Slot), 0));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Calls.size(), 1u);
  ASSERT_EQ(Program->High.Proxies.size(), 1u);
  EXPECT_EQ(Program->High.Calls.front().PC,
            Program->High.Proxies.front().PC);
  EXPECT_EQ(Program->High.Calls.front().TargetKind,
            Program->High.Proxies.front().Kind);
  // The proxy record is what says the callee's code runs against this
  // program's own storage, which no other member of the family does.
  EXPECT_EQ(Program->High.Proxies.front().Known, &Implementation);
}

TEST(EVMCall, ProvesNothingAboutACalleeItCannotFollow) {
  // A callee taken from the caller is not established by anything in the code.
  std::vector<uint8_t> Callee{opcodeByte(Opcode::CALLER)};
  auto Program = analyze(callsThrough(Opcode::CALL, std::move(Callee), 0));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Calls.size(), 1u);
  const CallFact &Call = Program->High.Calls.front();
  EXPECT_EQ(Call.TargetKind, CalleeKind::Dynamic);
  EXPECT_FALSE(Call.Target.has_value());
  EXPECT_FALSE(Call.Slot.has_value());
  EXPECT_EQ(Call.Precompiled, nullptr);
}

TEST(EVMCall, ReportsRecoveredCallsInTheTextualDump) {
  auto Program = analyzeAt(callsThrough(Opcode::STATICCALL, pushWord(1), 0x80),
                           Hardfork::Cancun);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  const std::string Text = dumpHighIR(Program->High);
  EXPECT_NE(Text.find("call 0x"), std::string::npos) << Text;
  EXPECT_NE(Text.find("STATICCALL"), std::string::npos) << Text;
  EXPECT_NE(Text.find("precompile ecrecover"), std::string::npos) << Text;
}

} // namespace
} // namespace neverd::evm
