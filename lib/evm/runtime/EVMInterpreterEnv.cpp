//===- EVMInterpreterEnv.cpp - EVM execution environment primitives -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMInterpreterDetail.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <limits>
#include <utility>

namespace neverd::evm::detail {
namespace {

enum class WordMapValuePolicy { Any, NonZero };

llvm::Error validateWordWidth(llvm::Twine Field, const llvm::APInt &Value) {
  if (Value.getBitWidth() == kWordBits)
    return llvm::Error::success();
  return llvm::make_error<llvm::StringError>(
      "evm: environment field " + Field + " must be " + llvm::Twine(kWordBits) +
          "-bit, got " + llvm::Twine(Value.getBitWidth()) + "-bit",
      llvm::inconvertibleErrorCode());
}

llvm::Error validateWordMap(llvm::StringRef Name, const WordMap &Map,
                            WordMapValuePolicy ValuePolicy) {
  for (const auto &[Key, Value] : Map) {
    if (llvm::Error E = validateWordWidth(llvm::Twine(Name) + " key", Key))
      return E;
    if (llvm::Error E = validateWordWidth(llvm::Twine(Name) + " value", Value))
      return E;
    if (ValuePolicy == WordMapValuePolicy::NonZero && Value.isZero())
      return llvm::make_error<llvm::StringError>(
          "evm: environment field " + llvm::Twine(Name) +
              " contains a zero-valued entry; sparse state must omit it",
          llvm::inconvertibleErrorCode());
  }
  return llvm::Error::success();
}

llvm::Error validateAddress(llvm::Twine Field, const llvm::APInt &Value) {
  if (llvm::Error E = validateWordWidth(Field, Value))
    return E;
  if (Value.getActiveBits() <= kAddressBits)
    return llvm::Error::success();
  return llvm::make_error<llvm::StringError>(
      "evm: environment field " + Field + " must fit a " +
          llvm::Twine(kAddressBits) + "-bit EVM address",
      llvm::inconvertibleErrorCode());
}

llvm::Error validateAddressMap(llvm::StringRef Name, const WordMap &Map) {
  for (const auto &[Key, Value] : Map) {
    if (llvm::Error E = validateAddress(llvm::Twine(Name) + " key", Key))
      return E;
    if (llvm::Error E = validateWordWidth(llvm::Twine(Name) + " value", Value))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error validateAggregateLimit(llvm::StringRef Name, size_t Limit,
                                   llvm::ArrayRef<size_t> Amounts) {
  size_t Used = 0;
  for (size_t Amount : Amounts) {
    if (Used > Limit || Amount > Limit - Used)
      return llvm::make_error<llvm::StringError>(
          "evm: environment exceeds " + llvm::Twine(Name) + " limit " +
              llvm::Twine(Limit),
          llvm::inconvertibleErrorCode());
    Used += Amount;
  }
  return llvm::Error::success();
}

llvm::Error validateExternalCode(const BytecodeMap &ExternalCode,
                                 size_t ByteLimit) {
  size_t Bytes = 0;
  for (const auto &[Address, Code] : ExternalCode) {
    if (llvm::Error E = validateAddress("ExternalCode key", Address))
      return E;
    if (Bytes > ByteLimit || Code.size() > ByteLimit - Bytes)
      return llvm::make_error<llvm::StringError>(
          "evm: environment exceeds " + llvm::Twine(kMaxExternalCodeBytesName) +
              " limit " + llvm::Twine(ByteLimit),
          llvm::inconvertibleErrorCode());
    Bytes += Code.size();
  }
  return llvm::Error::success();
}

} // namespace

llvm::APInt zeroWord() { return llvm::APInt(kWordBits, 0); }
llvm::APInt boolWord(bool Value) {
  return llvm::APInt(kWordBits, Value ? 1 : 0);
}

llvm::Error validateEnvironment(const ExecutionEnvironment &Environment,
                                const InterpreterOptions &Options) {
  const size_t CalldataBytes[] = {Environment.Calldata.size()};
  if (llvm::Error E = validateAggregateLimit(
          kMaxCalldataBytesName, Options.MaxCalldataBytes, CalldataBytes))
    return E;
  const size_t HostEnvironmentEntries[] = {
      Environment.BlockHashes.size(), Environment.Balances.size(),
      Environment.CodeHashes.size(), Environment.ExternalCode.size(),
      Environment.BlobHashes.size()};
  if (llvm::Error E = validateAggregateLimit(kMaxHostEnvironmentEntriesName,
                                             Options.MaxHostEnvironmentEntries,
                                             HostEnvironmentEntries))
    return E;
  const size_t HostReturnDataBytes[] = {Environment.InitialReturnData.size(),
                                        Environment.CallReturnData.size(),
                                        Environment.CreateReturnData.size()};
  if (llvm::Error E = validateAggregateLimit(kMaxHostReturnDataBytesName,
                                             Options.MaxHostReturnDataBytes,
                                             HostReturnDataBytes))
    return E;
  const size_t PersistentStateEntries[] = {Environment.Storage.size(),
                                           Environment.TransientStorage.size()};
  if (llvm::Error E = validateAggregateLimit(kMaxPersistentStateEntriesName,
                                             Options.MaxPersistentStateEntries,
                                             PersistentStateEntries))
    return E;

  struct NamedWord {
    llvm::StringLiteral Name;
    const llvm::APInt *Value;
  };
  const NamedWord Words[] = {
      {"CallValue", &Environment.CallValue},
      {"GasPrice", &Environment.GasPrice},
      {"Timestamp", &Environment.Timestamp},
      {"BlockNumber", &Environment.BlockNumber},
      {"Difficulty", &Environment.Difficulty},
      {"PrevRandao", &Environment.PrevRandao},
      {"GasLimit", &Environment.GasLimit},
      {"ChainID", &Environment.ChainID},
      {"BaseFee", &Environment.BaseFee},
      {"BlobBaseFee", &Environment.BlobBaseFee},
      {"GasRemaining", &Environment.GasRemaining},
  };
  for (const NamedWord &Word : Words)
    if (llvm::Error E = validateWordWidth(Word.Name, *Word.Value))
      return E;

  const NamedWord Addresses[] = {
      {"Address", &Environment.Address},
      {"Origin", &Environment.Origin},
      {"Caller", &Environment.Caller},
      {"Coinbase", &Environment.Coinbase},
      {"CreatedAddress", &Environment.CreatedAddress},
  };
  for (const NamedWord &Address : Addresses)
    if (llvm::Error E = validateAddress(Address.Name, *Address.Value))
      return E;

  struct NamedWordMap {
    llvm::StringLiteral Name;
    const WordMap *Map;
    WordMapValuePolicy ValuePolicy;
  };
  const NamedWordMap Maps[] = {
      {"Storage", &Environment.Storage, WordMapValuePolicy::NonZero},
      {"TransientStorage", &Environment.TransientStorage,
       WordMapValuePolicy::NonZero},
      {"BlockHashes", &Environment.BlockHashes, WordMapValuePolicy::Any},
  };
  for (const NamedWordMap &NamedMap : Maps)
    if (llvm::Error E =
            validateWordMap(NamedMap.Name, *NamedMap.Map, NamedMap.ValuePolicy))
      return E;

  if (llvm::Error E = validateAddressMap("Balances", Environment.Balances))
    return E;
  if (llvm::Error E = validateAddressMap("CodeHashes", Environment.CodeHashes))
    return E;

  if (llvm::Error E = validateExternalCode(Environment.ExternalCode,
                                           Options.MaxExternalCodeBytes))
    return E;
  for (const llvm::APInt &Hash : Environment.BlobHashes)
    if (llvm::Error E = validateWordWidth("BlobHashes value", Hash))
      return E;
  return llvm::Error::success();
}

std::optional<size_t> toSize(const llvm::APInt &Word, size_t Limit) {
  if (Word.getActiveBits() > std::numeric_limits<size_t>::digits)
    return std::nullopt;
  const uint64_t Value = Word.getZExtValue();
  if (Value > Limit)
    return std::nullopt;
  return static_cast<size_t>(Value);
}

bool checkedRange(size_t Offset, size_t Size, size_t Limit, size_t &End) {
  if (Offset > Limit || Size > Limit - Offset)
    return false;
  End = Offset + Size;
  return true;
}

llvm::APInt bytesToWord(const std::vector<uint8_t> &Bytes, size_t Offset) {
  llvm::APInt Result(kWordBits, 0);
  for (size_t I = 0; I < kWordBytes; ++I) {
    Result <<= kBitsPerByte;
    if (Offset <= Bytes.size() && I < Bytes.size() - Offset)
      Result |= Bytes[Offset + I];
  }
  return Result;
}

void wordToBytes(const llvm::APInt &Word, uint8_t *Output) {
  for (size_t I = 0; I < kWordBytes; ++I)
    Output[I] = static_cast<uint8_t>(Word.extractBitsAsZExtValue(
        kBitsPerByte,
        static_cast<unsigned>((kWordBytes - 1 - I) * kBitsPerByte)));
}

llvm::APInt mapLookup(const WordMap &Map, const llvm::APInt &Key) {
  auto It = Map.find(Key);
  return It == Map.end() ? zeroWord() : It->second;
}

llvm::APInt canonicalAddress(const llvm::APInt &Word) {
  return Word.trunc(kAddressBits).zext(kWordBits);
}

std::vector<uint8_t>::iterator byteIterator(std::vector<uint8_t> &Bytes,
                                            size_t Offset) {
  return Bytes.begin() +
         static_cast<std::vector<uint8_t>::difference_type>(Offset);
}

std::vector<uint8_t>::const_iterator
byteIterator(const std::vector<uint8_t> &Bytes, size_t Offset) {
  return Bytes.begin() +
         static_cast<std::vector<uint8_t>::difference_type>(Offset);
}

const std::vector<uint8_t> &codeLookup(const BytecodeMap &Map,
                                       const llvm::APInt &Address) {
  static const std::vector<uint8_t> Empty;
  auto It = Map.find(canonicalAddress(Address));
  return It == Map.end() ? Empty : It->second;
}

} // namespace neverd::evm::detail
