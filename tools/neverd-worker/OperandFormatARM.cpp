//===- OperandFormatARM.cpp - ARM-family operand formats -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The user's number formats in ARM, Thumb and AArch64 operands, spelled as
/// the decoder spells those operands: an immediate `#0x1a`, a displacement
/// `[x20, #0x10]`. OperandFormat.cpp spells every other operand.
///
//===----------------------------------------------------------------------===//

#include "OperandFormatDetail.h"

#include <array>
#include <cctype>
#include <charconv>

namespace neverd::worker::operand_format_detail {
namespace {

/// The decoder's numeric spelling: decimal below ten, otherwise lowercase
/// hexadecimal after `0x`.
std::string armNumber(std::uint64_t value) {
  if (value < 10)
    return std::to_string(value);
  std::array<char, 16> digits{};
  const auto end =
      std::to_chars(digits.data(), digits.data() + digits.size(), value, 16)
          .ptr;
  return "0x" + std::string(digits.data(), end);
}

std::string lowercase(std::string_view text) {
  std::string result(text);
  for (char &c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

bool isPunct(const Token &token, std::string_view text) {
  return token.kind == TokenKind::Punct && token.text == text;
}

/// A memory operand `[base]`, `[base, #disp]` or `[base, #disp]!` of a base
/// register other than the program counter, whose displacement locates
/// nothing by itself.
struct ArmAddress {
  Token base;
  std::int64_t displacement = 0;
  bool writeback = false;
};

std::optional<ArmAddress> armAddress(const OperandTokens &tokens,
                                     OperandDialect dialect) {
  OperandTokens solid;
  for (const Token &token : tokens)
    if (token.kind != TokenKind::Space)
      solid.push_back(token);
  if (solid.size() < 3 || !isPunct(solid[0], "[") ||
      solid[1].kind != TokenKind::Identifier ||
      !isRegisterName(dialect, solid[1].text))
    return std::nullopt;
  const auto base = lowercase(solid[1].text);
  if (base == "pc" || base == "r15")
    return std::nullopt;
  ArmAddress address{solid[1]};
  std::size_t at = 2;
  if (isPunct(solid[at], ",")) {
    if (at + 2 >= solid.size() || !isPunct(solid[at + 1], "#"))
      return std::nullopt;
    at += 2;
    const bool negative = isPunct(solid[at], "-");
    if (negative)
      ++at;
    if (at >= solid.size() || solid[at].kind != TokenKind::Number ||
        !solid[at].valid)
      return std::nullopt;
    address.displacement = static_cast<std::int64_t>(solid[at].value);
    if (negative)
      address.displacement = -address.displacement;
    ++at;
  }
  if (at >= solid.size() || !isPunct(solid[at], "]"))
    return std::nullopt;
  if (++at < solid.size() && isPunct(solid[at], "!")) {
    address.writeback = true;
    ++at;
  }
  if (at != solid.size())
    return std::nullopt;
  return address;
}

/// Bytes register \p name holds, or zero for a vector list or an unknown
/// register.
unsigned registerBytes(OperandDialect dialect, std::string_view name) {
  if (name.empty())
    return 0;
  const bool numbered =
      name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1]));
  if (dialect == OperandDialect::ARM) {
    // VFP and NEON registers by their letter; core registers 32 bits.
    if (numbered && name.front() == 'd')
      return 8;
    if (numbered && name.front() == 'q')
      return 16;
    return 4;
  }
  if (name == "sp" || name == "fp" || name == "lr" || name == "xzr")
    return 8;
  if (name == "wsp" || name == "wzr")
    return 4;
  if (!numbered)
    return 0;
  switch (name.front()) {
  case 'b':
    return 1;
  case 'h':
    return 2;
  case 'w':
  case 's':
    return 4;
  case 'x':
  case 'd':
    return 8;
  case 'q':
    return 16;
  default:
    return 0;
  }
}

/// The access size an ARM load, store or swap mnemonic states, past its
/// condition (`ldrbeq`, and `ldrhs` as `ldr` taken if higher or same): 1 or
/// 2 bytes, or zero when its register sets it. A doubleword pair moves each
/// register's 4 bytes, as an AArch64 pair counts each register.
unsigned armMnemonicBytes(std::string_view mnemonic) {
  static constexpr std::string_view Conditions[] = {
      "eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs",
      "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"};
  struct Sized {
    std::string_view mnemonic;
    unsigned bytes;
  };
  static constexpr Sized Mnemonics[] = {
      {"ldr", 0},    {"str", 0},    {"ldrt", 0},   {"strt", 0},   {"ldrex", 0},
      {"strex", 0},  {"lda", 0},    {"stl", 0},    {"ldaex", 0},  {"stlex", 0},
      {"vldr", 0},   {"vstr", 0},   {"ldrd", 0},   {"strd", 0},   {"ldrexd", 0},
      {"strexd", 0}, {"ldaexd", 0}, {"stlexd", 0}, {"swp", 0},    {"ldrb", 1},
      {"strb", 1},   {"ldrsb", 1},  {"ldrbt", 1},  {"strbt", 1},  {"ldrsbt", 1},
      {"ldrexb", 1}, {"strexb", 1}, {"ldab", 1},   {"stlb", 1},   {"ldaexb", 1},
      {"stlexb", 1}, {"swpb", 1},   {"ldrh", 2},   {"strh", 2},   {"ldrsh", 2},
      {"ldrht", 2},  {"strht", 2},  {"ldrsht", 2}, {"ldrexh", 2}, {"strexh", 2},
      {"ldah", 2},   {"stlh", 2},   {"ldaexh", 2}, {"stlexh", 2}};
  const auto known = [&](std::string_view name) -> std::optional<unsigned> {
    for (const Sized &entry : Mnemonics)
      if (entry.mnemonic == name)
        return entry.bytes;
    return std::nullopt;
  };
  if (const auto bytes = known(mnemonic))
    return *bytes;
  for (const std::string_view condition : Conditions)
    if (mnemonic.size() > condition.size() && mnemonic.ends_with(condition))
      if (const auto bytes =
              known(mnemonic.substr(0, mnemonic.size() - condition.size())))
        return *bytes;
  return 0;
}

/// The access size an AArch64 load or store mnemonic states: a `b`, `h` or
/// `sw` size suffix; zero when its first register sets it.
unsigned aarch64MnemonicBytes(std::string_view mnemonic) {
  // The pointer-authenticating loads name a key, not a size.
  if (mnemonic == "ldraa" || mnemonic == "ldrab")
    return 0;
  if (mnemonic.ends_with("sw"))
    return 4;
  if (mnemonic.ends_with('b'))
    return 1;
  if (mnemonic.ends_with('h'))
    return 2;
  return 0;
}

/// Appends \p value as a structure offset into \p format's type, where an
/// access of \p bytes bytes (none for an address) begins: `_T.Process`, with
/// what lies past the member's start (`_T.List+4`). False when no member
/// holds it.
bool appendArmStructOffset(StyledText &out, const NumberFormat &format,
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
    out.append(armNumber(rest < 0 ? std::uint64_t(0) - std::uint64_t(rest)
                                  : std::uint64_t(rest)),
               ListingRole::Number);
  }
  return true;
}

/// Appends immediate \p value, \p bits wide, as \p format shows it: `#0x1a`,
/// `#-0x10`, `#~0xf`, `#0b11010`, `#'A'`, `#NAME`, `#_T.Size` or, for an
/// offset, the bare name the listing gives a location. False when the format
/// cannot show the value.
bool appendArmNumber(StyledText &out, std::uint64_t value, unsigned bits,
                     const NumberFormat &format, const LocationNamer &namer,
                     const StructNamer *structs,
                     const OperandFacts::EnumNamer *enums) {
  const std::uint64_t mask =
      bits >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << bits) - 1;
  std::uint64_t shown = value & mask;
  std::string sign;
  if (format.negate) {
    shown = (~shown + 1) & mask;
    sign += '-';
  }
  if (format.invert) {
    shown = ~shown & mask;
    sign += '~';
  }
  std::string text;
  switch (format.base) {
  case NumberBase::Number:
  case NumberBase::Hex:
    text = armNumber(shown);
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
    if (!sign.empty() || !enums)
      return false;
    const auto symbolic = (*enums)(format.type, shown, bits, format.delta != 0);
    if (!symbolic)
      return false;
    out.append("#", ListingRole::Number);
    out.append(*symbolic, ListingRole::Keyword);
    return true;
  }
  case NumberBase::Struct: {
    if (!sign.empty() || !structs)
      return false;
    // The value signed in its width: a structure offset may lie before
    // where the register points.
    std::uint64_t extended = shown;
    if (bits < 64 && bits && (shown >> (bits - 1)) & 1)
      extended |= ~mask;
    StyledText member;
    if (!appendArmStructOffset(
            member, format, static_cast<std::int64_t>(extended), 0, *structs))
      return false;
    out.append("#", ListingRole::Number);
    out.append(member);
    return true;
  }
  }
  out.append("#", ListingRole::Number);
  if (!sign.empty())
    out.append(sign, ListingRole::Punctuation);
  out.append(text, ListingRole::Number);
  return true;
}
} // namespace

std::optional<std::uint64_t> armImmediate(const OperandTokens &tokens,
                                          std::uint64_t mask) {
  if (tokens.empty() || !isPunct(tokens.front(), "#"))
    return std::nullopt;
  const bool negative = tokens.size() == 3 && isPunct(tokens[1], "-");
  if (tokens.size() != (negative ? 3u : 2u))
    return std::nullopt;
  const Token &number = tokens.back();
  if (number.kind != TokenKind::Number || !number.valid)
    return std::nullopt;
  return (negative ? ~number.value + 1 : number.value) & mask;
}

bool armStructOperand(const OperandTokens &tokens, OperandDialect dialect) {
  return armImmediate(tokens, ~std::uint64_t(0)).has_value() ||
         armAddress(tokens, dialect).has_value();
}

ArmWidths armWidths(OperandDialect dialect, std::string_view mnemonic,
                    const std::vector<OperandTokens> &operands) {
  // The first two registers operands state outside a memory operand's
  // brackets, as the decoder lists register operands, and whether the first
  // lies in a register list.
  std::vector<std::string> registers;
  bool listed = false;
  for (const auto &operand : operands) {
    int memory = 0, list = 0;
    for (const Token &token : operand) {
      if (isPunct(token, "["))
        ++memory;
      else if (isPunct(token, "]"))
        --memory;
      else if (isPunct(token, "{"))
        ++list;
      else if (isPunct(token, "}"))
        --list;
      else if (!memory && registers.size() < 2 &&
               token.kind == TokenKind::Identifier &&
               isRegisterName(dialect, token.text)) {
        if (registers.empty())
          listed = list > 0;
        registers.push_back(lowercase(token.text));
      }
    }
  }
  const std::string first = registers.empty() ? std::string() : registers[0];
  // The mnemonic and its qualifier: a vector element type (`vmov.i64`) or a
  // width (`ldr.w`).
  const auto dot = mnemonic.find('.');
  const auto name = lowercase(mnemonic.substr(0, dot));
  const auto qualifier = dot == std::string_view::npos
                             ? std::string()
                             : lowercase(mnemonic.substr(dot + 1));
  ArmWidths widths;
  // AArch64 counts an immediate in its first register's width, 32 bits for a
  // `w` register and 64 otherwise; ARM and Thumb in 32 bits, 64 for a 64-bit
  // integer vector element. Their native operands count alike.
  if (dialect == OperandDialect::AArch64)
    widths.immediateBits = first.starts_with('w') ? 32 : 64;
  else
    widths.immediateBits =
        qualifier == "i64" || qualifier == "s64" || qualifier == "u64" ? 64
                                                                       : 32;
  // A load, store, swap or compare-and-swap moves what its mnemonic's size
  // says, else what its data register holds: the first register, but the
  // second of an exclusive store, whose first takes its status. A register
  // list's elements have no single size; anything else only computes an
  // address.
  const bool vector = dialect == OperandDialect::ARM && name.starts_with('v');
  const std::string_view access = std::string_view(name).substr(vector ? 1 : 0);
  if (!access.starts_with("ld") && !access.starts_with("st") &&
      !access.starts_with("swp") &&
      !(dialect == OperandDialect::AArch64 && access.starts_with("cas")))
    return widths;
  const unsigned stated = dialect == OperandDialect::ARM
                              ? armMnemonicBytes(name)
                              : aarch64MnemonicBytes(name);
  if (stated) {
    widths.accessBytes = stated;
    return widths;
  }
  if (listed)
    return widths;
  const bool exclusiveStore =
      dialect == OperandDialect::AArch64 &&
      (name == "stxr" || name == "stlxr" || name == "stxp" || name == "stlxp");
  widths.accessBytes = registerBytes(
      dialect,
      exclusiveStore ? (registers.size() > 1 ? registers[1] : "") : first);
  return widths;
}

bool armShift(const OperandTokens &tokens) {
  if (tokens.empty() || tokens.front().kind != TokenKind::Identifier)
    return false;
  const auto name = lowercase(tokens.front().text);
  return name == "lsl" || name == "lsr" || name == "asr" || name == "ror" ||
         name == "msl";
}

bool formatArmOperand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const NumberFormat &format, const ArmWidths &widths,
                      bool shifted) {
  // A shifted immediate is a field of a larger number: no member or location
  // stands for it.
  if (shifted &&
      (format.base == NumberBase::Struct || format.base == NumberBase::Offset))
    return false;
  // A structure offset the user gives a displacement from a register:
  // `[x20, #_T.List.Flink]`, `[sp, #_T.Size]!`.
  if (format.base == NumberBase::Struct && facts.structs)
    if (const auto address = armAddress(tokens, facts.dialect)) {
      StyledText member;
      if (appendArmStructOffset(member, format, address->displacement,
                                widths.accessBytes, *facts.structs)) {
        out.append("[", ListingRole::Punctuation);
        out.append(address->base.text, ListingRole::Register);
        out.append(", ", ListingRole::Punctuation);
        out.append("#", ListingRole::Number);
        out.append(member);
        out.append("]", ListingRole::Punctuation);
        if (address->writeback)
          out.append("!", ListingRole::Punctuation);
        return true;
      }
    }
  const auto whole = armImmediate(tokens, ~std::uint64_t(0));
  if (!whole)
    return false;
  // The instruction's width, or wider when the immediate spells more bits,
  // so that another base never changes the value.
  unsigned bits = widths.immediateBits ? widths.immediateBits : 64;
  if (bits < 64 && (*whole >> bits) != 0 &&
      (*whole >> (bits - 1)) != (~std::uint64_t(0) >> (bits - 1)))
    bits = 64;
  const std::uint64_t mask =
      bits >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << bits) - 1;
  return appendArmNumber(out, *whole & mask, bits, format, namer, facts.structs,
                         facts.enums);
}

} // namespace neverd::worker::operand_format_detail
