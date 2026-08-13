//===- EVMCalls.cpp - Calls into another program ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/runtime/EVMCalls.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Twine.h"

#include <array>
#include <limits>

namespace neverd::evm {
namespace {

llvm::Error callError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("evm: call table: " + Message).str(), llvm::inconvertibleErrorCode());
}

/// The widest address the reserved range can hold, which is what bounds the
/// lookup: a value wider than this is an ordinary contract, and truncating it
/// would report a precompile the program never called.
inline constexpr unsigned kPrecompileAddressBits =
    std::numeric_limits<uint16_t>::digits;

} // namespace

//===----------------------------------------------------------------------===//
// The instructions that call another program
//===----------------------------------------------------------------------===//

llvm::ArrayRef<CallFamilyInfo> callFamilyInfos() {
  static const std::array Table = {
#define EVM_CALL_FAMILY(ID, OPCODE, VALUE_OPERAND, DELEGATES, STATIC, SUMMARY) \
  CallFamilyInfo{CallFamily::ID, Opcode::OPCODE, VALUE_OPERAND,                \
                 DELEGATES,      STATIC,         SUMMARY},
#include "neverd/evm/runtime/EVMCalls.def"
  };
  return Table;
}

const CallFamilyInfo &getCallFamilyInfo(CallFamily ID) {
  return callFamilyInfos()[static_cast<size_t>(ID)];
}

const CallFamilyInfo *findCallFamily(Opcode Op) {
  for (const CallFamilyInfo &Info : callFamilyInfos())
    if (Info.Op == Op)
      return &Info;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Where a callee address came from
//===----------------------------------------------------------------------===//

llvm::ArrayRef<CalleeKindInfo> calleeKindInfos() {
  static const std::array Table = {
#define EVM_CALLEE_KIND(ID, NAME, SUMMARY)                                     \
  CalleeKindInfo{CalleeKind::ID, NAME, SUMMARY},
#include "neverd/evm/runtime/EVMCalls.def"
  };
  return Table;
}

llvm::StringRef calleeKindName(CalleeKind Kind) {
  return calleeKindInfos()[static_cast<size_t>(Kind)].Name;
}

//===----------------------------------------------------------------------===//
// The addresses the protocol reserves for itself
//===----------------------------------------------------------------------===//

llvm::ArrayRef<PrecompileInfo> precompileInfos() {
  static const std::array Table = {
#define EVM_PRECOMPILE(ID, ADDRESS, INTRODUCED, NAME, EIP, SUMMARY)            \
  PrecompileInfo{Precompile::ID, Hardfork::INTRODUCED, (ADDRESS), NAME, EIP,   \
                 SUMMARY},
#include "neverd/evm/runtime/EVMPrecompiles.def"
  };
  return Table;
}

const PrecompileInfo &getPrecompileInfo(Precompile ID) {
  return precompileInfos()[static_cast<size_t>(ID)];
}

const PrecompileInfo *findPrecompile(uint64_t Address, Hardfork Fork) {
  for (const PrecompileInfo &Info : precompileInfos())
    if (Info.Address == Address && hardforkAtLeast(Fork, Info.Introduced))
      return &Info;
  return nullptr;
}

const PrecompileInfo *findPrecompile(const llvm::APInt &Address,
                                     Hardfork Fork) {
  if (Address.getActiveBits() > kPrecompileAddressBits)
    return nullptr;
  return findPrecompile(Address.getZExtValue(), Fork);
}

llvm::Error validateCallTables() {
  for (const CallFamilyInfo &Info : callFamilyInfos()) {
    const std::optional<OpcodeInfo> Assigned = assignedOpcodeInfo(Info.Op);
    if (!Assigned)
      return callError("a family entry names an unassigned opcode");
    if (findCallFamily(Info.Op) != &Info)
      return callError("'" + Assigned->Name + "' is listed twice");
    // The operand layout is derived from one flag, so this is what keeps that
    // derivation honest against the opcode database rather than against a
    // comment.
    if (Assigned->StackPops != Info.operandCount())
      return callError("'" + Assigned->Name + "' takes " +
                       llvm::Twine(static_cast<unsigned>(Assigned->StackPops)) +
                       " operands but its layout describes " +
                       llvm::Twine(static_cast<unsigned>(Info.operandCount())));
    if (Assigned->Effect != EffectKind::ExternalCall)
      return callError("'" + Assigned->Name + "' is not recorded as a call");
    // A call the protocol forbids to write state is exactly the one a
    // recovered function may perform and still be view.
    if (Info.IsStatic != (Assigned->StateAccess == StateAccessKind::Read))
      return callError("'" + Assigned->Name +
                       "' disagrees with the opcode database about whether it "
                       "may write state");
  }

  llvm::DenseSet<llvm::StringRef> Names;
  uint64_t Previous = 0;
  for (const PrecompileInfo &Info : precompileInfos()) {
    if (Info.Name.empty() || Info.Summary.empty())
      return callError("a reserved address is incompletely described");
    if (!Names.insert(Info.Name).second)
      return callError("'" + Info.Name + "' is listed twice");
    if (!isValidHardfork(Info.Introduced))
      return callError("'" + Info.Name + "' names no fork that introduced it");
    // Ascending order is what lets the table be read as the map it describes,
    // and it is what catches an entry inserted under the wrong fork heading.
    if (Info.Address <= Previous)
      return callError("'" + Info.Name + "' is out of address order");
    Previous = Info.Address;
    if (findPrecompile(Info.Address, kNewestKnownHardfork) != &Info)
      return callError("'" + Info.Name +
                       "' is not the single precompile at its address");
    if (findPrecompile(Info.Address, Info.Introduced) != &Info)
      return callError("'" + Info.Name +
                       "' is unreachable at the fork that introduced it");
  }
  return llvm::Error::success();
}

} // namespace neverd::evm
