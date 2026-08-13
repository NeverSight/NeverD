//===- BytecodeDecode.cpp - EVM hex and artifact decoding ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "BytecodeDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::evm::detail {
namespace {

inline constexpr llvm::StringLiteral kArtifactObjectKey = "object";
inline constexpr llvm::StringLiteral kArtifactEVMKey = "evm";
inline constexpr llvm::StringLiteral kArtifactContractsKey = "contracts";
inline constexpr llvm::StringLiteral kArtifactDeployedBytecodeKey =
    "deployedBytecode";
inline constexpr llvm::StringLiteral kArtifactRuntimeBytecodeKey =
    "runtimeBytecode";
inline constexpr llvm::StringLiteral kArtifactBytecodeKey = "bytecode";

std::optional<llvm::StringRef> bytecodeObject(const llvm::json::Value *Value) {
  if (!Value)
    return std::nullopt;
  if (auto Text = Value->getAsString())
    return Text;
  if (const auto *Object = Value->getAsObject())
    return Object->getString(kArtifactObjectKey);
  return std::nullopt;
}

bool hasHexPayload(llvm::StringRef Text) {
  Text = Text.trim();
  (void)(Text.consume_front("0x") || Text.consume_front("0X"));
  return !Text.trim().empty();
}

struct ArtifactCandidate {
  std::string Contract;
  llvm::StringRef Text;
  bool IsRuntime = false;
};

void appendContractCandidate(std::vector<ArtifactCandidate> &Candidates,
                             llvm::StringRef Contract,
                             const llvm::json::Object &Object) {
  const llvm::json::Object *EVM = Object.getObject(kArtifactEVMKey);
  const llvm::json::Object &Container = EVM ? *EVM : Object;

  if (auto Text = bytecodeObject(Container.get(kArtifactDeployedBytecodeKey));
      Text && hasHexPayload(*Text)) {
    Candidates.push_back({Contract.str(), *Text, true});
    return;
  }
  if (auto Text = bytecodeObject(Container.get(kArtifactRuntimeBytecodeKey));
      Text && hasHexPayload(*Text)) {
    Candidates.push_back({Contract.str(), *Text, true});
    return;
  }
  if (auto Text = bytecodeObject(Container.get(kArtifactBytecodeKey));
      Text && hasHexPayload(*Text))
    Candidates.push_back({Contract.str(), *Text, false});
}

llvm::Expected<ArtifactCandidate>
selectArtifactBytecode(const llvm::json::Value &Root,
                       const BytecodeLoadOptions &Options,
                       llvm::StringRef SourceName) {
  const auto *RootObject = Root.getAsObject();
  if (!RootObject)
    return inputError(SourceName, "artifact root must be a JSON object");

  std::vector<ArtifactCandidate> Candidates;
  appendContractCandidate(Candidates, "", *RootObject);

  if (const auto *Contracts = RootObject->getObject(kArtifactContractsKey)) {
    for (const auto &FileEntry : *Contracts) {
      const auto *FileContracts = FileEntry.second.getAsObject();
      if (!FileContracts)
        continue;
      for (const auto &ContractEntry : *FileContracts) {
        const auto *ContractObject = ContractEntry.second.getAsObject();
        if (!ContractObject)
          continue;
        std::string Qualified = llvm::StringRef(FileEntry.first).str() + ":" +
                                llvm::StringRef(ContractEntry.first).str();
        appendContractCandidate(Candidates, Qualified, *ContractObject);
      }
    }
  }

  if (Candidates.empty())
    return inputError(SourceName,
                      "artifact contains no bytecode or deployed bytecode");

  if (!Options.ArtifactContract.empty()) {
    llvm::StringRef Selector(Options.ArtifactContract);
    const std::string ContractSuffix = ":" + Selector.str();
    const ArtifactCandidate *Match = nullptr;
    for (const auto &Candidate : Candidates) {
      llvm::StringRef Qualified(Candidate.Contract);
      if (Qualified != Selector && !Qualified.ends_with(ContractSuffix))
        continue;
      if (Match)
        return inputError(SourceName,
                          "contract selector '" + Selector +
                              "' is ambiguous; use path/File.sol:Contract");
      Match = &Candidate;
    }
    if (Match)
      return *Match;
    return inputError(SourceName,
                      "contract '" + Selector + "' is not present in artifact");
  }

  if (Candidates.size() != 1)
    return inputError(
        SourceName,
        "artifact contains multiple contracts; select one explicitly");
  return Candidates.front();
}

} // namespace

llvm::Expected<std::vector<uint8_t>>
decodeHexBytecode(llvm::StringRef Content, llvm::StringRef SourceName) {
  std::string Compact;
  Compact.reserve(Content.size());
  for (char C : Content)
    if (!llvm::isSpace(C))
      Compact.push_back(C);

  llvm::StringRef Text(Compact);
  if (Text.consume_front("0x") || Text.consume_front("0X")) {
    // Prefix consumed.
  }
  if (Text.empty())
    return inputError(SourceName, "empty bytecode");
  if (Text.contains("__") || Text.contains('$'))
    return inputError(SourceName, "unresolved library placeholder in bytecode");
  static_assert(kHexDigitsPerByte == 2);
  if (Text.size() % kHexDigitsPerByte != 0)
    return inputError(SourceName, "hex bytecode has an odd number of digits");

  std::vector<uint8_t> Result;
  Result.reserve(Text.size() / kHexDigitsPerByte);
  for (size_t I = 0; I < Text.size(); I += kHexDigitsPerByte) {
    uint8_t Byte = 0;
    if (!llvm::tryGetHexFromNibbles(Text[I], Text[I + 1], Byte))
      return inputError(SourceName, "invalid hexadecimal digit at offset " +
                                        llvm::Twine(I));
    Result.push_back(Byte);
  }
  return Result;
}

llvm::Expected<DecodedArtifact>
decodeArtifactBytecode(llvm::StringRef Content,
                       const BytecodeLoadOptions &Options,
                       llvm::StringRef SourceName) {
  auto Parsed = llvm::json::parse(Content);
  if (!Parsed)
    return inputError(SourceName,
                      "invalid compiler artifact: " +
                          llvm::Twine(llvm::toString(Parsed.takeError())));
  auto Candidate = selectArtifactBytecode(*Parsed, Options, SourceName);
  if (!Candidate)
    return Candidate.takeError();
  auto Code = decodeHexBytecode(Candidate->Text, SourceName);
  if (!Code)
    return Code.takeError();
  return DecodedArtifact{std::move(*Code), Candidate->IsRuntime};
}

} // namespace neverd::evm::detail
