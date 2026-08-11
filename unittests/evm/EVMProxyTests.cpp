//===- EVMProxyTests.cpp - Known slot and delegation recovery tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/Analyzer.h"
#include "neverd/evm/StorageSlots.h"

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

/// A delegating call whose six operands are pushed in the order the opcode
/// pops them, with \p Target supplying the callee.
std::vector<uint8_t> delegatesTo(std::vector<uint8_t> Target) {
  std::vector<uint8_t> Code{
      opcodeByte(Opcode::PUSH0), // return size
      opcodeByte(Opcode::PUSH0), // return offset
      opcodeByte(Opcode::PUSH0), // argument size
      opcodeByte(Opcode::PUSH0), // argument offset
  };
  append(Code, std::move(Target));
  Code.push_back(opcodeByte(Opcode::GAS));
  Code.push_back(opcodeByte(Opcode::DELEGATECALL));
  Code.push_back(opcodeByte(Opcode::STOP));
  return Code;
}

std::vector<uint8_t> loadsSlot(const llvm::APInt &Slot) {
  std::vector<uint8_t> Code = pushWord(Slot);
  Code.push_back(opcodeByte(Opcode::SLOAD));
  return Code;
}

inline constexpr uint32_t kTestSelector = 0x12345678u;

/// A selector dispatcher whose unmatched path runs \p DefaultPath and whose
/// matched path runs \p Body.
std::vector<uint8_t> dispatcher(std::vector<uint8_t> DefaultPath,
                                std::vector<uint8_t> Body) {
  std::vector<uint8_t> Code = {
      opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::CALLDATALOAD),
      opcodeByte(Opcode::PUSH1),
      kWordBits - kSelectorBits,
      opcodeByte(Opcode::SHR),
      opcodeByte(Opcode::PUSH4),
      static_cast<uint8_t>(kTestSelector >> 24),
      static_cast<uint8_t>(kTestSelector >> 16),
      static_cast<uint8_t>(kTestSelector >> 8),
      static_cast<uint8_t>(kTestSelector),
      opcodeByte(Opcode::EQ),
      opcodeByte(Opcode::PUSH1),
      0,
      opcodeByte(Opcode::JUMPI),
  };
  Code[Code.size() - 2] = static_cast<uint8_t>(Code.size() + DefaultPath.size());
  append(Code, std::move(DefaultPath));
  Code.push_back(opcodeByte(Opcode::JUMPDEST));
  append(Code, std::move(Body));
  return Code;
}

/// A body that writes storage, which is more than rejecting a call requires.
std::vector<uint8_t> writesStorage() {
  return {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
          opcodeByte(Opcode::SSTORE), opcodeByte(Opcode::STOP)};
}

std::vector<uint8_t> reverts() {
  return {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::PUSH0),
          opcodeByte(Opcode::REVERT)};
}

//===----------------------------------------------------------------------===//
// The slot dictionary
//===----------------------------------------------------------------------===//

TEST(EVMKnownSlot, DerivesEachSlotToASingleDistinctNumber) {
  llvm::Error TableError = validateKnownSlotTable();
  ASSERT_FALSE(static_cast<bool>(TableError))
      << llvm::toString(std::move(TableError));

  for (const KnownSlotInfo &Info : knownSlotInfos()) {
    SCOPED_TRACE(Info.Preimage.str());
    EXPECT_EQ(&getKnownSlotInfo(Info.ID), &Info);
    EXPECT_EQ(findKnownSlot(Info.Slot), &Info);
    EXPECT_EQ(Info.Slot.getBitWidth(), kWordBits);
    EXPECT_FALSE(Info.Summary.empty());
    // The name reaches generated source, so it has to spell an identifier.
    EXPECT_FALSE(Info.Name.empty());
    for (char C : Info.Name)
      EXPECT_TRUE(llvm::isAlnum(C) || C == '_') << Info.Name.str();
  }

  for (const SlotDerivationInfo &Info : slotDerivationInfos())
    EXPECT_EQ(slotDerivationName(Info.ID), Info.Name);
}

TEST(EVMKnownSlot, MatchesThePublishedSlotNumbers) {
  // Each of these is the number its specification prints. A preimage that has
  // drifted still derives to something; only comparing against the published
  // number catches it.
  const auto Published = [](KnownSlot ID) {
    return getKnownSlotInfo(ID).Slot;
  };
  EXPECT_EQ(Published(KnownSlot::ERC1967Implementation),
            word("360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d3"
                 "82bbc"));
  EXPECT_EQ(Published(KnownSlot::ERC1967Admin),
            word("b53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5"
                 "d6103"));
  EXPECT_EQ(Published(KnownSlot::ERC1967Beacon),
            word("a3f0ad74e5423aebfd80d3ef4346578335a9a72aeaee59ff6cb3582b351"
                 "33d50"));
  EXPECT_EQ(Published(KnownSlot::ERC1967Rollback),
            word("4910fdfa16fed3260ed0e7147f7cc6da11a60208b5b9406d12a635614ff"
                 "d9143"));
  EXPECT_EQ(Published(KnownSlot::ERC1822Proxiable),
            word("c5f16f0fcc639fa48a6947836d9850f504798523bf8c9a3a87d5876cf62"
                 "2bcf7"));
  EXPECT_EQ(Published(KnownSlot::DiamondStorage),
            word("c8fcad8db84d3cc18b4c41d551ea0ee66dd599cde068d998e57d5e09332"
                 "c131c"));
  EXPECT_EQ(Published(KnownSlot::ZeppelinOSImplementation),
            word("7050c9e0f4ca769c69bd3a8ef740bc37934f8e2c036e5a723fd8ee048ed"
                 "3f8c3"));
  EXPECT_EQ(Published(KnownSlot::OZInitializable),
            word("f0c57e16840df040f15088dc2f81fe391c3923bec73e23a9662efc9c229"
                 "c6a00"));
  EXPECT_EQ(Published(KnownSlot::OZOwnable),
            word("9016d09d72d40fdae2fd8ceac6b6234c7706214fd39c1cd1e609a0528c1"
                 "99300"));

  // An ERC-7201 namespace owns an aligned run of slots, so its base never has
  // a low byte set.
  for (const KnownSlotInfo &Info : knownSlotInfos())
    if (Info.Derivation == SlotDerivation::ERC7201)
      EXPECT_EQ(Info.Slot.extractBitsAsZExtValue(kBitsPerByte, 0), 0u)
          << Info.Preimage.str();
}

TEST(EVMKnownSlot, LeavesCompilerAllocatedSlotsUnnamed) {
  // A compiler numbers a contract's own variables from zero, so the low slots
  // must never resolve to a published name.
  for (uint64_t Slot = 0; Slot < 64; ++Slot)
    EXPECT_EQ(findKnownSlot(llvm::APInt(kWordBits, Slot)), nullptr) << Slot;
  EXPECT_EQ(findKnownSlot(llvm::APInt(kAddressBits, 0)), nullptr);
}

//===----------------------------------------------------------------------===//
// What a delegating call says about its target
//===----------------------------------------------------------------------===//

TEST(EVMProxy, ReadsTheTargetOfAMinimalProxyClone) {
  // The ERC-1167 runtime, byte for byte, delegating to 0x1122..44 and
  // forwarding whatever comes back.
  const std::vector<uint8_t> Code = {
      0x36, 0x3d, 0x3d, 0x37, 0x3d, 0x3d, 0x3d, 0x36, 0x3d, 0x73, 0x11, 0x22,
      0x33, 0x44, 0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44, 0x11, 0x22,
      0x33, 0x44, 0x11, 0x22, 0x33, 0x44, 0x5a, 0xf4, 0x3d, 0x82, 0x80, 0x3e,
      0x90, 0x3d, 0x91, 0x60, 0x2b, 0x57, 0xfd, 0x5b, 0xf3};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Proxies.size(), 1u);
  const ProxyFact &Proxy = Program->High.Proxies.front();
  EXPECT_EQ(Proxy.Kind, CalleeKind::Fixed);
  EXPECT_EQ(Proxy.Op, Opcode::DELEGATECALL);
  ASSERT_TRUE(Proxy.Implementation.has_value());
  EXPECT_EQ(*Proxy.Implementation,
            word("1122334411223344112233441122334411223344"));
  EXPECT_FALSE(Proxy.Slot.has_value());
  EXPECT_EQ(Proxy.Known, nullptr);

  // A clone answers to no selector of its own; everything it does is the
  // fallback.
  EXPECT_TRUE(Program->High.Functions.empty());
  EXPECT_TRUE(Program->High.HasFallback);
}

TEST(EVMProxy, NamesTheSlotAnUpgradeableProxyDelegatesThrough) {
  const KnownSlotInfo &Implementation =
      getKnownSlotInfo(KnownSlot::ERC1967Implementation);
  auto Program = analyze(delegatesTo(loadsSlot(Implementation.Slot)));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  ASSERT_EQ(Program->High.Proxies.size(), 1u);
  const ProxyFact &Proxy = Program->High.Proxies.front();
  EXPECT_EQ(Proxy.Kind, CalleeKind::NamedSlot);
  EXPECT_EQ(Proxy.Known, &Implementation);
  ASSERT_TRUE(Proxy.Slot.has_value());
  EXPECT_EQ(*Proxy.Slot, Implementation.Slot);
  EXPECT_FALSE(Proxy.Implementation.has_value());

  // The read of that slot is named wherever it is reported, and the standard
  // it belongs to is reported even though no selector matched.
  ASSERT_EQ(Program->High.Storage.size(), 1u);
  EXPECT_EQ(Program->High.Storage.front().Known, &Implementation);
  EXPECT_EQ(Program->High.Storage.front().SuggestedName,
            Implementation.Name.str());
  EXPECT_NE(llvm::find(Program->High.Standards, KnownStandard::ERC1967),
            Program->High.Standards.end());
}

TEST(EVMProxy, ReadsThroughTheMaskThatCleansALoadedAddress) {
  const KnownSlotInfo &Beacon = getKnownSlotInfo(KnownSlot::ERC1967Beacon);
  std::vector<uint8_t> Target = loadsSlot(Beacon.Slot);
  append(Target, pushWord(llvm::APInt::getLowBitsSet(kWordBits, kAddressBits)));
  Target.push_back(opcodeByte(Opcode::AND));
  Target.push_back(opcodeByte(Opcode::DUP1));

  auto Program = analyze(delegatesTo(std::move(Target)));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Proxies.size(), 1u);
  EXPECT_EQ(Program->High.Proxies.front().Kind, CalleeKind::NamedSlot);
  EXPECT_EQ(Program->High.Proxies.front().Known, &Beacon);
}

TEST(EVMProxy, SeparatesAnUnnamedSlotFromAnUnprovenTarget) {
  auto Constant = analyze(delegatesTo(loadsSlot(llvm::APInt(kWordBits, 7))));
  ASSERT_TRUE(static_cast<bool>(Constant))
      << llvm::toString(Constant.takeError());
  ASSERT_EQ(Constant->High.Proxies.size(), 1u);
  EXPECT_EQ(Constant->High.Proxies.front().Kind, CalleeKind::ConstantSlot);
  EXPECT_EQ(Constant->High.Proxies.front().Known, nullptr);
  ASSERT_TRUE(Constant->High.Proxies.front().Slot.has_value());
  EXPECT_EQ(*Constant->High.Proxies.front().Slot, llvm::APInt(kWordBits, 7));

  // A diamond routes to a facet through a key it derives, which is neither a
  // fixed address nor a slot anyone published.
  std::vector<uint8_t> Hashed{opcodeByte(Opcode::PUSH0),
                              opcodeByte(Opcode::PUSH0),
                              opcodeByte(Opcode::SHA3),
                              opcodeByte(Opcode::SLOAD)};
  auto Computed = analyze(delegatesTo(std::move(Hashed)));
  ASSERT_TRUE(static_cast<bool>(Computed))
      << llvm::toString(Computed.takeError());
  ASSERT_EQ(Computed->High.Proxies.size(), 1u);
  EXPECT_EQ(Computed->High.Proxies.front().Kind, CalleeKind::ComputedSlot);

  auto Dynamic = analyze(delegatesTo({opcodeByte(Opcode::CALLER)}));
  ASSERT_TRUE(static_cast<bool>(Dynamic))
      << llvm::toString(Dynamic.takeError());
  ASSERT_EQ(Dynamic->High.Proxies.size(), 1u);
  EXPECT_EQ(Dynamic->High.Proxies.front().Kind, CalleeKind::Dynamic);
  EXPECT_FALSE(Dynamic->High.Proxies.front().Slot.has_value());
  EXPECT_FALSE(Dynamic->High.Proxies.front().Implementation.has_value());
}

//===----------------------------------------------------------------------===//
// Whether an unrecognized call reaches anything
//===----------------------------------------------------------------------===//

TEST(EVMFallback, ReportsNoFallbackWhenTheDispatcherOnlyRejects) {
  auto Program = analyze(dispatcher(reverts(), writesStorage()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  // The function behind the selector writes storage, but no unrecognized call
  // can reach it, so its effects say nothing about a fallback.
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_FALSE(Program->High.HasFallback);
}

TEST(EVMFallback, ReportsAFallbackWhenTheUnmatchedPathDoesWork) {
  auto Program = analyze(dispatcher(writesStorage(), reverts()));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 1u);
  EXPECT_TRUE(Program->High.HasFallback);
}

TEST(EVMFallback, TreatsAProgramWithoutADispatcherAsAllFallback) {
  auto Working = analyze(writesStorage());
  ASSERT_TRUE(static_cast<bool>(Working))
      << llvm::toString(Working.takeError());
  EXPECT_TRUE(Working->High.HasFallback);

  auto Rejecting = analyze(reverts());
  ASSERT_TRUE(static_cast<bool>(Rejecting))
      << llvm::toString(Rejecting.takeError());
  EXPECT_FALSE(Rejecting->High.HasFallback);
}

} // namespace
} // namespace neverd::evm
