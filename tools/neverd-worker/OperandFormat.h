#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker {

/// Display role of a span of formatted listing text (ListingRoles.def).
enum class ListingRole : std::uint8_t {
#define NEVERD_LISTING_ROLE(Name, Code) Name = Code,
#include "ListingRoles.def"
};

/// Operand text dialect of an engine architecture.
enum class OperandDialect : std::uint8_t {
  X86,
  AArch64,
  ARM,
  SBF,
  EVM,
  Generic
};
OperandDialect operandDialect(std::string_view architecture);

struct TextSpan {
  std::uint32_t start = 0;
  std::uint32_t length = 0;
  ListingRole role = ListingRole::Plain;
  /// Location a name span denotes, for navigation.
  std::optional<std::uint64_t> address;
};

/// Text plus non-overlapping role spans in ascending order.
class StyledText {
public:
  void append(std::string_view text, ListingRole role,
              std::optional<std::uint64_t> address = std::nullopt);
  /// Pad with spaces to \p column, or add one space when already past it.
  void padTo(std::size_t column);
  void append(const StyledText &other);
  const std::string &text() const { return text_; }
  const std::vector<TextSpan> &spans() const { return spans_; }
  bool empty() const { return text_.empty(); }

  /// Columns the text takes in a fixed-width font.
  std::size_t columns() const { return columns_; }

private:
  std::string text_;
  std::vector<TextSpan> spans_;
  std::size_t columns_ = 0;
};

/// Columns \p text takes in a fixed-width font: two for East Asian wide
/// characters, one for every other character or malformed byte.
std::size_t displayColumns(std::string_view text);

/// How an operand reaches a location; it selects an automatic name when the
/// location has none.
enum class NameUse : std::uint8_t { Transfer, Data, Address, Slot };

struct LocationName {
  std::string text;
  ListingRole role = ListingRole::Plain;
  std::uint64_t address = 0;
};

/// Names a location for display.  \p SizeKeyword is the operand size keyword
/// (for example "qword") when the operand states one.
using LocationNamer = std::function<LocationName(
    std::uint64_t Address, NameUse Use, std::string_view SizeKeyword)>;

/// A stack variable that a frame-relative operand falls in.
struct FrameSlot {
  std::string name;
  /// Offset of the operand inside the variable.
  std::int64_t delta = 0;
  /// Bytes the variable spans.
  unsigned size = 0;
};

/// Names the frame variable at an offset from the frame register.
using FrameNamer = std::function<std::optional<FrameSlot>(std::int64_t)>;
/// The base the user shows an operand's number in (neverd/OperandFormats.def).
enum class NumberBase : std::uint8_t {
#define NEVERD_OPERAND_BASE(Id, Spelling) Id,
#include "neverd/OperandFormats.def"
};
std::optional<NumberBase> parseNumberBase(std::string_view spelling);
std::string_view numberBaseName(NumberBase base);
/// How the user shows an operand's number: its base, with its sign changed
/// (`-1` for 0FFFFFFFFh) or its bits inverted (`~0Fh` for 0FFFFFFF0h), or
/// as a structure offset into a local type.
struct NumberFormat {
  NumberBase base = NumberBase::Number;
  bool negate = false, invert = false;
  /// A structure offset's type, and how many bytes into it the register
  /// points: the number lands that much further in.
  std::string type;
  std::int64_t delta = 0;
  bool isDefault() const {
    return base == NumberBase::Number && !negate && !invert;
  }
};

/// Where a structure offset lands in its type: the member's path
/// (`Tail.Overlay.CurrentStackLocation`, empty for the type itself) and
/// where the member begins in the type.
struct StructLanding {
  std::string path;
  std::int64_t offset = 0;
};

/// Lands \p offset, where an access of \p bytes bytes (none for an address)
/// begins, among the members of the local type \p type; none when no member
/// of it holds that byte.
using StructNamer = std::function<std::optional<StructLanding>(
    std::string_view type, std::int64_t offset, unsigned bytes)>;

struct OperandFacts {
  OperandDialect dialect = OperandDialect::Generic;
  std::string_view mnemonic;
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  bool wide = true;
  /// Engine flow kind: "", "call", "icall", "jump", "cjump", "ijump", "ret".
  std::string_view flow;
  std::optional<std::uint64_t> target;
  /// Constant locations the instruction reads, writes or takes the address of.
  std::vector<std::uint64_t> references;
  /// Instruction bytes as lowercase hexadecimal, or empty.
  std::string_view bytes;
  /// The variables of the function's frame and the frame register, when its
  /// prologue sets one up.
  std::string_view frameRegister;
  /// Frame base minus this instruction's proven frame-register coordinate.
  std::int64_t frameDepth = 0;
  const FrameNamer *frame = nullptr;
  /// The stack register and its distance below the frame's base at this
  /// instruction, when known: operands through it name frame variables too.
  std::string_view stackRegister;
  std::optional<std::int64_t> stackDepth;
  /// The user's formats of the operands' numbers, by operand index.
  const std::vector<NumberFormat> *numberFormats = nullptr;
  /// Decoder-provided logical widths in the displayed operand order. Enum
  /// rendering requires these rather than borrowing another operand's width.
  const std::vector<unsigned> *operandBits = nullptr;
  /// Names structure offsets, when the engine has local types.
  const StructNamer *structs = nullptr;
  using EnumNamer = std::function<std::optional<std::string>(
      std::string_view, std::uint64_t, unsigned, bool)>;
  const EnumNamer *enums = nullptr;
};

/// A memory operand relative to a frame register.
struct FrameAccess {
  std::int64_t offset = 0;
  /// Bytes accessed, or zero when only the address is taken.
  unsigned size = 0;
};

/// The memory operand of x86 operand text based on \p frameRegister.
std::optional<FrameAccess> x86FrameAccess(std::string_view operands,
                                          std::string_view frameRegister);

/// The x86 size keyword of \p bytes ("qword" for 8), or empty.
std::string_view x86SizeKeyword(unsigned bytes);

/// An instruction's mnemonic and operand text as classic listings spell it.
struct ClassicInstruction {
  std::string mnemonic, operands;
  std::vector<std::size_t> operandIndices;
};

/// Respell engine disassembler text for \p dialect: classic mnemonics, hidden
/// prefixes, implicit string operands and folded operand forms.  The
/// instruction is the same; only its spelling changes.
ClassicInstruction classicInstruction(OperandDialect dialect,
                                      std::string_view mnemonic,
                                      std::string_view operands,
                                      std::string_view bytes);

/// IDA-compatible x86 numeric spelling: decimal below ten, otherwise uppercase
/// hexadecimal with an `h` suffix and a leading zero before a letter.
std::string x86Number(std::uint64_t value);

/// Format disassembler operand text, replacing exact transfer targets and
/// PC-relative or absolute memory locations with their names.
StyledText formatOperands(std::string_view operands, const OperandFacts &facts,
                          const LocationNamer &namer);
/// Whether formatted operands of \p dialect show the user's number formats.
bool showsNumberFormats(OperandDialect dialect);
/// Which operands of \p operands, an instruction's operand text, a number
/// format changes: the plain numbers, by operand index. \p mnemonic tells an
/// SBF call target or byte-swap width, numbers that are no constants.
std::vector<bool> formattableOperands(std::string_view operands,
                                      OperandDialect dialect,
                                      std::string_view mnemonic = {});
/// Which operands a structure offset changes: the plain numbers and the
/// memory operands that add a displacement to a base register, by index.
std::vector<bool> structOperands(std::string_view operands,
                                 OperandDialect dialect,
                                 std::string_view mnemonic = {});
/// The number operand \p index of \p operands is, as written, a negative
/// one with its sign; none when it is no plain number of \p dialect.
std::optional<std::int64_t> operandNumber(std::string_view operands,
                                          std::size_t index,
                                          OperandDialect dialect,
                                          std::string_view mnemonic = {});

/// Classify one identifier of operand text for \p dialect.
bool isRegisterName(OperandDialect dialect, std::string_view name);
bool isOperandKeyword(std::string_view name);
/// The kinds of data item a user defines besides a value of a size keyword.
#define NEVERD_DATA_ITEM_KIND(Id, Spelling)                                    \
  inline constexpr std::string_view Id = Spelling;
#include "neverd/DataNames.def"

/// Automatic data-name prefix for a size keyword, or empty.
std::string_view dataNamePrefix(std::string_view sizeKeyword);
/// The size keyword of an access \p bytes wide (`qword`), or empty.
std::string_view sizeKeywordOf(std::uint64_t bytes);
/// Parse an automatic name such as `loc_F2329` back to its address.
std::optional<std::uint64_t> parseDummyName(std::string_view name);
bool isPaddingMnemonic(std::string_view mnemonic);

} // namespace neverd::worker
