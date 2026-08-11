//===- Pubkey.cpp - Solana address encoding -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Pubkey.h"

#include "neverd/sbf/SBFConstants.h"

#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <limits>

namespace neverd::sbf {
namespace {

/// Base conversion carries stay below 58 * 255 and 256 * 57, so a 32-bit
/// accumulator cannot overflow.
using Carry = uint32_t;
static_assert(kBase58Radix * std::numeric_limits<uint8_t>::max() <=
                  std::numeric_limits<Carry>::max(),
              "the base58 accumulator must hold one digit-times-byte product");

constexpr Carry kByteRadix = Carry{1} << kBitsPerByte;

llvm::Error base58Error(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(("sbf: base58: " + Message).str(),
                                             llvm::inconvertibleErrorCode());
}

size_t countLeading(llvm::ArrayRef<uint8_t> Bytes, uint8_t Value) {
  const auto *First = std::find_if(Bytes.begin(), Bytes.end(),
                                   [&](uint8_t Byte) { return Byte != Value; });
  return static_cast<size_t>(First - Bytes.begin());
}

} // namespace

bool Pubkey::isZero() const {
  return std::all_of(Bytes.begin(), Bytes.end(),
                     [](uint8_t Byte) { return Byte == 0; });
}

std::string encodeBase58(llvm::ArrayRef<uint8_t> Bytes) {
  const size_t LeadingZeros = countLeading(Bytes, 0);

  // Little-endian base58 digits of the remaining big-endian integer.
  llvm::SmallVector<uint8_t> Digits;
  Digits.reserve(maxBase58Length(Bytes.size()));
  for (uint8_t Byte : Bytes.drop_front(LeadingZeros)) {
    Carry Accumulator = Byte;
    for (uint8_t &Digit : Digits) {
      Accumulator += static_cast<Carry>(Digit) * kByteRadix;
      Digit = static_cast<uint8_t>(Accumulator % kBase58Radix);
      Accumulator /= kBase58Radix;
    }
    while (Accumulator != 0) {
      Digits.push_back(static_cast<uint8_t>(Accumulator % kBase58Radix));
      Accumulator /= kBase58Radix;
    }
  }

  std::string Text(LeadingZeros, kBase58ZeroDigit);
  Text.reserve(LeadingZeros + Digits.size());
  for (uint8_t Digit : llvm::reverse(Digits))
    Text.push_back(kBase58Alphabet[Digit]);
  return Text;
}

llvm::Expected<llvm::SmallVector<uint8_t>> decodeBase58(llvm::StringRef Text) {
  const size_t LeadingZeros = Text.size() - Text.ltrim(kBase58ZeroDigit).size();

  // Little-endian bytes of the decoded big-endian integer.
  llvm::SmallVector<uint8_t> Bytes;
  Bytes.reserve(maxBase58ByteCount(Text.size()));
  for (char Character : Text.drop_front(LeadingZeros)) {
    const size_t Digit = kBase58Alphabet.find(Character);
    if (Digit == llvm::StringRef::npos)
      return base58Error("invalid digit '" + llvm::Twine(Character) + "'");
    Carry Accumulator = static_cast<Carry>(Digit);
    for (uint8_t &Byte : Bytes) {
      Accumulator += static_cast<Carry>(Byte) * kBase58Radix;
      Byte = static_cast<uint8_t>(Accumulator % kByteRadix);
      Accumulator /= kByteRadix;
    }
    while (Accumulator != 0) {
      Bytes.push_back(static_cast<uint8_t>(Accumulator % kByteRadix));
      Accumulator /= kByteRadix;
    }
  }

  llvm::SmallVector<uint8_t> Result(LeadingZeros, 0);
  Result.reserve(LeadingZeros + Bytes.size());
  llvm::append_range(Result, llvm::reverse(Bytes));
  return Result;
}

llvm::Expected<Pubkey> parsePubkey(llvm::StringRef Text) {
  llvm::Expected<llvm::SmallVector<uint8_t>> Bytes = decodeBase58(Text);
  if (!Bytes)
    return Bytes.takeError();
  if (Bytes->size() != kPubkeyByteCount)
    return base58Error("address '" + Text + "' decodes to " +
                       llvm::Twine(Bytes->size()) + " bytes, expected " +
                       llvm::Twine(kPubkeyByteCount));
  return readPubkey(*Bytes);
}

std::string formatPubkey(const Pubkey &Key) { return encodeBase58(Key.Bytes); }

llvm::Expected<Pubkey> readPubkey(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < kPubkeyByteCount)
    return base58Error("need " + llvm::Twine(kPubkeyByteCount) +
                       " bytes to read an address, have " +
                       llvm::Twine(Bytes.size()));
  Pubkey Key;
  llvm::copy(Bytes.take_front(kPubkeyByteCount), Key.Bytes.begin());
  return Key;
}

} // namespace neverd::sbf
