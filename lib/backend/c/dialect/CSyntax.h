//===- CSyntax.h - Reading the C NeverD emits -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A reader for the C the HighC emitter writes, so that another language can
/// spell the same code (SourceDialect.h).  The emitted C is the semantic
/// authority; this reads it back without second-guessing it.  It accepts the
/// subset of C and of its GNU extensions the emitter writes and rejects
/// anything else with the offset it stopped at, rather than skipping text it
/// cannot read.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_DIALECT_CSYNTAX_H
#define NEVERD_LIB_BACKEND_C_DIALECT_CSYNTAX_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {
namespace csyntax {

enum class TokenKind : uint8_t {
  Identifier,
  Number,
  Char,
  String,
  Punctuator,
  /// A `/* */` or `//` comment, kept so that another spelling keeps it.
  Comment,
  /// A whole preprocessor line, `#include <stdint.h>`.
  Directive,
  End,
};

struct Token {
  TokenKind Kind = TokenKind::End;
  /// The token's text in the source.
  llvm::StringRef Text;
  /// Its byte offset in the source.
  size_t Offset = 0;
  /// Whether it is the first token on its line.
  bool LineStart = false;

  bool is(TokenKind K, llvm::StringRef Spelling) const {
    return Kind == K && Text == Spelling;
  }
  bool isPunctuator(llvm::StringRef Spelling) const {
    return is(TokenKind::Punctuator, Spelling);
  }
};

/// The tokens of \p Source, ending with an End token, or an error at the
/// first byte no C token starts with or an unterminated comment, string or
/// character.
llvm::Expected<std::vector<Token>> lex(llvm::StringRef Source);

/// Visit the same tokens without retaining a token array for a large unit.
/// Token text borrows \p Source; \p Visit must not mutate that storage.
llvm::Error forEachToken(llvm::StringRef Source,
                         llvm::function_ref<void(const Token &)> Visit);

} // namespace csyntax
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_DIALECT_CSYNTAX_H
