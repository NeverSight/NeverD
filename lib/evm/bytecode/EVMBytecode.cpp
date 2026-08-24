//===- EVMBytecode.cpp - EVM bytecode loading and normalization ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMBytecodeDetail.h"

#include "neverd/evm/bytecode/EVMMetadata.h"
#include "neverd/evm/bytecode/EVMOpcodes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace neverd::evm {
namespace detail {

llvm::Error inputError(llvm::StringRef SourceName, llvm::Twine Message) {
  std::string Text = kEVMArchName.str();
  if (!SourceName.empty())
    Text += ": " + SourceName.str();
  Text += ": " + Message.str();
  return llvm::make_error<llvm::StringError>(Text,
                                             llvm::inconvertibleErrorCode());
}

} // namespace detail
namespace {

inline constexpr llvm::StringLiteral kRawExtensions[] = {".raw", ".evmraw"};
inline constexpr llvm::StringLiteral kEVMExtensions[] = {
    ".evm", ".hex", ".bin", ".bytecode", ".json", ".raw", ".evmraw"};

bool hasRawSuffix(llvm::StringRef SourceName) {
  std::string Lower = SourceName.lower();
  return llvm::any_of(kRawExtensions, [&](llvm::StringRef Extension) {
    return llvm::StringRef(Lower).ends_with(Extension);
  });
}

bool looksBinary(llvm::StringRef Content) {
  return llvm::any_of(Content, [](char C) {
    return C == 0 || (!llvm::isPrint(C) && !llvm::isSpace(C));
  });
}

llvm::Expected<LoadedBytecode> finishLoaded(std::vector<uint8_t> Code,
                                            BytecodeSourceKind Source,
                                            bool SourceIsRuntime,
                                            const BytecodeLoadOptions &Options,
                                            llvm::StringRef SourceName) {
  if (!isValidHardfork(Options.Fork))
    return detail::inputError(SourceName, "invalid hardfork value");

  LoadedBytecode Result;
  Result.Source = Source;
  Result.Fork = Options.Fork;
  Result.SourceIsRuntime = SourceIsRuntime;
  Result.Original = Code;
  Result.Container = classifyBytecodeContainer(Code);

  // A container that is not instructions has no deployment wrapper to unwrap
  // and no trailer to find. Running either step would report a transformation
  // that did not happen, on bytes that are not code.
  if (getBytecodeContainerInfo(Result.Container).Disposition !=
      ContainerDisposition::Decodable) {
    Result.Code = std::move(Code);
    return Result;
  }

  // The trailer is read before extraction as well as after it. One compiler
  // writes it into the deployment container and leaves the runtime code
  // without one, because the layout it records is about code that has not been
  // returned yet; a reader that only looks after unwrapping finds nothing and
  // reports an unknown build for a contract that named itself.
  Result.InputMetadata = findContractMetadata(Code);

  if (Options.ExtractRuntime && !SourceIsRuntime) {
    if (auto Runtime = detail::extractStaticRuntime(Code, Options.Fork);
        Runtime && *Runtime != Code) {
      Code = std::move(*Runtime);
      Result.RuntimeExtracted = true;
    }
  }

  // The trailer is read whether or not it is removed: finding it is what tells
  // the decoder which bytes are not code, and what it says about the compiler
  // is worth reporting either way.
  Result.RuntimeMetadata = Result.RuntimeExtracted ? findContractMetadata(Code)
                                                   : Result.InputMetadata;
  if (Options.StripMetadata && Result.RuntimeMetadata &&
      Result.RuntimeMetadata->Offset != 0) {
    Code.resize(Result.RuntimeMetadata->Offset);
    Result.MetadataStripped = true;
  }
  Result.Code = std::move(Code);
  return Result;
}

} // namespace

llvm::Expected<LoadedBytecode>
decodeBytecodeInput(llvm::StringRef Content, llvm::StringRef SourceName,
                    const BytecodeLoadOptions &Options) {
  BytecodeInputFormat Format = Options.Format;
  llvm::StringRef Trimmed = Content.trim();
  if (Format == BytecodeInputFormat::Auto) {
    if (hasRawSuffix(SourceName) || looksBinary(Content))
      Format = BytecodeInputFormat::Raw;
    else if (Trimmed.starts_with("{") || Trimmed.starts_with("["))
      Format = BytecodeInputFormat::Artifact;
    else
      Format = BytecodeInputFormat::Hex;
  }

  if (Format == BytecodeInputFormat::Raw) {
    std::vector<uint8_t> Code(
        reinterpret_cast<const uint8_t *>(Content.data()),
        reinterpret_cast<const uint8_t *>(Content.data() + Content.size()));
    return finishLoaded(std::move(Code), BytecodeSourceKind::Raw, false,
                        Options, SourceName);
  }

  if (Format == BytecodeInputFormat::Hex) {
    auto Code = detail::decodeHexBytecode(Content, SourceName);
    if (!Code)
      return Code.takeError();
    return finishLoaded(std::move(*Code), BytecodeSourceKind::Hex, false,
                        Options, SourceName);
  }

  auto Artifact = detail::decodeArtifactBytecode(Content, Options, SourceName);
  if (!Artifact)
    return Artifact.takeError();
  return finishLoaded(std::move(Artifact->Code), BytecodeSourceKind::Artifact,
                      Artifact->IsRuntime, Options, SourceName);
}

llvm::Expected<LoadedBytecode>
normalizeBytecode(llvm::ArrayRef<uint8_t> Original, BytecodeSourceKind Source,
                  bool SourceIsRuntime, llvm::StringRef SourceName,
                  const BytecodeLoadOptions &Options) {
  return finishLoaded(std::vector<uint8_t>(Original.begin(), Original.end()),
                      Source, SourceIsRuntime, Options, SourceName);
}

llvm::Expected<LoadedBytecode>
loadBytecodeFile(const std::filesystem::path &Path,
                 const BytecodeLoadOptions &Options) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string(), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return detail::inputError(Path.string(), "cannot open input file");
  return decodeBytecodeInput((*Buffer)->getBuffer(), Path.string(), Options);
}

bool hasEVMFileExtension(const std::filesystem::path &Path) {
  std::string Extension = Path.extension().string();
  std::transform(Extension.begin(), Extension.end(), Extension.begin(),
                 [](char C) { return llvm::toLower(C); });
  return llvm::any_of(kEVMExtensions, [&](llvm::StringRef Known) {
    return llvm::StringRef(Extension) == Known;
  });
}

bool looksLikeEVMInput(const std::filesystem::path &Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string(), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer || looksBinary((*Buffer)->getBuffer()))
    return false;
  // Extension-free auto-detection is intentionally limited to validated text;
  // otherwise every non-empty native data blob would look like raw EVM code.
  BytecodeLoadOptions Options;
  Options.Format = (*Buffer)->getBuffer().trim().starts_with("{")
                       ? BytecodeInputFormat::Artifact
                       : BytecodeInputFormat::Hex;
  auto Loaded =
      decodeBytecodeInput((*Buffer)->getBuffer(), Path.string(), Options);
  if (!Loaded) {
    llvm::consumeError(Loaded.takeError());
    return false;
  }
  return true;
}

} // namespace neverd::evm
