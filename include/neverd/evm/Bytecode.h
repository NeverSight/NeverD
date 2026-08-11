//===- Bytecode.h - EVM bytecode input normalization ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares normalization of raw, hexadecimal, and Solidity compiler-artifact
/// inputs into executable EVM bytecode.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_BYTECODE_H
#define NEVERD_EVM_BYTECODE_H

#include "neverd/evm/Metadata.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd::evm {

enum class BytecodeSourceKind : uint8_t { Raw, Hex, Artifact };

enum class BytecodeInputFormat : uint8_t { Auto, Raw, Hex, Artifact };

/// Selects the accepted container and optional runtime normalization steps.
struct BytecodeLoadOptions {
  BytecodeInputFormat Format = BytecodeInputFormat::Auto;
  bool ExtractRuntime = true;
  bool StripMetadata = true;
  /// Optional contract selector for multi-contract standard-json artifacts.
  /// Accepts either "Contract" or "path/File.sol:Contract".
  std::string ArtifactContract;
};

/// Owns normalized executable bytes and records transformations that occurred.
struct LoadedBytecode {
  std::vector<uint8_t> Code;
  BytecodeSourceKind Source = BytecodeSourceKind::Raw;
  bool RuntimeExtracted = false;
  bool MetadataStripped = false;
  size_t OriginalSize = 0;
  /// The compiler trailer, when the input carried one. Set whether or not the
  /// trailer was removed, because what it says about the build is a fact about
  /// the input rather than a step of normalization. Its offset is relative to
  /// the bytes normalization was given, which is the runtime code once a
  /// deployment container has been unwrapped.
  std::optional<ContractMetadata> Metadata;
};

/// Normalize raw binary, hexadecimal text, or a compiler artifact into EVM
/// runtime bytecode. SourceName is used for diagnostics and input hints only.
llvm::Expected<LoadedBytecode>
decodeBytecodeInput(llvm::StringRef Content, llvm::StringRef SourceName = {},
                    const BytecodeLoadOptions &Options = {});

llvm::Expected<LoadedBytecode>
loadBytecodeFile(const std::filesystem::path &Path,
                 const BytecodeLoadOptions &Options = {});

/// Return true only for files that can be fully validated as EVM input.
bool looksLikeEVMInput(const std::filesystem::path &Path);

/// Return true for explicit EVM/container filename extensions. This does not
/// validate file contents and lets callers preserve detailed loader errors.
bool hasEVMFileExtension(const std::filesystem::path &Path);

} // namespace neverd::evm

#endif // NEVERD_EVM_BYTECODE_H
