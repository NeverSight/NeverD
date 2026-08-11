//===- Bytecode.cpp - EVM bytecode input normalization ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Bytecode.h"

#include "neverd/evm/Decoder.h"
#include "neverd/evm/Metadata.h"
#include "neverd/evm/Opcodes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <array>
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

using AbstractValue = std::optional<uint64_t>;

AbstractValue pushedValue(const LowInstruction &Instruction) {
  // PUSH0 carries no immediate, so it has no decode status to be complete and
  // nothing that could have been truncated. It pushes zero.
  if (Instruction.Info.ImmediateBytes == 0)
    return uint64_t{0};
  // A push whose data ran off the end of the code pushes bytes the chain never
  // supplied, so its value is unknown rather than zero-padded.
  if (Instruction.ImmediateStatus != ImmediateDecodeStatus::Complete)
    return std::nullopt;
  if (Instruction.Immediate.getActiveBits() >
      std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return Instruction.Immediate.getZExtValue();
}

struct StaticCopy {
  uint64_t Destination = 0;
  uint64_t Source = 0;
  uint64_t Size = 0;
};

std::optional<std::vector<uint8_t>>
extractStaticRuntime(llvm::ArrayRef<uint8_t> Code, Hardfork Fork) {
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
    const LowInstruction Instruction =
        decodeInstructionAt(Code, PC, Fork, /*Diagnostics=*/nullptr);
    PC = Instruction.NextPC;
    // A byte the fork does not execute faults, which ends the constructor's
    // linear path just as a terminator does.
    if (!Instruction.isExecutable())
      return std::nullopt;

    const Opcode Op = Instruction.opcode();
    if (isPush(Op)) {
      Stack.push_back(pushedValue(Instruction));
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
    // The operand-indexed stack instructions reach an arbitrary depth chosen
    // by an immediate. No compiler emits one in a constructor wrapper, so
    // declining to model them costs nothing and keeps this walk from claiming
    // a provenance it did not follow.
    if (isDeepDup(Op) || isDeepSwap(Op) || isExchange(Op))
      return std::nullopt;
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
      // A static constructor wrapper copies an embedded runtime that follows
      // its terminal RETURN. Requiring that provenance prevents ordinary
      // runtime CODECOPY/RETURN logic from being destructively reclassified.
      if (Offset && Size && LastCopy && LastCopy->Destination == *Offset &&
          LastCopy->Size == *Size && LastCopy->Source >= PC &&
          LastCopy->Source <= Code.size() &&
          LastCopy->Size <= Code.size() - LastCopy->Source) {
        const size_t Source = static_cast<size_t>(LastCopy->Source);
        const size_t Size = static_cast<size_t>(LastCopy->Size);
        const llvm::ArrayRef<uint8_t> Runtime = Code.slice(Source, Size);
        return std::vector<uint8_t>(Runtime.begin(), Runtime.end());
      }
      return std::nullopt;
    }

    // This bounded abstract interpreter follows only the constructor's linear
    // path. A terminal/control-transfer instruction ends that proof; scanning
    // fallthrough bytes would reinterpret unreachable runtime data as code.
    if (Instruction.Info.IsTerminator)
      return std::nullopt;
    // The extractor remembers that a CODECOPY established a byte-for-byte
    // provenance relationship with the creation code. Any later memory write
    // invalidates that conservative proof, including compound host operations
    // such as CALL and EXTCODECOPY whose primary effect is not memory access.
    if (mayWriteMemory(Instruction.Info))
      LastCopy.reset();
    for (uint8_t I = 0; I < Instruction.Info.StackPops; ++I)
      (void)Pop();
    for (uint8_t I = 0; I < Instruction.Info.StackPushes; ++I)
      Stack.push_back(std::nullopt);
  }
  return std::nullopt;
}

llvm::Expected<LoadedBytecode> finishLoaded(std::vector<uint8_t> Code,
                                            BytecodeSourceKind Source,
                                            bool SourceIsRuntime,
                                            const BytecodeLoadOptions &Options,
                                            llvm::StringRef SourceName) {
  if (Code.empty())
    return inputError(SourceName, "empty bytecode");
  if (!isValidHardfork(Options.Fork))
    return inputError(SourceName, "invalid hardfork value");

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
    if (auto Runtime = extractStaticRuntime(Code, Options.Fork);
        Runtime && !Runtime->empty() && *Runtime != Code) {
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
  if (Code.empty())
    return inputError(SourceName,
                      "no executable bytecode remains after normalization");
  Result.Code = std::move(Code);
  return Result;
}

} // namespace

llvm::ArrayRef<BytecodeSourceInfo> bytecodeSourceInfos() {
  static const std::array Table = {
#define EVM_BYTECODE_SOURCE(ID, SPELLING, SUMMARY)                             \
  BytecodeSourceInfo{BytecodeSourceKind::ID, SPELLING, SUMMARY},
#include "neverd/evm/EVMBytecodeContainers.def"
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
#include "neverd/evm/EVMBytecodeContainers.def"
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
#include "neverd/evm/EVMBytecodeContainers.def"

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
#include "neverd/evm/EVMBytecodeContainers.def"
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
  const std::string Named =
      (Info.Name + " container (" + Info.EIP + ")").str();

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
      return inputError(SourceName,
                        Named + " is not assigned until " +
                            hardforkName(*Activated) + ", and the analyzed "
                            "fork is " +
                            hardforkName(Loaded.Fork));
    return inputError(SourceName,
                      Named + ": the code that runs here belongs to 0x" +
                          llvm::toHex(Loaded.delegateTarget(),
                                      /*LowerCase=*/true) +
                          ", whose runtime code was not supplied");
  }
  case ContainerDisposition::Unrecognized:
    return inputError(SourceName, Named + " is " + Info.Summary);
  }
  return inputError(SourceName, Named + " has no disposition");
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
