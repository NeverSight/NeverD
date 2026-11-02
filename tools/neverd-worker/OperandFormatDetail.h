//===- OperandFormatDetail.h - Shared operand text helpers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the dialect formatters share: OperandFormat.cpp tokenizes operand
/// text and formats x86 and generic operands; OperandFormatARM.cpp,
/// OperandFormatSBF.cpp and OperandFormatEVM.cpp format the operands of
/// their dialects that the user gives a number format.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_OPERANDFORMATDETAIL_H
#define NEVERD_TOOLS_NEVERD_WORKER_OPERANDFORMATDETAIL_H

#include "OperandFormat.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker::operand_format_detail {

enum class TokenKind : std::uint8_t { Identifier, Number, Punct, Space };
struct Token {
  TokenKind kind = TokenKind::Punct;
  std::string_view text;
  std::uint64_t value = 0;
  bool valid = false;
};
/// Tokens of one operand, without surrounding spaces.
using OperandTokens = std::vector<Token>;

/// \p value as characters, most significant byte first (`'ABCD'`); none
/// unless every byte but leading zeros prints.
std::optional<std::string> characterLiteral(std::uint64_t value);
/// The binary digits of \p value without leading zeros.
std::string binaryDigits(std::uint64_t value);

/// Whether \p dialect spells operands as the ARM family does: `#0x1a`,
/// `[x20, #0x10]`, `{r4, lr}`.
inline bool armDialect(OperandDialect dialect) {
  return dialect == OperandDialect::AArch64 || dialect == OperandDialect::ARM;
}
/// The value of an ARM-family immediate, `#5`, `#-5` or `#0x1a`, unsigned in
/// \p mask's width; none for anything else, a floating constant included.
std::optional<std::uint64_t> armImmediate(const OperandTokens &tokens,
                                          std::uint64_t mask);
/// Whether a structure offset can name an ARM-family operand: an immediate,
/// or a memory operand of a base register and an immediate displacement.
bool armStructOperand(const OperandTokens &tokens, OperandDialect dialect);
/// What an ARM-family instruction's formats need: the width its immediates
/// count in, and the bytes a load or store moves through each register.
struct ArmWidths {
  unsigned immediateBits = 0;
  unsigned accessBytes = 0;
};
ArmWidths armWidths(OperandDialect dialect, std::string_view mnemonic,
                    const std::vector<OperandTokens> &operands);
/// Whether \p tokens shift the operand before it (`lsl #12`).
bool armShift(const OperandTokens &tokens);
/// Appends an ARM-family operand as the user's \p format shows it, an
/// immediate \p shifted by the next operand being only a field of its
/// number; false, with nothing appended, when the format cannot show it.
bool formatArmOperand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const NumberFormat &format, const ArmWidths &widths,
                      bool shifted);

/// The value of an SBF immediate, `5`, `-16` or `0x1a`, in 64 bits; none
/// for anything else and for the call target or byte-swap width that
/// \p mnemonic takes.
std::optional<std::uint64_t> sbfImmediate(std::string_view mnemonic,
                                          const OperandTokens &tokens);
/// Whether a structure offset can name an SBF operand: an immediate, or a
/// memory operand of a base register and a displacement.
bool sbfStructOperand(std::string_view mnemonic, const OperandTokens &tokens);
/// The access an SBF memory operand makes by itself: its base register,
/// displacement, and the bytes the load or store (\p write) moves.
struct SbfAccess {
  std::string base;
  std::int64_t offset = 0;
  unsigned bytes = 0;
  bool write = false;
};
std::optional<SbfAccess> sbfMemoryAccess(std::string_view mnemonic,
                                         const OperandTokens &tokens);
/// The access operand \p operand of the SBF instruction text makes by
/// itself.
std::optional<SbfAccess> sbfOperandAccess(std::string_view mnemonic,
                                          std::string_view operands,
                                          std::size_t operand);
/// Appends an SBF operand as the user's \p format shows it; false, with
/// nothing appended, when the format cannot show it.
bool formatSbfOperand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const NumberFormat &format);

/// The bits of the data an EVM PUSH carries, `PUSH1` to `PUSH32`; none for
/// another mnemonic.
unsigned evmPushBits(std::string_view mnemonic);
/// The value of PUSH data that fits 64 bits; none for wider or other text.
std::optional<std::uint64_t> evmImmediate(std::string_view mnemonic,
                                          const OperandTokens &tokens);
/// Whether \p tokens are the PUSH data of \p mnemonic, of any width.
bool evmPushData(std::string_view mnemonic, const OperandTokens &tokens);
/// Appends EVM PUSH data as the user's \p format shows it, in its own
/// width; false, with nothing appended, when the format cannot show it.
bool formatEvmOperand(StyledText &out, const OperandTokens &tokens,
                      const OperandFacts &facts, const LocationNamer &namer,
                      const NumberFormat &format);

} // namespace neverd::worker::operand_format_detail

#endif // NEVERD_TOOLS_NEVERD_WORKER_OPERANDFORMATDETAIL_H
