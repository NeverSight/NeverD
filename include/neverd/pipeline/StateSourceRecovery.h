//===- StateSourceRecovery.h - Explicit state ABI source contracts -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover ordinary source calls from an explicitly isolated register bank.

#ifndef NEVERD_PIPELINE_STATESOURCERECOVERY_H
#define NEVERD_PIPELINE_STATESOURCERECOVERY_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace neverd {
struct StateSourceSlot {
  unsigned Offset = 0;
  unsigned Bytes = 8;
};
struct StateSourceWrite {
  StateSourceSlot Slot;
  uint64_t Value = 0;
};
enum class StateSourceType {
  Void,
  U32,
  U64,
  I32,
  I64,
  Pointer,
  U8,
  I8,
  U16,
  I16
};
enum class StateSourceVoidResult { Zero, Unspecified };
struct StateSourcePointerSource {
  unsigned Image = 0;
  unsigned Offset = 0;
};
struct StateSourceImport {
  uint64_t Selector = 0;
  std::string Name;
  std::vector<StateSourceType> Parameters;
  StateSourceType Result = StateSourceType::U64;
  /// Candidate identity for a pointer loaded from this image slot. This is
  /// never a constant-memory assertion: the captured target must also equal
  /// the imported function's address before a direct call is emitted.
  std::optional<StateSourcePointerSource> PointerSource;
  /// When present, Parameters describes every supplied argument and this
  /// positive prefix length describes the variadic declaration's fixed part.
  /// Trailing scalar types include the C default argument promotions.
  std::optional<unsigned> VariadicFixedParameters;
  /// Zero models gateways that explicitly clear a void result. Unspecified
  /// models capture of physical return-register bits: recovery must prove
  /// every destination byte overwritten before observation on normal return.
  StateSourceVoidResult VoidResult = StateSourceVoidResult::Zero;
  /// Explicit bit-pattern defaults for a suffix of Parameters. Missing
  /// operands use these values; supplied operands retain their captured values.
  /// A pointer default must be null. Variadic imports cannot have defaults.
  std::vector<uint64_t> Defaults;
  /// Instead of a named import, call this captured pointer operand with the
  /// other Parameters in order. Name must be empty. The target is required,
  /// not variadic or defaulted, and null retains the original gateway.
  /// This asserts the target's callable ABI; it supplies no symbol identity.
  std::optional<unsigned> TargetArgument;
  /// Prepend the gateway's opaque second pointer operand to the native call.
  /// It is captured unchanged, including null. Parameters, defaults and bank
  /// argument indices still describe only the explicitly supplied operands.
  /// The producer owns the receiver ABI and its observable context effects.
  bool PassContext = false;
  /// A void gateway may copy this required operand's captured bank bits to
  /// its result channel. Capture precedes fixed writes and native parameter
  /// conversion; this is independent of physical native return registers.
  std::optional<unsigned> VoidResultArgument;
};
struct StateSourceImage {
  StateSourceSlot Base;
  /// A readable guest extent, disjoint from the bank. Bytes are only a
  /// specialization candidate: their current contents are checked at runtime.
  bool Readable = false;
  std::vector<uint8_t> Bytes;
};
struct StateSourceString {
  unsigned Argument = 0;
  unsigned Image = 0;
  /// The gateway passes a NUL-terminated string by value. The imported API
  /// only reads its content, neither retaining its address nor observing its
  /// identity. Scratch allocation
  /// and normalization have no other observable effects under this contract.
  bool ByValue = false;
  /// When both slots are present, a nonzero key and matching mode select
  /// bytewise XOR normalization, including the encoded NUL terminator.
  std::optional<StateSourceSlot> Mode;
  std::optional<StateSourceSlot> XorKey;
  uint64_t EncodedMode = 1;
};
struct StateSourceArgumentExclusion {
  unsigned Argument = 0;
  uint64_t Value = 0;
};
struct StateSourceFallbackWrites {
  /// An additional producer assertion, including unknown targets/arguments
  /// and nonzero statuses: only the fixed Writes and result channel can change
  /// bank bytes. Reads and guest/context effects remain unrestricted. Without
  /// this assertion fallback calls may change every byte. Omit ResultBytes to
  /// use the captured result-width descriptor; otherwise supply a fixed width.
  std::optional<unsigned> ResultBytes;
};
struct StateSourceGateway {
  std::string Name;
  std::optional<StateSourceSlot> Selector;
  /// Mutually exclusive with Selector. Imports supply PointerSource instead
  /// of numeric selectors; unknown provenance or addresses keep the gateway.
  std::optional<StateSourceSlot> FunctionPointer;
  std::optional<StateSourceSlot> ArgumentCount;
  std::vector<StateSourceSlot> Arguments;
  StateSourceSlot ResultOffset;
  StateSourceSlot ResultBytes;
  std::vector<StateSourceWrite> Writes;
  std::vector<StateSourceImport> Imports;
  /// These argument slots contain offsets of eight-byte values in the bank.
  /// Only statically bounded offsets are dereferenced, before fixed writes.
  std::vector<unsigned> IndirectArguments;
  std::vector<StateSourceString> Strings;
  /// Values with additional gateway behavior retain the original call.
  std::vector<StateSourceArgumentExclusion> ExcludedArguments;
  std::optional<StateSourceFallbackWrites> FallbackWrites;
};
struct StateSourceContract {
  unsigned StateBytes = 0;
  /// Required assertions, supplied separately from path observations:
  /// guest/context memory cannot alias the register bank; calls cannot retain
  /// bank addresses; declared gateways return zero status after normal return
  /// and modify only the declared result channel and fixed writes in the bank.
  /// Selector, arguments and result-channel descriptors are captured on entry,
  /// before the fixed writes and imported operation.
  /// Imported callees may freely access guest memory and may fail to return.
  bool IsolatedBank = false;
  bool ZeroStatusGateways = false;
  std::vector<StateSourceGateway> Gateways;
  std::vector<StateSourceImage> Images;
  /// Opt in to local bank scalarization. The entire declared extent is
  /// readable/writable for the invocation and exclusively owned except at
  /// synchronous calls receiving the bank. No asynchronous observer or
  /// nonlocal re-entry may inspect it. All bytes are preserved at boundaries;
  /// this is not an assertion that scratch registers are dead on return.
  bool LocalizeBank = false;
};
struct StateSourceRecovery {
  StateSourceContract Contract;
  std::vector<std::string> Probes;
  unsigned Functions = 0;
  unsigned RewrittenCalls = 0;
  unsigned GuardedCalls = 0;
  unsigned UnresolvedCalls = 0;
  unsigned LocalizedFunctions = 0;
  unsigned ForwardedStateLoads = 0;
  unsigned RedundantStateStores = 0;
  unsigned BoundedStateCalls = 0;
};

/// Validate the entire contract/module before mutation. Scoped alias facts
/// keep the public state ABI and unresolved call boundaries intact. Temporary
/// probes retain ordinary call effects until finishStateSourceRecovery.
llvm::Expected<StateSourceRecovery>
prepareStateSourceRecovery(llvm::Module &Module,
                           const StateSourceContract &Contract);

/// Consume constants proven by the shared LLVM optimizer at each probe.
/// Argument counts and candidate strings get runtime guards and the original
/// fallback. Indirect arguments require statically bounded bank offsets.
/// Restore unresolved gateways. No observation name or sampled value is used.
llvm::Error finishStateSourceRecovery(llvm::Module &Module,
                                      StateSourceRecovery &Recovery);
} // namespace neverd
#endif
