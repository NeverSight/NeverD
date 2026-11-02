#include "OperandFormat.h"

#include "OperandFormatDetail.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace neverd::worker {
namespace operand_format_detail {

std::string binaryDigits(std::uint64_t value) {
  std::string text;
  for (std::uint64_t bit = std::uint64_t(1) << 63; bit; bit >>= 1)
    if (!text.empty() || (value & bit))
      text += value & bit ? '1' : '0';
  return text.empty() ? std::string("0") : text;
}

std::optional<std::string> characterLiteral(std::uint64_t value) {
  // Most significant byte first, as the characters read in the source.
  std::string text;
  for (int shift = 56; shift >= 0; shift -= 8) {
    const auto byte = static_cast<unsigned char>(value >> shift);
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

} // namespace operand_format_detail

namespace {
using operand_format_detail::binaryDigits;
using operand_format_detail::characterLiteral;
using operand_format_detail::OperandTokens;
using operand_format_detail::Token;
using operand_format_detail::TokenKind;

struct RegisterTable {
  std::unordered_set<std::string> x86, aarch64, arm, sbf;
};

void addRange(std::unordered_set<std::string> &set, std::string_view prefix,
              int first, int last, std::string_view suffix) {
  for (int index = first; index <= last; ++index)
    set.insert(std::string(prefix) + std::to_string(index) +
               std::string(suffix));
}

const RegisterTable &registerTable() {
  static const RegisterTable table = [] {
    RegisterTable result;
    const auto family =
        [&](std::string_view name) -> std::unordered_set<std::string> & {
      if (name == "X86")
        return result.x86;
      if (name == "AArch64")
        return result.aarch64;
      if (name == "SBF")
        return result.sbf;
      return result.arm;
    };
#define NEVERD_REGISTER_NAME(Family, Name) family(#Family).insert(Name);
#define NEVERD_REGISTER_RANGE(Family, Prefix, First, Last, Suffix)             \
  addRange(family(#Family), Prefix, First, Last, Suffix);
#include "ListingVocabulary.def"
    return result;
  }();
  return table;
}

bool isIdentifierStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
         c == '%' || c == '@' || c == '?';
}
bool isIdentifierPart(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
         c == '@' || c == '?' || c == '.';
}
std::string lower(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

std::vector<Token> tokenize(std::string_view text) {
  std::vector<Token> tokens;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    const std::size_t start = i;
    Token token;
    if (std::isspace(static_cast<unsigned char>(c))) {
      while (i < text.size() &&
             std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
      token.kind = TokenKind::Space;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      token.kind = TokenKind::Number;
      int base = 10;
      std::size_t digits = i;
      if (c == '0' && i + 1 < text.size() &&
          (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        base = 16;
        digits = i + 2;
      }
      i = digits;
      while (i < text.size() &&
             std::isxdigit(static_cast<unsigned char>(text[i])) &&
             (base == 16 || std::isdigit(static_cast<unsigned char>(text[i]))))
        ++i;
      const auto parsed = std::from_chars(text.data() + digits, text.data() + i,
                                          token.value, base);
      token.valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + i &&
                    i > digits;
      // A digit run fused to letters (an AArch64 arrangement such as `16b`)
      // is one identifier, not a number.
      if (i < text.size() && isIdentifierPart(text[i]) && text[i] != '.') {
        while (i < text.size() && isIdentifierPart(text[i]))
          ++i;
        token.kind = TokenKind::Identifier;
        token.valid = false;
      }
    } else if (isIdentifierStart(c)) {
      token.kind = TokenKind::Identifier;
      while (i < text.size() && isIdentifierPart(text[i]))
        ++i;
    } else {
      token.kind = TokenKind::Punct;
      ++i;
    }
    token.text = text.substr(start, i - start);
    tokens.push_back(token);
  }
  return tokens;
}

ListingRole identifierRole(OperandDialect dialect, std::string_view text) {
  if (isRegisterName(dialect, text))
    return ListingRole::Register;
  if (isOperandKeyword(lower(text)))
    return ListingRole::Keyword;
  return ListingRole::Plain;
}

bool isTransfer(std::string_view flow) {
  return flow == "call" || flow == "jump" || flow == "cjump";
}

/// Classic operand forms of one x86 instruction (NEVERD_X86_OPERAND_FORM).
enum X86OperandForm : unsigned {
  FoldRepeatedSource = 1u << 0,
  SwapRegisters = 1u << 1,
  SignedByte = 1u << 2,
  StackWidth = 1u << 3,
};

unsigned x86OperandForms(std::string_view mnemonic) {
  static const std::unordered_map<std::string_view, unsigned> forms = [] {
    std::unordered_map<std::string_view, unsigned> result;
#define NEVERD_X86_OPERAND_FORM(Mnemonic, Form) result[Mnemonic] |= Form;
#include "ListingVocabulary.def"
    return result;
  }();
  const auto it = forms.find(mnemonic);
  return it == forms.end() ? 0 : it->second;
}

/// The size keyword a general purpose register implies, or empty.
std::string_view x86RegisterSize(std::string_view name) {
  static const std::unordered_map<std::string, std::string_view> sizes = [] {
    std::unordered_map<std::string, std::string_view> result;
#define NEVERD_X86_REGISTER_SIZE(Keyword, Name) result.emplace(Name, Keyword);
#define NEVERD_X86_REGISTER_SIZE_RANGE(Keyword, Prefix, First, Last, Suffix)   \
  for (int index = First; index <= Last; ++index)                              \
    result.emplace(std::string(Prefix) + std::to_string(index) + Suffix,       \
                   Keyword);
#include "ListingVocabulary.def"
    return result;
  }();
  const auto it = sizes.find(lower(name));
  return it == sizes.end() ? std::string_view() : it->second;
}

unsigned sizeBits(std::string_view keyword) {
#define NEVERD_X86_SIZE_BITS(Keyword, Bits)                                    \
  if (keyword == Keyword)                                                      \
    return Bits;
#include "ListingVocabulary.def"
  return 0;
}

bool isDisplacedBase(std::string_view name) {
  static const std::unordered_set<std::string_view> bases = {
#define NEVERD_X86_DISPLACED_BASE(Name) Name,
#include "ListingVocabulary.def"
  };
  return bases.contains(lower(name));
}

bool isThreadSegment(std::string_view name) {
  static const std::unordered_set<std::string_view> segments = {
#define NEVERD_X86_THREAD_SEGMENT(Name) Name,
#include "ListingVocabulary.def"
  };
  return segments.contains(name);
}

/// Width in bytes of the zero displacement a multi-byte no-op encodes in its
/// ModRM byte, or zero.
unsigned noOpDisplacementBytes(std::string_view bytes) {
  // ModRM.mod: 01 adds a byte of displacement, 10 four bytes.
  constexpr unsigned Displacement8 = 1, Displacement32 = 2;
  std::vector<unsigned char> encoding;
  for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
    unsigned value = 0;
    const auto parsed =
        std::from_chars(bytes.data() + i, bytes.data() + i + 2, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.data() + i + 2)
      return 0;
    encoding.push_back(static_cast<unsigned char>(value));
  }
  unsigned char escape = 0, opcode = 0;
#define NEVERD_X86_MULTIBYTE_NOP(Escape, Opcode)                               \
  escape = Escape;                                                             \
  opcode = Opcode;
#include "ListingVocabulary.def"
  const auto at = std::find(encoding.begin(), encoding.end(), escape);
  if (std::distance(at, encoding.end()) < 3 || at[1] != opcode)
    return 0;
  const unsigned mod = at[2] >> 6;
  return mod == Displacement8 ? 1 : mod == Displacement32 ? 4 : 0;
}

/// Facts an x86 operand needs about the other operands of its instruction.
struct X86Context {
  /// Size keywords that register operands state.
  std::vector<std::string_view> registerSizes;
  /// Width of a negative immediate shown unsigned, or zero to keep its sign.
  unsigned immediateBits = 0;
  /// A negative immediate that fits a signed byte keeps its sign.
  bool signedByte = false;
};

/// The size keyword in front of a memory operand, or empty.
std::string memorySize(const std::vector<Token> &tokens) {
  for (const Token &token : tokens) {
    if (token.kind == TokenKind::Punct && token.text == "[")
      break;
    if (token.kind == TokenKind::Identifier)
      if (auto word = lower(token.text); !dataNamePrefix(word).empty())
        return word;
  }
  return {};
}

bool isLoneRegister(const std::vector<Token> &operand) {
  return operand.size() == 1 && operand.front().kind == TokenKind::Identifier &&
         isRegisterName(OperandDialect::X86, operand.front().text);
}

/// The value of an operand that is a plain number, `5` or `-5`, unsigned in
/// \p mask's width.
std::optional<std::uint64_t> plainNumber(const OperandTokens &tokens,
                                         std::uint64_t mask) {
  if (tokens.size() == 1 && tokens[0].kind == TokenKind::Number &&
      tokens[0].valid)
    return tokens[0].value & mask;
  if (tokens.size() == 2 && tokens[0].kind == TokenKind::Punct &&
      tokens[0].text == "-" && tokens[1].kind == TokenKind::Number &&
      tokens[1].valid)
    return (~tokens[1].value + 1) & mask;
  return std::nullopt;
}

std::vector<OperandTokens> splitOperands(const std::vector<Token> &tokens) {
  std::vector<OperandTokens> operands(1);
  int depth = 0;
  for (const Token &token : tokens) {
    if (token.kind == TokenKind::Punct) {
      if (token.text == "[" || token.text == "{" || token.text == "(")
        ++depth;
      else if ((token.text == "]" || token.text == "}" || token.text == ")") &&
               depth > 0)
        --depth;
      else if (token.text == "," && depth == 0) {
        operands.emplace_back();
        continue;
      }
    }
    operands.back().push_back(token);
  }
  for (auto &operand : operands) {
    while (!operand.empty() && operand.front().kind == TokenKind::Space)
      operand.erase(operand.begin());
    while (!operand.empty() && operand.back().kind == TokenKind::Space)
      operand.pop_back();
  }
  return operands;
}

void appendX86Number(StyledText &out, const Token &token) {
  if (token.valid)
    out.append(x86Number(token.value), ListingRole::Number);
  else
    out.append(token.text, ListingRole::Number);
}

/// Effective address of an x86 memory operand that names one fixed location:
/// a RIP/EIP base without index, or a bare absolute displacement.
std::optional<std::uint64_t> fixedX86Location(const std::vector<Token> &inner,
                                              const OperandFacts &facts,
                                              bool &pcRelative) {
  std::string base;
  int registers = 0;
  std::uint64_t displacement = 0;
  bool negative = false, sawNumber = false;
  for (const Token &token : inner) {
    if (token.kind == TokenKind::Identifier) {
      if (!isRegisterName(OperandDialect::X86, token.text))
        return std::nullopt;
      base = lower(token.text);
      ++registers;
    } else if (token.kind == TokenKind::Number) {
      if (!token.valid || sawNumber)
        return std::nullopt;
      displacement = token.value;
      sawNumber = true;
    } else if (token.kind == TokenKind::Punct) {
      if (token.text == "-")
        negative = true;
      else if (token.text != "+")
        return std::nullopt;
    }
  }
  const std::uint64_t signedDisplacement =
      negative ? ~displacement + 1 : displacement;
  const std::uint64_t next = facts.address + facts.size;
  if (registers == 1 && (base == "rip" || base == "eip")) {
    pcRelative = true;
    std::uint64_t location = next + signedDisplacement;
    if (base == "eip")
      location &= 0xffffffffu;
    return location;
  }
  if (registers == 0 && sawNumber && !negative) {
    pcRelative = false;
    return displacement;
  }
  return std::nullopt;
}

/// Registers and displacement inside the brackets of an x86 memory operand.
struct X86Address {
  std::vector<Token> tokens;
  /// Positions in tokens; an index register is followed by its scale.
  std::optional<std::size_t> base, index;
  bool displaced = false;
  std::int64_t displacement = 0;
};

X86Address parseX86Address(const std::vector<Token> &inner) {
  X86Address address;
  for (const Token &token : inner)
    if (token.kind != TokenKind::Space)
      address.tokens.push_back(token);
  const auto &tokens = address.tokens;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token &token = tokens[i];
    const bool scale = i > 0 && tokens[i - 1].text == "*";
    if (token.kind == TokenKind::Number && !scale) {
      address.displaced = true;
      const auto value = static_cast<std::int64_t>(token.value);
      address.displacement +=
          i > 0 && tokens[i - 1].text == "-" ? -value : value;
    }
    if (token.kind != TokenKind::Identifier ||
        !isRegisterName(OperandDialect::X86, token.text))
      continue;
    // An index is followed by its scale, or is the second register.
    if ((i + 1 < tokens.size() && tokens[i + 1].text == "*") || address.base)
      address.index = i;
    else
      address.base = i;
  }
  return address;
}

void appendX86Token(StyledText &out, const Token &token) {
  if (token.kind == TokenKind::Space)
    out.append(" ", ListingRole::Plain);
  else if (token.kind == TokenKind::Identifier)
    out.append(token.text, identifierRole(OperandDialect::X86, token.text));
  else if (token.kind == TokenKind::Number)
    appendX86Number(out, token);
  else
    out.append(token.text, ListingRole::Punctuation);
}

/// Appends \p value, where an access of \p bytes bytes begins (none for an
/// address), as a structure offset into \p format's type:
/// `_KTHREAD.Process`, or `(_KAPC.Type-10h)` in parentheses when
/// \p grouped and the value lies off the member's start.  False when no
/// member holds it.
bool appendStructOffset(StyledText &out, const NumberFormat &format,
                        std::int64_t value, unsigned bytes,
                        const StructNamer &structs, bool grouped) {
  const auto landing = structs(format.type, value + format.delta, bytes);
  if (!landing)
    return false;
  std::string name = format.type;
  if (!landing->path.empty())
    name += "." + landing->path;
  // The member stands for where it begins in the type; the rest is what
  // the value adds to it, the register's delta taken back.
  const std::int64_t rest = value - landing->offset;
  const bool open = grouped && rest != 0;
  if (open)
    out.append("(", ListingRole::Punctuation);
  out.append(name, ListingRole::StructOffset);
  if (rest) {
    out.append(rest < 0 ? "-" : "+", ListingRole::Punctuation);
    out.append(x86Number(rest < 0 ? std::uint64_t(0) - std::uint64_t(rest)
                                  : std::uint64_t(rest)),
               ListingRole::Number);
  }
  if (open)
    out.append(")", ListingRole::Punctuation);
  return true;
}

/// Appends \p value, a number \p bits wide, as \p format shows it; false
/// when the format cannot show it (a character that does not print, an
/// offset no name denotes, a structure offset no member holds), so the
/// operand keeps the listing's spelling.
bool appendFormattedNumber(StyledText &out, std::uint64_t value, unsigned bits,
                           const NumberFormat &format,
                           const LocationNamer &namer,
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
    text = x86Number(shown);
    break;
  case NumberBase::Decimal:
    text = std::to_string(shown);
    break;
  case NumberBase::Binary:
    text = binaryDigits(shown) + "b";
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
    out.append("offset ", ListingRole::Keyword);
    out.append(name.text, name.role, name.address);
    return true;
  }
  case NumberBase::Enum: {
    if (!sign.empty() || !enums)
      return false;
    const auto symbolic = (*enums)(format.type, shown, bits, format.delta != 0);
    if (!symbolic)
      return false;
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
    return appendStructOffset(out, format, static_cast<std::int64_t>(extended),
                              0, *structs, false);
  }
  }
  if (!sign.empty())
    out.append(sign, ListingRole::Punctuation);
  out.append(text, ListingRole::Number);
  return true;
}

void formatX86Operand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const X86Context &context, const NumberFormat *format,
                      unsigned operandBits) {
  const auto open =
      std::find_if(tokens.begin(), tokens.end(), [](const Token &t) {
        return t.kind == TokenKind::Punct && t.text == "[";
      });
  if (open != tokens.end()) {
    const auto close = std::find_if(open, tokens.end(), [](const Token &t) {
      return t.kind == TokenKind::Punct && t.text == "]";
    });
    const std::vector<Token> prefix(tokens.begin(), open);
    const std::vector<Token> inner(
        open + 1, close == tokens.end() ? tokens.end() : close);
    const std::string sizeKeyword = memorySize(prefix);
    std::string segment;
    for (std::size_t i = 0; i + 1 < prefix.size(); ++i)
      if (prefix[i].kind == TokenKind::Identifier && prefix[i + 1].text == ":")
        segment = lower(prefix[i].text);
    // A register operand of the same size already states the size.
    const bool sizeStated =
        !sizeKeyword.empty() &&
        std::find(context.registerSizes.begin(), context.registerSizes.end(),
                  sizeKeyword) != context.registerSizes.end();
    const auto appendPrefix = [&](bool omitSize) {
      for (std::size_t i = 0; i < prefix.size(); ++i) {
        const Token &token = prefix[i];
        if (omitSize && token.kind == TokenKind::Identifier) {
          const auto word = lower(token.text);
          if (word == sizeKeyword || word == "ptr") {
            if (i + 1 < prefix.size() && prefix[i + 1].kind == TokenKind::Space)
              ++i;
            continue;
          }
        }
        appendX86Token(out, token);
      }
    };
    const auto appendSuffix = [&] {
      if (close == tokens.end())
        return;
      out.append("]", ListingRole::Punctuation);
      for (auto it = close + 1; it != tokens.end(); ++it)
        if (it->kind != TokenKind::Space)
          appendX86Token(out, *it);
    };
    const X86Address parsed = parseX86Address(inner);
    const auto &compact = parsed.tokens;
    // Thread storage is addressed by offset, never by an image location.
    if (isThreadSegment(segment) && compact.size() == 1 &&
        compact.front().kind == TokenKind::Number) {
      appendPrefix(sizeStated);
      appendX86Number(out, compact.front());
      return;
    }
    bool pcRelative = false;
    const auto location = isThreadSegment(segment)
                              ? std::nullopt
                              : fixedX86Location(inner, facts, pcRelative);
    if (location) {
      const bool address = facts.mnemonic == "lea";
      const NameUse use = address ? NameUse::Address
                          : (facts.flow == "icall" || facts.flow == "ijump")
                              ? NameUse::Slot
                              : NameUse::Data;
      const LocationName name = namer(*location, use, sizeKeyword);
      if (!name.text.empty()) {
        if (!address) {
          out.append(segment.empty() ? (pcRelative ? "cs" : "ds") : segment,
                     ListingRole::Register);
          out.append(":", ListingRole::Punctuation);
        }
        out.append(name.text, name.role, name.address);
        return;
      }
    }
    const auto appendIndex = [&] {
      const std::size_t index = *parsed.index;
      appendX86Token(out, compact[index]);
      if (index + 2 < compact.size() && compact[index + 1].text == "*") {
        appendX86Token(out, compact[index + 1]);
        appendX86Token(out, compact[index + 2]);
      }
    };
    // A structure offset the user gives a register's displacement:
    // `[rcx+_KTHREAD.Process]`, `[rdx+rax*8+(_FOO.Items+8)]`.  The access
    // reads as many bytes as its size keyword or register says, and `lea`
    // reads none.
    if (format && format->base == NumberBase::Struct && facts.structs &&
        parsed.base) {
      unsigned bytes = 0;
      if (facts.mnemonic != "lea") {
        bytes = sizeBits(sizeKeyword) / 8;
        if (!bytes && context.registerSizes.size() == 1)
          bytes = sizeBits(context.registerSizes.front()) / 8;
      }
      StyledText member;
      if (appendStructOffset(member, *format, parsed.displacement, bytes,
                             *facts.structs, true)) {
        appendPrefix(sizeStated);
        out.append("[", ListingRole::Punctuation);
        appendX86Token(out, compact[*parsed.base]);
        if (parsed.index) {
          out.append("+", ListingRole::Punctuation);
          appendIndex();
        }
        out.append("+", ListingRole::Punctuation);
        out.append(member);
        appendSuffix();
        return;
      }
    }
    // A variable of the function's frame: `[rbp+rax*4+var_290]`, or through
    // the stack register `[rsp+48h+var_30]`, which names its distance below
    // the frame's base.  The variable's type states the size unless the
    // access differs from it.
    const auto baseName =
        parsed.base ? lower(compact[*parsed.base].text) : std::string();
    const bool viaFrame =
        !facts.frameRegister.empty() && baseName == facts.frameRegister;
    const bool viaStack = facts.stackDepth && !facts.stackRegister.empty() &&
                          baseName == facts.stackRegister;
    const auto depth = viaStack ? *facts.stackDepth : facts.frameDepth;
    if (facts.frame && (viaFrame || viaStack))
      if (const auto slot = (*facts.frame)(parsed.displacement - depth)) {
        appendPrefix(slot->delta == 0 &&
                     sizeBits(sizeKeyword) == slot->size * 8);
        out.append("[", ListingRole::Punctuation);
        appendX86Token(out, compact[*parsed.base]);
        if (parsed.index) {
          out.append("+", ListingRole::Punctuation);
          appendIndex();
        }
        if (depth) {
          out.append(depth < 0 ? "-" : "+", ListingRole::Punctuation);
          out.append(
              x86Number(static_cast<std::uint64_t>(depth < 0 ? -depth : depth)),
              ListingRole::Number);
        }
        out.append("+", ListingRole::Punctuation);
        out.append(slot->name, ListingRole::Plain);
        if (slot->delta) {
          out.append("+", ListingRole::Punctuation);
          out.append(x86Number(static_cast<std::uint64_t>(slot->delta)),
                     ListingRole::Number);
        }
        appendSuffix();
        return;
      }
    appendPrefix(sizeStated);
    if (parsed.index && !parsed.base) {
      // Without a base register the displacement leads: `ds:1[rsi*2]`.
      std::uint64_t displacement =
          static_cast<std::uint64_t>(parsed.displacement);
      if (!facts.wide)
        displacement &= 0xffffffffu;
      if (segment.empty()) {
        out.append("ds", ListingRole::Register);
        out.append(":", ListingRole::Punctuation);
      }
      // A zero displacement only scales the index (`ds:0[rdx*8]`).
      const LocationName name =
          displacement ? namer(displacement,
                               facts.mnemonic == "lea" ? NameUse::Address
                                                       : NameUse::Data,
                               sizeKeyword)
                       : LocationName{};
      if (!name.text.empty())
        out.append(name.text, name.role, name.address);
      else
        out.append(x86Number(displacement), ListingRole::Number);
      out.append("[", ListingRole::Punctuation);
      appendIndex();
      appendSuffix();
      return;
    }
    out.append("[", ListingRole::Punctuation);
    for (const Token &token : compact)
      appendX86Token(out, token);
    if (!parsed.displaced) {
      // A displacement the encoding carries is shown even when zero: as wide
      // as encoded in a no-op, as `+0` after a base that requires one.
      if (const unsigned width = noOpDisplacementBytes(facts.bytes)) {
        out.append("+", ListingRole::Punctuation);
        out.append(std::string(width * 2, '0') + "h", ListingRole::Number);
      } else if (parsed.base && isDisplacedBase(compact[*parsed.base].text)) {
        out.append("+", ListingRole::Punctuation);
        out.append("0", ListingRole::Number);
      }
    }
    appendSuffix();
    return;
  }
  // An immediate the user formats: its value, unsigned in the operation's
  // width.
  if (format && !format->isDefault()) {
    const unsigned bits = format->base == NumberBase::Enum ? operandBits
                          : context.immediateBits ? context.immediateBits
                                                  : 64;
    if (bits && bits <= 64) {
      const std::uint64_t mask =
          bits == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << bits) - 1;
      if (const auto value = plainNumber(tokens, mask);
          value && appendFormattedNumber(out, *value, bits, *format, namer,
                                         facts.structs, facts.enums))
        return;
    }
  }
  // A negative immediate is the unsigned value of the operation's width,
  // unless the instruction keeps a signed byte.
  if (context.immediateBits && tokens.size() == 2 &&
      tokens[0].kind == TokenKind::Punct && tokens[0].text == "-" &&
      tokens[1].kind == TokenKind::Number && tokens[1].valid &&
      !(context.signedByte && tokens[1].value <= 0x80)) {
    const std::uint64_t mask =
        context.immediateBits >= 64
            ? ~std::uint64_t(0)
            : (std::uint64_t(1) << context.immediateBits) - 1;
    out.append(x86Number((~tokens[1].value + 1) & mask), ListingRole::Number);
    return;
  }
  for (const Token &token : tokens) {
    switch (token.kind) {
    case TokenKind::Space:
      out.append(" ", ListingRole::Plain);
      break;
    case TokenKind::Identifier:
      out.append(token.text, identifierRole(OperandDialect::X86, token.text));
      break;
    case TokenKind::Number:
      if (token.valid && facts.target && token.value == *facts.target &&
          isTransfer(facts.flow)) {
        if (facts.flow != "call" && facts.size == 2)
          out.append("short ", ListingRole::Keyword);
        const LocationName name = namer(token.value, NameUse::Transfer, {});
        if (!name.text.empty()) {
          out.append(name.text, name.role, name.address);
          break;
        }
      }
      appendX86Number(out, token);
      break;
    case TokenKind::Punct:
      out.append(token.text, ListingRole::Punctuation);
      break;
    }
  }
}

void formatGenericOperand(StyledText &out, const OperandTokens &tokens,
                          const OperandFacts &facts,
                          const LocationNamer &namer) {
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token &token = tokens[i];
    // `#0x1000` naming an exact transfer target or referenced location.
    if (token.kind == TokenKind::Punct && token.text == "#" &&
        i + 1 < tokens.size() && tokens[i + 1].kind == TokenKind::Number &&
        tokens[i + 1].valid) {
      const std::uint64_t value = tokens[i + 1].value;
      const bool transfer =
          facts.target && value == *facts.target && isTransfer(facts.flow);
      const bool referenced =
          std::find(facts.references.begin(), facts.references.end(), value) !=
          facts.references.end();
      if (transfer || referenced) {
        const LocationName name =
            namer(value, transfer ? NameUse::Transfer : NameUse::Data, {});
        if (!name.text.empty()) {
          out.append(name.text, name.role, name.address);
          ++i;
          continue;
        }
      }
    }
    switch (token.kind) {
    case TokenKind::Space:
      out.append(token.text, ListingRole::Plain);
      break;
    case TokenKind::Identifier:
      out.append(token.text, identifierRole(facts.dialect, token.text));
      break;
    case TokenKind::Number:
      if (token.valid && facts.target && token.value == *facts.target &&
          isTransfer(facts.flow)) {
        const LocationName name = namer(token.value, NameUse::Transfer, {});
        if (!name.text.empty()) {
          out.append(name.text, name.role, name.address);
          break;
        }
      }
      out.append(token.text, ListingRole::Number);
      break;
    case TokenKind::Punct:
      out.append(token.text, token.text == "#" ? ListingRole::Number
                                               : ListingRole::Punctuation);
      break;
    }
  }
}
} // namespace

using operand_format_detail::armDialect;
using operand_format_detail::armImmediate;
using operand_format_detail::armShift;
using operand_format_detail::armStructOperand;
using operand_format_detail::ArmWidths;
using operand_format_detail::armWidths;
using operand_format_detail::evmImmediate;
using operand_format_detail::evmPushData;
using operand_format_detail::formatArmOperand;
using operand_format_detail::formatEvmOperand;
using operand_format_detail::formatSbfOperand;
using operand_format_detail::sbfImmediate;
using operand_format_detail::sbfStructOperand;

bool showsNumberFormats(OperandDialect dialect) {
  // Other dialects keep the decoder's spelling.
  return dialect != OperandDialect::Generic;
}

std::vector<bool> formattableOperands(std::string_view operands,
                                      OperandDialect dialect,
                                      std::string_view mnemonic) {
  std::vector<bool> result;
  for (const auto &operand : splitOperands(tokenize(operands)))
    switch (dialect) {
    case OperandDialect::AArch64:
    case OperandDialect::ARM:
      result.push_back(armImmediate(operand, ~std::uint64_t(0)).has_value());
      break;
    case OperandDialect::SBF:
      result.push_back(sbfImmediate(mnemonic, operand).has_value());
      break;
    case OperandDialect::EVM:
      result.push_back(evmPushData(mnemonic, operand));
      break;
    case OperandDialect::X86:
      result.push_back(plainNumber(operand, ~std::uint64_t(0)).has_value());
      break;
    case OperandDialect::Generic:
      result.push_back(false);
      break;
    }
  return result;
}

std::vector<bool> structOperands(std::string_view operands,
                                 OperandDialect dialect,
                                 std::string_view mnemonic) {
  std::vector<bool> result;
  const auto pieces = splitOperands(tokenize(operands));
  for (std::size_t index = 0; index < pieces.size(); ++index) {
    const auto &operand = pieces[index];
    bool fits = false;
    if (dialect == OperandDialect::SBF) {
      fits = sbfStructOperand(mnemonic, operand);
    } else if (dialect == OperandDialect::EVM) {
      fits = evmImmediate(mnemonic, operand).has_value();
    } else if (armDialect(dialect)) {
      // A shifted immediate is a field of a larger number.
      fits = armStructOperand(operand, dialect) &&
             !(index + 1 < pieces.size() && armShift(pieces[index + 1]) &&
               armImmediate(operand, ~std::uint64_t(0)));
    } else if (showsNumberFormats(dialect)) {
      const auto open =
          std::find_if(operand.begin(), operand.end(), [](const Token &t) {
            return t.kind == TokenKind::Punct && t.text == "[";
          });
      if (open == operand.end()) {
        fits = plainNumber(operand, ~std::uint64_t(0)).has_value();
      } else {
        const auto close =
            std::find_if(open, operand.end(), [](const Token &t) {
              return t.kind == TokenKind::Punct && t.text == "]";
            });
        const std::vector<Token> inner(open + 1, close);
        const X86Address parsed = parseX86Address(inner);
        // A displacement from the instruction pointer is a location.
        if (parsed.base) {
          const auto base = lower(parsed.tokens[*parsed.base].text);
          fits = base != "rip" && base != "eip";
        }
      }
    }
    result.push_back(fits);
  }
  return result;
}

std::optional<std::int64_t> operandNumber(std::string_view operands,
                                          std::size_t index,
                                          OperandDialect dialect,
                                          std::string_view mnemonic) {
  const auto pieces = splitOperands(tokenize(operands));
  if (index >= pieces.size())
    return std::nullopt;
  const auto value =
      dialect == OperandDialect::SBF   ? sbfImmediate(mnemonic, pieces[index])
      : dialect == OperandDialect::EVM ? evmImmediate(mnemonic, pieces[index])
      : armDialect(dialect) ? armImmediate(pieces[index], ~std::uint64_t(0))
                            : plainNumber(pieces[index], ~std::uint64_t(0));
  if (!value)
    return std::nullopt;
  return static_cast<std::int64_t>(*value);
}

OperandDialect operandDialect(std::string_view architecture) {
  const auto name = lower(architecture);
  if (name == "x86_64" || name == "x86-64" || name == "x86" || name == "x64" ||
      name == "i386" || name == "amd64")
    return OperandDialect::X86;
  if (name == "aarch64" || name == "arm64")
    return OperandDialect::AArch64;
  if (name.starts_with("arm") || name.starts_with("thumb"))
    return OperandDialect::ARM;
  if (name == "sbf")
    return OperandDialect::SBF;
  if (name == "evm")
    return OperandDialect::EVM;
  return OperandDialect::Generic;
}

void StyledText::append(std::string_view text, ListingRole role,
                        std::optional<std::uint64_t> address) {
  if (text.empty())
    return;
  if (role != ListingRole::Plain || address) {
    if (!address && !spans_.empty() && !spans_.back().address &&
        spans_.back().role == role &&
        spans_.back().start + spans_.back().length == text_.size())
      spans_.back().length += static_cast<std::uint32_t>(text.size());
    else
      spans_.push_back({static_cast<std::uint32_t>(text_.size()),
                        static_cast<std::uint32_t>(text.size()), role,
                        address});
  }
  text_ += text;
  columns_ += displayColumns(text);
}

void StyledText::padTo(std::size_t column) {
  if (columns_ < column) {
    text_.append(column - columns_, ' ');
    columns_ = column;
  } else if (!text_.empty()) {
    text_ += ' ';
    ++columns_;
  }
}

void StyledText::append(const StyledText &other) {
  const auto offset = static_cast<std::uint32_t>(text_.size());
  text_ += other.text_;
  columns_ += other.columns_;
  for (TextSpan span : other.spans_) {
    span.start += offset;
    spans_.push_back(span);
  }
}

std::size_t displayColumns(std::string_view text) {
  std::size_t columns = 0;
  for (std::size_t at = 0; at < text.size();) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80) {
      ++columns;
      ++at;
      continue;
    }
    const unsigned length = (lead & 0xe0) == 0xc0   ? 2
                            : (lead & 0xf0) == 0xe0 ? 3
                            : (lead & 0xf8) == 0xf0 ? 4
                                                    : 1;
    std::uint32_t code = lead & (0x7f >> length);
    bool formed = length > 1 && at + length <= text.size();
    for (unsigned i = 1; formed && i < length; ++i) {
      const auto next = static_cast<unsigned char>(text[at + i]);
      formed = (next & 0xc0) == 0x80;
      code = code << 6 | (next & 0x3f);
    }
    if (!formed) {
      ++columns;
      ++at;
      continue;
    }
    unsigned width = 1;
#define NEVERD_WIDE_CHARACTERS(First, Last)                                    \
  if (code >= First && code <= Last)                                           \
    width = 2;
#include "neverd/support/WideCharacters.def"
    columns += width;
    at += length;
  }
  return columns;
}

std::string x86Number(std::uint64_t value) {
  if (value < 10)
    return std::to_string(value);
  char buffer[24];
  const auto end = std::to_chars(buffer, buffer + sizeof buffer, value, 16).ptr;
  std::string digits(buffer, end);
  std::transform(digits.begin(), digits.end(), digits.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (std::isalpha(static_cast<unsigned char>(digits.front())))
    digits.insert(digits.begin(), '0');
  return digits + "h";
}

StyledText formatOperands(std::string_view operands, const OperandFacts &facts,
                          const LocationNamer &namer) {
  StyledText out;
  const auto pieces = splitOperands(tokenize(operands));
  X86Context context;
  if (facts.dialect == OperandDialect::X86) {
    const unsigned forms = x86OperandForms(facts.mnemonic);
    context.signedByte = forms & SignedByte;
    for (const auto &operand : pieces)
      if (isLoneRegister(operand))
        if (const auto size = x86RegisterSize(operand.front().text);
            !size.empty())
          context.registerSizes.push_back(size);
    // The first operand sets the width of the operation.
    if (!pieces.empty())
      context.immediateBits =
          isLoneRegister(pieces.front())
              ? sizeBits(x86RegisterSize(pieces.front().front().text))
              : sizeBits(memorySize(pieces.front()));
    if (!context.immediateBits && (forms & StackWidth))
      context.immediateBits = facts.wide ? 64 : 32;
  }
  // The ARM family's widths matter only to an operand the user formats.
  const bool arm = armDialect(facts.dialect);
  ArmWidths widths;
  if (arm && facts.numberFormats)
    widths = armWidths(facts.dialect, facts.mnemonic, pieces);
  bool first = true;
  for (std::size_t index = 0; index < pieces.size(); ++index) {
    const auto &operand = pieces[index];
    if (operand.empty())
      continue;
    if (!first)
      out.append(", ", ListingRole::Punctuation);
    first = false;
    const NumberFormat *format =
        facts.numberFormats && index < facts.numberFormats->size()
            ? &(*facts.numberFormats)[index]
            : nullptr;
    if (facts.dialect == OperandDialect::X86)
      formatX86Operand(out, operand, facts, namer, context, format,
                       facts.operandBits && index < facts.operandBits->size()
                           ? (*facts.operandBits)[index]
                           : 0);
    else if (!format || format->isDefault() ||
             !(arm ? formatArmOperand(out, operand, facts, namer, *format,
                                      widths,
                                      index + 1 < pieces.size() &&
                                          armShift(pieces[index + 1]))
               : facts.dialect == OperandDialect::SBF
                   ? formatSbfOperand(out, operand, facts, namer, *format)
                   : facts.dialect == OperandDialect::EVM &&
                         formatEvmOperand(out, operand, facts, namer, *format)))
      formatGenericOperand(out, operand, facts, namer);
  }
  return out;
}

std::optional<NumberBase> parseNumberBase(std::string_view spelling) {
#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  if (spelling == Spelling)                                                    \
    return NumberBase::Id;
#include "neverd/OperandFormats.def"
  return std::nullopt;
}

std::string_view numberBaseName(NumberBase base) {
  switch (base) {
#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  case NumberBase::Id:                                                         \
    return Spelling;
#include "neverd/OperandFormats.def"
  }
  return {};
}

std::optional<FrameAccess> x86FrameAccess(std::string_view operands,
                                          std::string_view frameRegister) {
  for (const auto &operand : splitOperands(tokenize(operands))) {
    const auto open =
        std::find_if(operand.begin(), operand.end(), [](const Token &t) {
          return t.kind == TokenKind::Punct && t.text == "[";
        });
    if (open == operand.end())
      continue;
    const auto close = std::find_if(open, operand.end(), [](const Token &t) {
      return t.kind == TokenKind::Punct && t.text == "]";
    });
    const X86Address address =
        parseX86Address(std::vector<Token>(open + 1, close));
    if (!address.base ||
        lower(address.tokens[*address.base].text) != frameRegister)
      continue;
    return FrameAccess{
        address.displacement,
        sizeBits(memorySize(std::vector<Token>(operand.begin(), open))) / 8};
  }
  return std::nullopt;
}

std::string_view x86SizeKeyword(unsigned bytes) {
#define NEVERD_X86_SIZE_BITS(Keyword, Bits)                                    \
  if (bytes * 8 == Bits)                                                       \
    return Keyword;
#include "ListingVocabulary.def"
  return {};
}

ClassicInstruction classicInstruction(OperandDialect dialect,
                                      std::string_view mnemonic,
                                      std::string_view operands,
                                      std::string_view bytes) {
  ClassicInstruction result{std::string(mnemonic), std::string(operands)};
  if (dialect != OperandDialect::X86)
    return result;
#define NEVERD_X86_ENCODING_SPELLING(Bytes, Mnemonic, Operands)                \
  if (bytes == Bytes)                                                          \
    return {Mnemonic, Operands};
#include "ListingVocabulary.def"
  static const std::unordered_map<std::string_view, std::string_view>
      spellings = {
#define NEVERD_X86_MNEMONIC_SPELLING(Engine, Classic) {Engine, Classic},
#include "ListingVocabulary.def"
      };
  static const std::unordered_set<std::string_view> hiddenPrefixes = {
#define NEVERD_X86_HIDDEN_PREFIX(Spelling) Spelling,
#include "ListingVocabulary.def"
  };
  static const std::unordered_set<std::string_view> stringMnemonics = {
#define NEVERD_X86_STRING_MNEMONIC(Spelling) Spelling,
#include "ListingVocabulary.def"
  };
  // Prefixes share the mnemonic field: `rep movsb`, `notrack jmp`.
  std::vector<std::string_view> words;
  for (std::size_t i = 0; i < mnemonic.size();) {
    while (i < mnemonic.size() &&
           std::isspace(static_cast<unsigned char>(mnemonic[i])))
      ++i;
    const std::size_t start = i;
    while (i < mnemonic.size() &&
           !std::isspace(static_cast<unsigned char>(mnemonic[i])))
      ++i;
    if (i > start)
      words.push_back(mnemonic.substr(start, i - start));
  }
  if (words.empty())
    return result;
  std::string_view base = words.back();
  if (const auto it = spellings.find(base); it != spellings.end())
    base = it->second;
  result.mnemonic.clear();
  for (std::size_t i = 0; i + 1 < words.size(); ++i)
    if (!hiddenPrefixes.contains(words[i])) {
      result.mnemonic += words[i];
      result.mnemonic += ' ';
    }
  result.mnemonic += base;
  const auto pieces = splitOperands(tokenize(operands));
  // The SSE movsd and cmpsd name an xmm register; the string forms never do.
  const bool vector = std::any_of(
      pieces.begin(), pieces.end(), [](const OperandTokens &operand) {
        return isLoneRegister(operand) &&
               lower(operand.front().text).starts_with("xmm");
      });
  if (stringMnemonics.contains(base) && !vector) {
    result.operands.clear();
    return result;
  }
  const auto text = [](const OperandTokens &operand) {
    std::string spelled;
    for (const Token &token : operand)
      spelled += token.text;
    return spelled;
  };
  const unsigned forms = x86OperandForms(base);
  if ((forms & FoldRepeatedSource) && pieces.size() == 3 &&
      text(pieces[0]) == text(pieces[1])) {
    result.operands = text(pieces[0]) + ", " + text(pieces[2]);
    result.operandIndices = {0, 2};
  } else if ((forms & SwapRegisters) && pieces.size() == 2 &&
             isLoneRegister(pieces[0]) && isLoneRegister(pieces[1])) {
    result.operands = text(pieces[1]) + ", " + text(pieces[0]);
    result.operandIndices = {1, 0};
  }
  return result;
}

bool isRegisterName(OperandDialect dialect, std::string_view name) {
  const auto key = lower(name);
  const auto &table = registerTable();
  switch (dialect) {
  case OperandDialect::X86:
    return table.x86.contains(key) ||
           (key.starts_with("st(") && key.size() == 5 && key[4] == ')');
  case OperandDialect::AArch64: {
    // Vector registers may carry an arrangement (`v0.16b`) or lane (`v1.s`).
    const auto dot = key.find('.');
    return table.aarch64.contains(
        dot == std::string::npos ? key : key.substr(0, dot));
  }
  case OperandDialect::ARM:
    return table.arm.contains(key);
  case OperandDialect::SBF:
    return table.sbf.contains(key);
  case OperandDialect::EVM:
    return false;
  case OperandDialect::Generic:
    return !key.empty() && key.front() == '$';
  }
  return false;
}

bool isOperandKeyword(std::string_view name) {
  static const std::unordered_set<std::string_view> keywords = {
#define NEVERD_OPERAND_KEYWORD(Spelling) Spelling,
#include "ListingVocabulary.def"
  };
  return keywords.contains(name);
}

std::string_view dataNamePrefix(std::string_view sizeKeyword) {
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Bytes, Prefix)                      \
  if (sizeKeyword == SizeKeyword)                                              \
    return Prefix;
#include "neverd/DataNames.def"
  return {};
}

std::string_view sizeKeywordOf(std::uint64_t bytes) {
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Bytes, Prefix)                      \
  if (bytes == Bytes)                                                          \
    return SizeKeyword;
#include "neverd/DataNames.def"
  return {};
}

std::optional<std::uint64_t> parseDummyName(std::string_view name) {
  static constexpr std::array prefixes = {
#define NEVERD_AUTOMATIC_NAME_PREFIX(Prefix) std::string_view(Prefix),
#include "neverd/AutomaticNames.def"
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Bytes, Prefix)                      \
  std::string_view(Prefix),
#define NEVERD_DATA_ITEM_NAME(Id, Prefix) std::string_view(Prefix),
#include "neverd/DataNames.def"
  };
  for (std::string_view prefix : prefixes) {
    if (!name.starts_with(prefix) || name.size() == prefix.size() ||
        name.size() - prefix.size() > 16)
      continue;
    std::uint64_t value = 0;
    const auto *begin = name.data() + prefix.size();
    const auto *end = name.data() + name.size();
    const auto parsed = std::from_chars(begin, end, value, 16);
    if (parsed.ec == std::errc{} && parsed.ptr == end)
      return value;
  }
  return std::nullopt;
}

bool isPaddingMnemonic(std::string_view mnemonic) {
  static const std::unordered_set<std::string_view> padding = {
#define NEVERD_PADDING_MNEMONIC(Spelling) Spelling,
#include "ListingVocabulary.def"
  };
  return padding.contains(mnemonic);
}

namespace operand_format_detail {
std::optional<SbfAccess> sbfOperandAccess(std::string_view mnemonic,
                                          std::string_view operands,
                                          std::size_t operand) {
  const auto pieces = splitOperands(tokenize(operands));
  if (operand >= pieces.size())
    return std::nullopt;
  return sbfMemoryAccess(mnemonic, pieces[operand]);
}
} // namespace operand_format_detail

} // namespace neverd::worker
