//===- EVMImageMetadata.h - Loader-to-frontend EVM metadata ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares what kind of thing a blob of EVM code is, and the typed record a
/// loader hands the frontend after deciding.
///
/// An EVM input arrives with no header, so everything a loader learns about it
/// it learns by reading the bytes: whether they are instructions at all, which
/// compiler emitted them, and how much of what was supplied was removed to get
/// to the part that executes. Recording that here is what keeps the frontend
/// from reading the bytes a second time and reaching a different answer.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_EVMIMAGEMETADATA_H
#define NEVERD_EVM_EVMIMAGEMETADATA_H

#include "neverd/evm/Metadata.h"
#include "neverd/evm/Opcodes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::evm {

/// What analysis can do with a container.
enum class ContainerDisposition : uint8_t {
  /// The bytes are instructions.
  Decodable,
  /// The bytes name the account whose instructions run here.
  RequiresDelegateTarget,
  /// The bytes are a container this analysis does not read.
  Unrecognized,
};

enum class BytecodeContainer : uint8_t {
#define EVM_BYTECODE_CONTAINER(ID, SPELLING, MARKER, MARKER_BYTES, EXACT_SIZE, \
                               DISPOSITION, EIP, SUMMARY)                      \
  ID,
#include "neverd/evm/EVMBytecodeContainers.def"
};

struct BytecodeContainerInfo {
  BytecodeContainer ID;
  llvm::StringLiteral Name;
  /// The leading bytes that identify the container, big-endian. Meaningless
  /// when MarkerBytes is zero, which marks the fallback.
  uint32_t Marker;
  uint8_t MarkerBytes;
  /// The only size the container is encoded at, or zero when it has none.
  uint8_t ExactSize;
  ContainerDisposition Disposition;
  /// The proposal that assigned the marker, empty for the fallback.
  llvm::StringLiteral EIP;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<BytecodeContainerInfo> bytecodeContainerInfos();
const BytecodeContainerInfo &
getBytecodeContainerInfo(BytecodeContainer Container);
llvm::StringRef bytecodeContainerName(BytecodeContainer Container);

/// The fork at which the protocol gives \p Container's marker its meaning, and
/// nothing when no fork has ever been scheduled to.
std::optional<Hardfork>
bytecodeContainerActivation(BytecodeContainer Container);

/// Classify \p Code by the container it is, which is a question about the
/// bytes alone and not about the fork.
BytecodeContainer classifyBytecodeContainer(llvm::ArrayRef<uint8_t> Code);

/// The account a delegation indicator names, big-endian and twenty bytes long.
/// Empty for every other container. Whether the fork under analysis recognizes
/// the indicator is a separate question from what it says.
llvm::ArrayRef<uint8_t> delegationTarget(llvm::ArrayRef<uint8_t> Code,
                                         BytecodeContainer Container);

/// How the input spelled its bytes.
enum class BytecodeSourceKind : uint8_t {
#define EVM_BYTECODE_SOURCE(ID, SPELLING, SUMMARY) ID,
#include "neverd/evm/EVMBytecodeContainers.def"
};

struct BytecodeSourceInfo {
  BytecodeSourceKind ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<BytecodeSourceInfo> bytecodeSourceInfos();
llvm::StringRef bytecodeSourceName(BytecodeSourceKind Source);

/// What a loader decided about an EVM input.
///
/// The bytes themselves live in the image: the container in its raw bytes and
/// the executable remainder in its code segment. What is here is everything
/// that had to be worked out rather than copied, and would otherwise be
/// discarded at the loader boundary and silently re-derived — differently —
/// further along.
struct ImageMetadata {
  BytecodeSourceKind Source = BytecodeSourceKind::Raw;
  BytecodeContainer Container = BytecodeContainer::Legacy;
  /// The fork the loader normalized under. A session analyzing a different
  /// fork has to normalize again rather than inherit this result.
  Hardfork Fork = Hardfork::Latest;
  bool SourceIsRuntime = false;
  bool RuntimeExtracted = false;
  bool MetadataStripped = false;
  /// The account a delegation indicator names, empty for every other input.
  std::vector<uint8_t> DelegateTarget;
  /// The trailer on the container, and the trailer on the code that remains
  /// after unwrapping it. The two compilers put theirs in different places, so
  /// which one is set says as much as what it contains.
  std::optional<ContractMetadata> InputMetadata;
  std::optional<ContractMetadata> RuntimeMetadata;
};

} // namespace neverd::evm

#endif // NEVERD_EVM_EVMIMAGEMETADATA_H
