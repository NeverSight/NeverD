//===- EVMCalls.h - Calls into another program ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines what a call out of the program under analysis is made of: the
/// operand layout of the four instructions that perform one, the lattice of
/// places a callee address can come from, and the dictionary of addresses the
/// protocol answers at itself.
///
/// A decompiled contract is only half a contract without this. Almost every
/// deployed program is a participant in a system rather than a whole one: it
/// moves a token it does not implement, delegates to an implementation it does
/// not contain, and checks a signature against native code that is not
/// deployed anywhere. Recovering which selector it sends where is what turns a
/// wall of memory stores into a named call to a named interface.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_RUNTIME_EVMCALLS_H
#define NEVERD_EVM_RUNTIME_EVMCALLS_H

#include "neverd/evm/bytecode/EVMOpcodes.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::evm {

//===----------------------------------------------------------------------===//
// The instructions that call another program
//===----------------------------------------------------------------------===//

/// Operand positions shared by the whole family, counted from the top of the
/// operand stack, which is the order operands are recorded in.
inline constexpr size_t kCallGasOperand = 0;
inline constexpr size_t kCallCalleeOperand = 1;
/// The operands that follow the argument window: where the returned data is
/// written, and how much of it is kept.
inline constexpr size_t kCallReturnWindowOperands = 2;

enum class CallFamily : uint8_t {
#define EVM_CALL_FAMILY(ID, OPCODE, VALUE_OPERAND, DELEGATES, STATIC, SUMMARY) \
  ID,
#include "neverd/evm/runtime/EVMCalls.def"
};

/// The operand layout and the semantics of one member of the call family.
struct CallFamilyInfo {
  CallFamily ID;
  Opcode Op;
  /// True when a value operand sits between the callee and the argument
  /// window. This is what shifts every later operand by one.
  bool HasValueOperand;
  /// True when the callee's code runs against this program's storage, which is
  /// what makes the callee part of this program rather than a program it talks
  /// to.
  bool Delegates;
  /// True when the callee is forbidden to write state, which is what a
  /// compiler emits for a call it declared view.
  bool IsStatic;
  llvm::StringLiteral Summary;

  /// Where the callee sits, which every member of the family agrees on.
  [[nodiscard]] constexpr size_t calleeOperand() const {
    return kCallCalleeOperand;
  }
  /// Where the transferred value sits, for the members that carry one.
  [[nodiscard]] constexpr std::optional<size_t> valueOperand() const {
    if (!HasValueOperand)
      return std::nullopt;
    return kCallCalleeOperand + 1;
  }
  /// Where the calldata handed to the callee starts.
  [[nodiscard]] constexpr size_t argumentsOffsetOperand() const {
    return kCallCalleeOperand + (HasValueOperand ? 2 : 1);
  }
  [[nodiscard]] constexpr size_t argumentsLengthOperand() const {
    return argumentsOffsetOperand() + 1;
  }
  /// How many operands the instruction takes, which the table checks against
  /// the opcode database so the two cannot drift apart.
  [[nodiscard]] constexpr size_t operandCount() const {
    return argumentsLengthOperand() + 1 + kCallReturnWindowOperands;
  }
};

llvm::ArrayRef<CallFamilyInfo> callFamilyInfos();
const CallFamilyInfo &getCallFamilyInfo(CallFamily ID);
/// The family entry for \p Op, or null when \p Op does not call another
/// program. CREATE and CREATE2 deliberately do not appear: they run code that
/// is not yet at an address, so there is no callee to recover.
const CallFamilyInfo *findCallFamily(Opcode Op);

//===----------------------------------------------------------------------===//
// Where a callee address came from
//===----------------------------------------------------------------------===//

enum class CalleeKind : uint8_t {
#define EVM_CALLEE_KIND(ID, NAME, SUMMARY) ID,
#include "neverd/evm/runtime/EVMCalls.def"
};

struct CalleeKindInfo {
  CalleeKind ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<CalleeKindInfo> calleeKindInfos();
llvm::StringRef calleeKindName(CalleeKind Kind);

//===----------------------------------------------------------------------===//
// The addresses the protocol reserves for itself
//===----------------------------------------------------------------------===//

enum class Precompile : uint8_t {
#define EVM_PRECOMPILE(ID, ADDRESS, INTRODUCED, NAME, EIP, SUMMARY) ID,
#include "neverd/evm/runtime/EVMPrecompiles.def"
};

struct PrecompileInfo {
  Precompile ID;
  /// The first fork at which the protocol answers at this address. Before it,
  /// the same call reaches an empty account and returns nothing.
  Hardfork Introduced;
  /// The reserved address. This is a whole address rather than a byte because
  /// EIP-7951 reserved one that does not fit in a byte.
  uint16_t Address;
  llvm::StringLiteral Name;
  /// The proposal that reserved the address, empty for the four that predate
  /// the proposal process.
  llvm::StringLiteral EIP;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<PrecompileInfo> precompileInfos();
const PrecompileInfo &getPrecompileInfo(Precompile ID);

/// The precompile \p Address names at \p Fork, or null when the protocol
/// reserves nothing there yet.
///
/// The fork gate is not a formality. Calling the address of a precompile a
/// later fork introduces is an ordinary call to an account with no code, which
/// succeeds and returns nothing, so naming it would report an operation the
/// program provably did not perform.
const PrecompileInfo *findPrecompile(const llvm::APInt &Address,
                                     Hardfork Fork = Hardfork::Latest);
const PrecompileInfo *findPrecompile(uint64_t Address,
                                     Hardfork Fork = Hardfork::Latest);

/// Report a call-family entry whose operand layout disagrees with the opcode
/// database, or two precompiles claiming one address or one name.
llvm::Error validateCallTables();

} // namespace neverd::evm

#endif // NEVERD_EVM_RUNTIME_EVMCALLS_H
