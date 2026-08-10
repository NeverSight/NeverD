//===- Bytecode.cpp - EVM bytecode input normalization ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Bytecode.h"

#include "neverd/evm/Opcodes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace neverd::evm {
namespace {

inline constexpr llvm::StringLiteral kArtifactObjectKey = "object";
inline constexpr llvm::StringLiteral kArtifactEVMKey = "evm";
inline constexpr llvm::StringLiteral kArtifactContractsKey = "contracts";
inline constexpr llvm::StringLiteral kArtifactDeployedBytecodeKey =
    "deployedBytecode";
inline constexpr llvm::StringLiteral kArtifactRuntimeBytecodeKey =
    "runtimeBytecode";
inline constexpr llvm::StringLiteral kArtifactBytecodeKey = "bytecode";
inline constexpr llvm::StringLiteral kRawExtensions[] = {".raw", ".evmraw"};
inline constexpr llvm::StringLiteral kEVMExtensions[] = {
    ".evm", ".hex", ".bin", ".bytecode", ".json", ".raw", ".evmraw"};
inline constexpr llvm::StringLiteral kSolidityMetadataKeys[] = {
    "solc", "ipfs", "bzzr0", "bzzr1"};
inline constexpr uint8_t kCBORMajorTypeMask = 0xe0U;
inline constexpr uint8_t kCBORMapMajorType = 0xa0U;

llvm::Error inputError(llvm::StringRef SourceName, llvm::Twine Message) {
  std::string Text = kEVMArchName.str();
  if (!SourceName.empty())
    Text += ": " + SourceName.str();
  Text += ": " + Message.str();
  return llvm::make_error<llvm::StringError>(Text,
                                             llvm::inconvertibleErrorCode());
}

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

llvm::Expected<std::vector<uint8_t>> decodeHex(llvm::StringRef Content,
                                               llvm::StringRef SourceName) {
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

std::optional<llvm::StringRef> bytecodeObject(const llvm::json::Value *Value) {
  if (!Value)
    return std::nullopt;
  if (auto Text = Value->getAsString())
    return Text;
  if (const auto *Object = Value->getAsObject())
    return Object->getString(kArtifactObjectKey);
  return std::nullopt;
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
      Text && !Text->empty()) {
    Candidates.push_back({Contract.str(), *Text, true});
    return;
  }
  if (auto Text = bytecodeObject(Container.get(kArtifactRuntimeBytecodeKey));
      Text && !Text->empty()) {
    Candidates.push_back({Contract.str(), *Text, true});
    return;
  }
  if (auto Text = bytecodeObject(Container.get(kArtifactBytecodeKey));
      Text && !Text->empty())
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

using AbstractValue = std::optional<uint64_t>;

AbstractValue readPushValue(const std::vector<uint8_t> &Code, size_t Offset,
                            size_t Width) {
  if (Width > sizeof(uint64_t) || Offset > Code.size() ||
      Width > Code.size() - Offset)
    return std::nullopt;
  uint64_t Value = 0;
  for (size_t I = 0; I < Width; ++I)
    Value = (Value << kBitsPerByte) | Code[Offset + I];
  return Value;
}

struct StaticCopy {
  uint64_t Destination = 0;
  uint64_t Source = 0;
  uint64_t Size = 0;
};

std::optional<std::vector<uint8_t>>
extractStaticRuntime(const std::vector<uint8_t> &Code) {
  std::vector<AbstractValue> Stack;
  std::optional<StaticCopy> LastCopy;
  auto Pop = [&]() -> AbstractValue {
    if (Stack.empty())
      return std::nullopt;
    AbstractValue Value = Stack.back();
    Stack.pop_back();
    return Value;
  };

  for (size_t PC = 0; PC < Code.size();) {
    const Opcode Op = static_cast<Opcode>(Code[PC++]);
    if (isPush(Op)) {
      const size_t Width = pushDataSize(Op);
      Stack.push_back(Width == 0 ? AbstractValue{uint64_t{0}}
                                 : readPushValue(Code, PC, Width));
      PC = std::min(Code.size(), PC + Width);
      continue;
    }
    if (isDup(Op)) {
      const size_t Depth = dupDepth(Op);
      Stack.push_back(Stack.size() >= Depth ? Stack[Stack.size() - Depth]
                                            : AbstractValue{});
      continue;
    }
    if (isSwap(Op)) {
      const size_t Depth = swapDepth(Op);
      if (Stack.size() > Depth)
        std::swap(Stack.back(), Stack[Stack.size() - 1 - Depth]);
      else
        Stack.clear();
      continue;
    }
    if (Op == Opcode::POP) {
      (void)Pop();
      continue;
    }
    if (Op == Opcode::CODECOPY) {
      const AbstractValue Destination = Pop();
      const AbstractValue Source = Pop();
      const AbstractValue Size = Pop();
      if (Destination && Source && Size)
        LastCopy = StaticCopy{*Destination, *Source, *Size};
      else
        LastCopy.reset();
      continue;
    }
    if (Op == Opcode::RETURN) {
      const AbstractValue Offset = Pop();
      const AbstractValue Size = Pop();
      if (Offset && Size && LastCopy && LastCopy->Destination == *Offset &&
          LastCopy->Size == *Size && LastCopy->Source <= Code.size() &&
          LastCopy->Size <= Code.size() - LastCopy->Source) {
        const size_t Source = static_cast<size_t>(LastCopy->Source);
        const size_t Size = static_cast<size_t>(LastCopy->Size);
        const auto Begin = Code.begin() + static_cast<std::ptrdiff_t>(Source);
        return std::vector<uint8_t>(Begin,
                                    Begin + static_cast<std::ptrdiff_t>(Size));
      }
      return std::nullopt;
    }

    const auto Info = opcodeInfo(Op);
    if (!Info)
      return std::nullopt;
    // This bounded abstract interpreter follows only the constructor's linear
    // path. A terminal/control-transfer instruction ends that proof; scanning
    // fallthrough bytes would reinterpret unreachable runtime data as code.
    if (Info->IsTerminator)
      return std::nullopt;
    // The extractor remembers that a CODECOPY established a byte-for-byte
    // provenance relationship with the creation code. Any later memory write
    // invalidates that conservative proof, including compound host operations
    // such as CALL and EXTCODECOPY whose primary effect is not memory access.
    if (mayWriteMemory(*Info))
      LastCopy.reset();
    for (uint8_t I = 0; I < Info->StackInputs; ++I)
      (void)Pop();
    for (uint8_t I = 0; I < Info->StackOutputs; ++I)
      Stack.push_back(std::nullopt);
  }
  return std::nullopt;
}

bool containsBytes(const std::vector<uint8_t> &Data, size_t Begin, size_t End,
                   llvm::StringRef Needle) {
  if (Begin > End || End > Data.size())
    return false;
  const auto First = Data.begin() + static_cast<std::ptrdiff_t>(Begin);
  const auto Last = Data.begin() + static_cast<std::ptrdiff_t>(End);
  return std::search(First, Last, Needle.begin(), Needle.end()) != Last;
}

bool stripSolidityMetadata(std::vector<uint8_t> &Code) {
  if (Code.size() <= kMetadataLengthBytes)
    return false;
  const size_t MetadataSize =
      (static_cast<size_t>(Code[Code.size() - kMetadataLengthBytes])
       << kBitsPerByte) |
      Code.back();
  if (MetadataSize == 0 || MetadataSize + kMetadataLengthBytes >= Code.size())
    return false;
  const size_t Start = Code.size() - MetadataSize - kMetadataLengthBytes;
  // Solidity appends a CBOR map. Validate both the major type and a compiler
  // metadata key so arbitrary bytecode ending in a small integer is untouched.
  if ((Code[Start] & kCBORMajorTypeMask) != kCBORMapMajorType)
    return false;
  const size_t End = Code.size() - kMetadataLengthBytes;
  if (llvm::none_of(kSolidityMetadataKeys, [&](llvm::StringRef Key) {
        return containsBytes(Code, Start, End, Key);
      }))
    return false;
  Code.resize(Start);
  return true;
}

llvm::Expected<LoadedBytecode> finishLoaded(std::vector<uint8_t> Code,
                                            BytecodeSourceKind Source,
                                            bool AlreadyRuntime,
                                            const BytecodeLoadOptions &Options,
                                            llvm::StringRef SourceName) {
  if (Code.empty())
    return inputError(SourceName, "empty bytecode");

  LoadedBytecode Result;
  Result.Source = Source;
  Result.OriginalSize = Code.size();
  if (Options.ExtractRuntime && !AlreadyRuntime) {
    if (auto Runtime = extractStaticRuntime(Code);
        Runtime && !Runtime->empty() && *Runtime != Code) {
      Code = std::move(*Runtime);
      Result.RuntimeExtracted = true;
    }
  }
  if (Options.StripMetadata)
    Result.MetadataStripped = stripSolidityMetadata(Code);
  if (Code.empty())
    return inputError(SourceName,
                      "no executable bytecode remains after normalization");
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
    auto Code = decodeHex(Content, SourceName);
    if (!Code)
      return Code.takeError();
    return finishLoaded(std::move(*Code), BytecodeSourceKind::Hex, false,
                        Options, SourceName);
  }

  auto Parsed = llvm::json::parse(Content);
  if (!Parsed)
    return inputError(SourceName,
                      "invalid compiler artifact: " +
                          llvm::Twine(llvm::toString(Parsed.takeError())));
  auto Candidate = selectArtifactBytecode(*Parsed, Options, SourceName);
  if (!Candidate)
    return Candidate.takeError();
  auto Code = decodeHex(Candidate->Text, SourceName);
  if (!Code)
    return Code.takeError();
  return finishLoaded(std::move(*Code), BytecodeSourceKind::Artifact,
                      Candidate->IsRuntime, Options, SourceName);
}

llvm::Expected<LoadedBytecode>
loadBytecodeFile(const std::filesystem::path &Path,
                 const BytecodeLoadOptions &Options) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string(), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return inputError(Path.string(), "cannot open input file");
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
  return !Loaded->Code.empty();
}

} // namespace neverd::evm
