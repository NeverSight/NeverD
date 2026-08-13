//===- EVMBytecode.h - EVM bytecode input normalization -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares normalization of raw, hexadecimal, and Solidity compiler-artifact
/// inputs into executable EVM bytecode.
///
/// Normalization answers two questions before a decoder ever runs: whether
/// these bytes are instructions at all, and which of them are. Both answers
/// depend on the fork being analyzed, so the fork is an input here rather than
/// a later concern.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_BYTECODE_EVMBYTECODE_H
#define NEVERD_EVM_BYTECODE_EVMBYTECODE_H

#include "neverd/evm/EVMImageMetadata.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd::evm {

enum class BytecodeInputFormat : uint8_t { Auto, Raw, Hex, Artifact };

/// Selects the accepted container and optional runtime normalization steps.
struct BytecodeLoadOptions {
  BytecodeInputFormat Format = BytecodeInputFormat::Auto;
  /// The fork whose instruction encoding decides where a constructor's
  /// instructions end. Walking a constructor under the wrong fork misreads a
  /// byte that is data on one fork and an opcode on another, and lands the walk
  /// on a boundary the real chain never had.
  Hardfork Fork = Hardfork::Latest;
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
  BytecodeContainer Container = BytecodeContainer::Legacy;
  /// The fork normalization was performed under, which is the fork the result
  /// is only valid for.
  Hardfork Fork = Hardfork::Latest;
  /// True when the input was already the deployed code, which is what a
  /// compiler artifact's deployed-bytecode field holds. Searching such an
  /// input for a deployment wrapper can only find one that is not there.
  bool SourceIsRuntime = false;
  bool RuntimeExtracted = false;
  bool MetadataStripped = false;
  /// The decoded input exactly as it was spelled, before a deployment wrapper
  /// was unwrapped and before a trailer was removed. Re-normalizing under a
  /// different fork has to start from these bytes, because which of them are
  /// code is a question the fork answers.
  std::vector<uint8_t> Original;
  /// The trailer on the input as given. One compiler puts it here and nowhere
  /// else, because the members it added describe the runtime code the
  /// constructor is about to return and are useless once that code is running.
  std::optional<ContractMetadata> InputMetadata;
  /// The trailer on the code that remains after a deployment wrapper has been
  /// unwrapped, which is where the other compiler puts it. Set whether or not
  /// the trailer was removed, because what it says about the build is a fact
  /// about the input rather than a step of normalization. Its offset is
  /// relative to the runtime code.
  std::optional<ContractMetadata> RuntimeMetadata;

  /// The account a delegation indicator names, empty for every other input.
  [[nodiscard]] llvm::ArrayRef<uint8_t> delegateTarget() const;
  [[nodiscard]] ContainerDisposition disposition() const;
};

/// Normalize raw binary, hexadecimal text, or a compiler artifact into EVM
/// runtime bytecode. SourceName is used for diagnostics and input hints only.
llvm::Expected<LoadedBytecode>
decodeBytecodeInput(llvm::StringRef Content, llvm::StringRef SourceName = {},
                    const BytecodeLoadOptions &Options = {});

/// Re-run normalization on an already decoded container. This is how a session
/// whose fork differs from the one the input was first read under gets an
/// answer for its own fork without re-reading the file.
llvm::Expected<LoadedBytecode>
normalizeBytecode(llvm::ArrayRef<uint8_t> Original, BytecodeSourceKind Source,
                  bool SourceIsRuntime, llvm::StringRef SourceName = {},
                  const BytecodeLoadOptions &Options = {});

llvm::Expected<LoadedBytecode>
loadBytecodeFile(const std::filesystem::path &Path,
                 const BytecodeLoadOptions &Options = {});

/// Explain why \p Loaded cannot be decoded as instructions, or success when it
/// can. Analysis calls this instead of deciding for itself, so that "these
/// bytes are an address" is reported once and in the same words everywhere.
llvm::Error checkDecodable(const LoadedBytecode &Loaded,
                           llvm::StringRef SourceName = {});

/// The record a loader hands the frontend. The bytes are left behind because
/// the image already owns them; what travels is what had to be worked out.
ImageMetadata describeBytecode(const LoadedBytecode &Loaded);

/// Return true only for files that can be fully validated as EVM input.
bool looksLikeEVMInput(const std::filesystem::path &Path);

/// Return true for explicit EVM/container filename extensions. This does not
/// validate file contents and lets callers preserve detailed loader errors.
bool hasEVMFileExtension(const std::filesystem::path &Path);

} // namespace neverd::evm

#endif // NEVERD_EVM_BYTECODE_EVMBYTECODE_H
