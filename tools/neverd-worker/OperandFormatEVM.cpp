//===- OperandFormatEVM.cpp - EVM operand formats -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Formats the data of an EVM PUSH the user gives a number format, in the
/// PUSH's own width of up to 256 bits. The engine spells the data in
/// hexadecimal with both digits of every byte, `PUSH2 0x00ff`.
///
//===----------------------------------------------------------------------===//

#include "OperandFormatDetail.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <vector>

namespace neverd::worker::operand_format_detail {
namespace {

/// An EVM word, least significant limb first.
using Word = std::array<std::uint64_t, 4>;
constexpr unsigned LimbBits = 64;
constexpr unsigned WordBits = LimbBits * 4;
/// The most PUSH data bytes, PUSH32's.
constexpr unsigned MaxPushBytes = WordBits / 8;
/// Nine decimal digits divide a word in 32-bit halves.
constexpr std::uint32_t DecimalChunk = 1000000000;
constexpr unsigned DecimalChunkDigits = 9;

std::optional<Word> pushData(const OperandTokens &tokens) {
  if (tokens.size() != 1 || tokens[0].kind != TokenKind::Number)
    return std::nullopt;
  std::string_view text = tokens[0].text;
  if (!text.starts_with("0x") && !text.starts_with("0X"))
    return std::nullopt;
  text.remove_prefix(2);
  if (text.empty() || text.size() > WordBits / 4)
    return std::nullopt;
  Word word{};
  for (const char c : text) {
    unsigned digit = 0;
    if (c >= '0' && c <= '9')
      digit = unsigned(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = unsigned(c - 'a') + 10;
    else if (c >= 'A' && c <= 'F')
      digit = unsigned(c - 'A') + 10;
    else
      return std::nullopt;
    for (std::size_t limb = word.size() - 1; limb; --limb)
      word[limb] = (word[limb] << 4) | (word[limb - 1] >> (LimbBits - 4));
    word[0] = (word[0] << 4) | digit;
  }
  return word;
}

void truncate(Word &word, unsigned bits) {
  for (std::size_t limb = 0; limb < word.size(); ++limb) {
    const unsigned low = unsigned(limb) * LimbBits;
    if (bits <= low)
      word[limb] = 0;
    else if (bits < low + LimbBits)
      word[limb] &= (std::uint64_t(1) << (bits - low)) - 1;
  }
}

bool isZero(const Word &word) {
  return std::all_of(word.begin(), word.end(),
                     [](std::uint64_t limb) { return !limb; });
}

/// Divides \p word in place by \p divisor, returning the remainder.
std::uint32_t divide(Word &word, std::uint32_t divisor) {
  std::uint64_t rest = 0;
  for (std::size_t limb = word.size(); limb--;)
    for (int half = 1; half >= 0; --half) {
      const unsigned shift = unsigned(half) * 32;
      const std::uint64_t current =
          (rest << 32) | ((word[limb] >> shift) & 0xffffffffu);
      rest = current % divisor;
      word[limb] = (word[limb] & ~(std::uint64_t(0xffffffffu) << shift)) |
                   ((current / divisor) << shift);
    }
  return std::uint32_t(rest);
}

std::string decimalDigits(Word word) {
  if (isZero(word))
    return "0";
  std::vector<std::uint32_t> chunks;
  while (!isZero(word))
    chunks.push_back(divide(word, DecimalChunk));
  std::string text = std::to_string(chunks.back());
  for (std::size_t chunk = chunks.size() - 1; chunk--;) {
    const std::string part = std::to_string(chunks[chunk]);
    text += std::string(DecimalChunkDigits - part.size(), '0') + part;
  }
  return text;
}

/// Hexadecimal with at least \p digits digits, as the engine pads PUSH data.
std::string hexDigits(const Word &word, std::size_t digits) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string text;
  for (std::size_t limb = word.size(); limb--;)
    for (int shift = LimbBits - 4; shift >= 0; shift -= 4)
      text += Digits[(word[limb] >> shift) & 0xf];
  const std::size_t first =
      std::min(text.find_first_not_of('0'),
               text.size() - std::max<std::size_t>(digits, 1));
  return "0x" + text.substr(first);
}

std::string binaryText(const Word &word) {
  std::string text;
  for (std::size_t limb = word.size(); limb--;)
    for (int bit = LimbBits - 1; bit >= 0; --bit) {
      const bool one = (word[limb] >> bit) & 1;
      if (one || !text.empty())
        text += one ? '1' : '0';
    }
  return text.empty() ? std::string("0") : text;
}

/// The bytes as characters, most significant first (`'ERC20'`); none unless
/// every byte but leading zeros prints.
std::optional<std::string> characterText(const Word &word) {
  std::string text;
  for (std::size_t limb = word.size(); limb--;)
    for (int shift = LimbBits - 8; shift >= 0; shift -= 8) {
      const auto byte = static_cast<unsigned char>(word[limb] >> shift);
      if (!byte && text.empty())
        continue;
      if (byte < 0x20 || byte >= 0x7f || byte == '\'')
        return std::nullopt;
      text += static_cast<char>(byte);
    }
  if (text.empty())
    return std::nullopt;
  return "'" + text + "'";
}

/// The word when it fits 64 bits.
std::optional<std::uint64_t> narrow(const Word &word) {
  if (word[1] || word[2] || word[3])
    return std::nullopt;
  return word[0];
}

} // namespace

unsigned evmPushBits(std::string_view mnemonic) {
  if (!mnemonic.starts_with("PUSH"))
    return 0;
  unsigned bytes = 0;
  const auto *end = mnemonic.data() + mnemonic.size();
  const auto parsed = std::from_chars(mnemonic.data() + 4, end, bytes);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !bytes ||
      bytes > MaxPushBytes)
    return 0;
  return bytes * 8;
}

bool evmPushData(std::string_view mnemonic, const OperandTokens &tokens) {
  return evmPushBits(mnemonic) && pushData(tokens);
}

std::optional<std::uint64_t> evmImmediate(std::string_view mnemonic,
                                          const OperandTokens &tokens) {
  if (!evmPushBits(mnemonic))
    return std::nullopt;
  const auto word = pushData(tokens);
  return word ? narrow(*word) : std::nullopt;
}

bool formatEvmOperand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const NumberFormat &format) {
  const unsigned bits = evmPushBits(facts.mnemonic);
  auto shown = bits ? pushData(tokens) : std::nullopt;
  if (!shown)
    return false;
  truncate(*shown, bits);
  std::string sign;
  if (format.negate) {
    for (auto &limb : *shown)
      limb = ~limb;
    for (auto &limb : *shown)
      if (++limb)
        break;
    truncate(*shown, bits);
    sign += '-';
  }
  if (format.invert) {
    for (auto &limb : *shown)
      limb = ~limb;
    truncate(*shown, bits);
    sign += '~';
  }
  std::string text;
  switch (format.base) {
  case NumberBase::Number:
  case NumberBase::Hex:
    text = hexDigits(*shown, bits / 4);
    break;
  case NumberBase::Decimal:
    text = decimalDigits(*shown);
    break;
  case NumberBase::Binary:
    text = "0b" + binaryText(*shown);
    break;
  case NumberBase::Char: {
    const auto characters = characterText(*shown);
    if (!characters)
      return false;
    text = *characters;
    break;
  }
  case NumberBase::Offset: {
    // A code offset, as a jump's destination is.
    const auto value = narrow(*shown);
    if (!sign.empty() || !value)
      return false;
    const LocationName name = namer(*value, NameUse::Address, {});
    if (name.text.empty())
      return false;
    out.append(name.text, name.role, name.address);
    return true;
  }
  case NumberBase::Enum: {
    const auto value = narrow(*shown);
    if (!sign.empty() || !facts.enums || !value ||
        bits > std::numeric_limits<std::uint64_t>::digits)
      return false;
    const auto symbolic =
        (*facts.enums)(format.type, *value, bits, format.delta != 0);
    if (!symbolic)
      return false;
    out.append(*symbolic, ListingRole::Keyword);
    return true;
  }
  case NumberBase::Struct: {
    const auto value = narrow(*shown);
    if (!sign.empty() || !facts.structs || !value ||
        bits > std::numeric_limits<std::uint64_t>::digits)
      return false;
    const auto landing = (*facts.structs)(
        format.type, static_cast<std::int64_t>(*value) + format.delta, 0);
    if (!landing)
      return false;
    std::string name = format.type;
    if (!landing->path.empty())
      name += "." + landing->path;
    out.append(name, ListingRole::StructOffset);
    if (const std::int64_t rest =
            static_cast<std::int64_t>(*value) - landing->offset) {
      out.append(rest < 0 ? "-" : "+", ListingRole::Punctuation);
      out.append(std::to_string(rest < 0
                                    ? std::uint64_t(0) - std::uint64_t(rest)
                                    : std::uint64_t(rest)),
                 ListingRole::Number);
    }
    return true;
  }
  }
  if (!sign.empty())
    out.append(sign, ListingRole::Punctuation);
  out.append(text, ListingRole::Number);
  return true;
}

} // namespace neverd::worker::operand_format_detail
