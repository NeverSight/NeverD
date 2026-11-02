//===- OperandFormatSBF.cpp - SBF operand formats -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Formats the SBF operands the user gives a number format: an ALU, store or
/// branch immediate, or an lddw constant, in the width the instruction uses
/// it, and a memory displacement as a structure offset. The engine spells
/// immediates in signed decimal, the lddw constant in hexadecimal and memory
/// as `[r10 - 8]`.
///
//===----------------------------------------------------------------------===//

#include "OperandFormatDetail.h"

#include <limits>
#include <string>

namespace neverd::worker::operand_format_detail {
namespace {

bool isPunct(const Token &token, std::string_view text) {
  return token.kind == TokenKind::Punct && token.text == text;
}

/// Whether \p mnemonic's number is no constant of the program: a call
/// target or syscall hash, or the width a byte swap takes.
bool sbfOpaqueNumber(std::string_view mnemonic) {
  return mnemonic == "call" || mnemonic == "le" || mnemonic == "be";
}

/// The width the instruction uses its immediate in: a store's data, a
/// 32-bit operation's, or 64 bits.
unsigned sbfImmediateBits(std::string_view mnemonic) {
  if (mnemonic == "stb")
    return 8;
  if (mnemonic == "sth")
    return 16;
  if (mnemonic == "stw" || mnemonic.ends_with("32"))
    return 32;
  return 64;
}

/// The bytes a load or store moves: `ldxw`, `stxdw`, `stb`; none otherwise.
unsigned sbfAccessBytes(std::string_view mnemonic) {
  if (!mnemonic.starts_with("ld") && !mnemonic.starts_with("st"))
    return 0;
  if (mnemonic.ends_with("dw"))
    return 8;
  if (mnemonic.ends_with("w"))
    return 4;
  if (mnemonic.ends_with("h"))
    return 2;
  if (mnemonic.ends_with("b"))
    return 1;
  return 0;
}

/// A memory operand, `[r10 - 8]`, `[r1 + 16]` or `[r2]`.
struct SbfAddress {
  std::string_view base;
  std::int64_t displacement = 0;
};

std::optional<SbfAddress> sbfAddress(const OperandTokens &tokens) {
  std::vector<Token> inner;
  for (const Token &token : tokens)
    if (token.kind != TokenKind::Space)
      inner.push_back(token);
  if (inner.size() < 3 || !isPunct(inner.front(), "[") ||
      !isPunct(inner.back(), "]") || inner[1].kind != TokenKind::Identifier ||
      !isRegisterName(OperandDialect::SBF, inner[1].text))
    return std::nullopt;
  SbfAddress result{inner[1].text, 0};
  if (inner.size() == 3)
    return result;
  if (inner.size() != 5 ||
      !(isPunct(inner[2], "+") || isPunct(inner[2], "-")) ||
      inner[3].kind != TokenKind::Number || !inner[3].valid ||
      inner[3].value > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
    return std::nullopt;
  const auto magnitude = static_cast<std::int64_t>(inner[3].value);
  result.displacement = isPunct(inner[2], "-") ? -magnitude : magnitude;
  return result;
}

/// Appends the member \p value plus the format's delta lands on, and any
/// rest in decimal, as the engine spells numbers.
bool appendSbfStructOffset(StyledText &out, const NumberFormat &format,
                           std::int64_t value, unsigned bytes,
                           const StructNamer &structs) {
  const auto landing = structs(format.type, value + format.delta, bytes);
  if (!landing)
    return false;
  std::string name = format.type;
  if (!landing->path.empty())
    name += "." + landing->path;
  out.append(name, ListingRole::StructOffset);
  if (const std::int64_t rest = value - landing->offset) {
    out.append(rest < 0 ? "-" : "+", ListingRole::Punctuation);
    out.append(std::to_string(rest < 0 ? std::uint64_t(0) - std::uint64_t(rest)
                                       : std::uint64_t(rest)),
               ListingRole::Number);
  }
  return true;
}

} // namespace

std::optional<std::uint64_t> sbfImmediate(std::string_view mnemonic,
                                          const OperandTokens &tokens) {
  if (sbfOpaqueNumber(mnemonic))
    return std::nullopt;
  if (tokens.size() == 1 && tokens[0].kind == TokenKind::Number &&
      tokens[0].valid)
    return tokens[0].value;
  if (tokens.size() == 2 && isPunct(tokens[0], "-") &&
      tokens[1].kind == TokenKind::Number && tokens[1].valid)
    return ~tokens[1].value + 1;
  return std::nullopt;
}

std::optional<SbfAccess> sbfMemoryAccess(std::string_view mnemonic,
                                         const OperandTokens &tokens) {
  const auto address = sbfAddress(tokens);
  const unsigned bytes = sbfAccessBytes(mnemonic);
  if (!address || !bytes)
    return std::nullopt;
  return SbfAccess{std::string(address->base), address->displacement, bytes,
                   mnemonic.starts_with("st")};
}

bool sbfStructOperand(std::string_view mnemonic, const OperandTokens &tokens) {
  return sbfImmediate(mnemonic, tokens) || sbfAddress(tokens);
}

bool formatSbfOperand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const NumberFormat &format) {
  // A structure offset of a displacement: `[r1 + _T.Size]`.
  if (const auto address = sbfAddress(tokens)) {
    if (format.base != NumberBase::Struct || !facts.structs)
      return false;
    StyledText member;
    if (!appendSbfStructOffset(member, format, address->displacement,
                               sbfAccessBytes(facts.mnemonic), *facts.structs))
      return false;
    out.append("[", ListingRole::Punctuation);
    out.append(address->base, ListingRole::Register);
    out.append(" + ", ListingRole::Punctuation);
    out.append(member);
    out.append("]", ListingRole::Punctuation);
    return true;
  }
  const auto whole = sbfImmediate(facts.mnemonic, tokens);
  if (!whole)
    return false;
  // The instruction's width, or the encoding's 32 bits where a narrower
  // store cannot hold the number, so that another base never changes it.
  const bool constant = facts.mnemonic == "lddw";
  unsigned bits = constant ? 64 : sbfImmediateBits(facts.mnemonic);
  if (bits < 32 && (*whole >> bits) != 0 &&
      (*whole >> (bits - 1)) != (~std::uint64_t(0) >> (bits - 1)))
    bits = 32;
  const std::uint64_t mask =
      bits >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << bits) - 1;
  std::uint64_t shown = *whole & mask;
  std::string sign;
  if (format.negate) {
    shown = (~shown + 1) & mask;
    sign += '-';
  }
  if (format.invert) {
    shown = ~shown & mask;
    sign += '~';
  }
  const auto hex = [](std::uint64_t value) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string text;
    do {
      text.insert(text.begin(), Digits[value & 0xf]);
      value >>= 4;
    } while (value);
    return "0x" + text;
  };
  std::string text;
  switch (format.base) {
  case NumberBase::Number:
    text = constant ? hex(shown) : std::to_string(shown);
    break;
  case NumberBase::Hex:
    text = hex(shown);
    break;
  case NumberBase::Decimal:
    text = std::to_string(shown);
    break;
  case NumberBase::Binary:
    text = "0b" + binaryDigits(shown);
    break;
  case NumberBase::Char: {
    const auto characters = characterLiteral(shown);
    if (!characters)
      return false;
    text = *characters;
    break;
  }
  case NumberBase::Offset: {
    if (!sign.empty())
      return false;
    const LocationName name = namer(shown, NameUse::Address, {});
    if (name.text.empty())
      return false;
    out.append(name.text, name.role, name.address);
    return true;
  }
  case NumberBase::Enum: {
    if (!sign.empty() || !facts.enums)
      return false;
    const auto symbolic =
        (*facts.enums)(format.type, shown, bits, format.delta != 0);
    if (!symbolic)
      return false;
    out.append(*symbolic, ListingRole::Keyword);
    return true;
  }
  case NumberBase::Struct: {
    if (!sign.empty() || !facts.structs)
      return false;
    // The value signed in its width: a structure offset may lie before
    // where the register points.
    std::uint64_t extended = shown;
    if (bits < 64 && (shown >> (bits - 1)) & 1)
      extended |= ~mask;
    return appendSbfStructOffset(
        out, format, static_cast<std::int64_t>(extended), 0, *facts.structs);
  }
  }
  if (!sign.empty())
    out.append(sign, ListingRole::Punctuation);
  out.append(text, ListingRole::Number);
  return true;
}

} // namespace neverd::worker::operand_format_detail
