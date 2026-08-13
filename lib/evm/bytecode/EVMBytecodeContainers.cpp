//===- EVMBytecodeContainers.cpp - EVM container and image metadata ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMBytecodeDetail.h"

#include "neverd/evm/bytecode/EVMOpcodes.h"

#include "llvm/ADT/StringExtras.h"

#include <array>
#include <optional>
#include <string>

namespace neverd::evm {

llvm::ArrayRef<BytecodeSourceInfo> bytecodeSourceInfos() {
  static const std::array Table = {
#define EVM_BYTECODE_SOURCE(ID, SPELLING, SUMMARY)                             \
  BytecodeSourceInfo{BytecodeSourceKind::ID, SPELLING, SUMMARY},
#include "neverd/evm/bytecode/EVMBytecodeContainers.def"
  };
  return Table;
}

llvm::StringRef bytecodeSourceName(BytecodeSourceKind Source) {
  return bytecodeSourceInfos()[static_cast<size_t>(Source)].Name;
}

llvm::ArrayRef<BytecodeContainerInfo> bytecodeContainerInfos() {
  static const std::array Table = {
#define EVM_BYTECODE_CONTAINER(ID, SPELLING, MARKER, MARKER_BYTES, EXACT_SIZE, \
                               DISPOSITION, EIP, SUMMARY)                      \
  BytecodeContainerInfo{BytecodeContainer::ID,                                 \
                        SPELLING,                                              \
                        (MARKER),                                              \
                        (MARKER_BYTES),                                        \
                        (EXACT_SIZE),                                          \
                        ContainerDisposition::DISPOSITION,                     \
                        EIP,                                                   \
                        SUMMARY},
#include "neverd/evm/bytecode/EVMBytecodeContainers.def"
  };
  return Table;
}

// An indicator is a marker followed by an address and nothing else, which is
// what makes its size a complete identity check rather than a lower bound.
#define EVM_BYTECODE_CONTAINER(ID, SPELLING, MARKER, MARKER_BYTES, EXACT_SIZE, \
                               DISPOSITION, EIP, SUMMARY)                      \
  static_assert(ContainerDisposition::DISPOSITION !=                           \
                        ContainerDisposition::RequiresDelegateTarget ||        \
                    (EXACT_SIZE) == (MARKER_BYTES) + kAddressBytes,            \
                "a delegation indicator is a marker followed by an address");
#include "neverd/evm/bytecode/EVMBytecodeContainers.def"

const BytecodeContainerInfo &
getBytecodeContainerInfo(BytecodeContainer Container) {
  return bytecodeContainerInfos()[static_cast<size_t>(Container)];
}

llvm::StringRef bytecodeContainerName(BytecodeContainer Container) {
  return getBytecodeContainerInfo(Container).Name;
}

std::optional<Hardfork>
bytecodeContainerActivation(BytecodeContainer Container) {
  switch (Container) {
#define EVM_BYTECODE_CONTAINER_ACTIVATION(ID, HARDFORK)                        \
  case BytecodeContainer::ID:                                                  \
    return Hardfork::HARDFORK;
#include "neverd/evm/bytecode/EVMBytecodeContainers.def"
  default:
    return std::nullopt;
  }
}

BytecodeContainer classifyBytecodeContainer(llvm::ArrayRef<uint8_t> Code) {
  for (const BytecodeContainerInfo &Info : bytecodeContainerInfos()) {
    if (Info.MarkerBytes == 0 || Code.size() < Info.MarkerBytes)
      continue;
    uint32_t Leading = 0;
    for (uint8_t I = 0; I < Info.MarkerBytes; ++I)
      Leading = (Leading << kBitsPerByte) | Code[I];
    if (Leading != Info.Marker)
      continue;
    // A marker at any other size is malformed input rather than a variant of
    // the container. It stays instructions so the decoder can say which byte
    // it could not read, instead of this reporting a container that is not
    // there.
    if (Info.ExactSize != 0 && Code.size() != Info.ExactSize)
      continue;
    return Info.ID;
  }
  return BytecodeContainer::Legacy;
}

llvm::ArrayRef<uint8_t> delegationTarget(llvm::ArrayRef<uint8_t> Code,
                                         BytecodeContainer Container) {
  const BytecodeContainerInfo &Info = getBytecodeContainerInfo(Container);
  if (Info.Disposition != ContainerDisposition::RequiresDelegateTarget ||
      Code.size() != Info.ExactSize)
    return {};
  return Code.drop_front(Info.MarkerBytes);
}

llvm::ArrayRef<uint8_t> LoadedBytecode::delegateTarget() const {
  return delegationTarget(Original, Container);
}

ContainerDisposition LoadedBytecode::disposition() const {
  return getBytecodeContainerInfo(Container).Disposition;
}

llvm::Error checkDecodable(const LoadedBytecode &Loaded,
                           llvm::StringRef SourceName) {
  const BytecodeContainerInfo &Info =
      getBytecodeContainerInfo(Loaded.Container);
  const std::string Named = (Info.Name + " container (" + Info.EIP + ")").str();

  switch (Info.Disposition) {
  case ContainerDisposition::Decodable:
    return llvm::Error::success();
  case ContainerDisposition::RequiresDelegateTarget: {
    const std::optional<Hardfork> Activated =
        bytecodeContainerActivation(Loaded.Container);
    // Before activation the protocol would not let an account hold these
    // bytes, so naming a delegation would be describing a state the chain
    // could not have been in.
    if (Activated && !hardforkAtLeast(Loaded.Fork, *Activated))
      return detail::inputError(SourceName, Named + " is not assigned until " +
                                                hardforkName(*Activated) +
                                                ", and the analyzed fork is " +
                                                hardforkName(Loaded.Fork));
    return detail::inputError(
        SourceName,
        Named + ": the code that runs here belongs to 0x" +
            llvm::toHex(Loaded.delegateTarget(), /*LowerCase=*/true) +
            ", whose runtime code was not supplied");
  }
  case ContainerDisposition::Unrecognized:
    return detail::inputError(SourceName, Named + " is " + Info.Summary);
  }
  return detail::inputError(SourceName, Named + " has no disposition");
}

ImageMetadata describeBytecode(const LoadedBytecode &Loaded) {
  ImageMetadata Described;
  Described.Source = Loaded.Source;
  Described.Container = Loaded.Container;
  Described.Fork = Loaded.Fork;
  Described.SourceIsRuntime = Loaded.SourceIsRuntime;
  Described.RuntimeExtracted = Loaded.RuntimeExtracted;
  Described.MetadataStripped = Loaded.MetadataStripped;
  const llvm::ArrayRef<uint8_t> Target = Loaded.delegateTarget();
  Described.DelegateTarget.assign(Target.begin(), Target.end());
  Described.InputMetadata = Loaded.InputMetadata;
  Described.RuntimeMetadata = Loaded.RuntimeMetadata;
  return Described;
}

} // namespace neverd::evm
