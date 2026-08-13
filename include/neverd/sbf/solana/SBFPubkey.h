//===- SBFPubkey.h - Solana address encoding --------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the 32-byte Solana address and its base58 spelling. Every recovered
/// program ID, account owner, and sysvar reference is presented through this
/// type so a raw byte window is never printed as if it were an address.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFPUBKEY_H
#define NEVERD_SBF_SOLANA_SBFPUBKEY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::sbf {

/// Bitcoin base58 alphabet, which Solana adopts unchanged. It omits the four
/// glyphs that are ambiguous in print: zero, capital O, capital I, lowercase l.
inline constexpr llvm::StringLiteral kBase58Alphabet(
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz");
inline constexpr size_t kBase58Radix = kBase58Alphabet.size();
inline constexpr char kBase58ZeroDigit = *kBase58Alphabet.data();

static_assert(kBase58Radix == 58,
              "the base58 alphabet must hold exactly 58 digits");

/// Integer over-approximations of log(256)/log(58) and its reciprocal. They
/// only size buffers; the conversions themselves stay exact.
inline constexpr size_t kBase58DigitsPerByteNumerator = 138;
inline constexpr size_t kBase58DigitsPerByteDenominator = 100;
inline constexpr size_t kBase58BytesPerDigitNumerator = 733;
inline constexpr size_t kBase58BytesPerDigitDenominator = 1000;

constexpr size_t maxBase58Length(size_t ByteCount) {
  return ByteCount * kBase58DigitsPerByteNumerator /
             kBase58DigitsPerByteDenominator +
         1;
}

constexpr size_t maxBase58ByteCount(size_t Length) {
  return Length * kBase58BytesPerDigitNumerator /
             kBase58BytesPerDigitDenominator +
         1;
}

/// Length of an ed25519 public key, which is also the length of a program
/// address, a program-derived address, and every account owner field.
inline constexpr size_t kPubkeyByteCount = 32;

/// An all-zero key encodes as one base58 zero digit per byte, which is the
/// shortest spelling any 32-byte value can have.
inline constexpr size_t kMinPubkeyStringLength = kPubkeyByteCount;

struct Pubkey {
  std::array<uint8_t, kPubkeyByteCount> Bytes{};

  bool isZero() const;

  friend bool operator==(const Pubkey &, const Pubkey &) = default;
  friend std::strong_ordering operator<=>(const Pubkey &,
                                          const Pubkey &) = default;
};

std::string encodeBase58(llvm::ArrayRef<uint8_t> Bytes);
llvm::Expected<llvm::SmallVector<uint8_t>> decodeBase58(llvm::StringRef Text);

/// Parse a base58 address, rejecting any spelling that does not decode to
/// exactly \c kPubkeyByteCount bytes.
llvm::Expected<Pubkey> parsePubkey(llvm::StringRef Text);
std::string formatPubkey(const Pubkey &Key);

/// Read a key from the first \c kPubkeyByteCount bytes of \p Bytes.
llvm::Expected<Pubkey> readPubkey(llvm::ArrayRef<uint8_t> Bytes);

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFPUBKEY_H
