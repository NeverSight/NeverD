//===- SourceDialect.cpp - Pseudocode in its source language --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/dialect/SourceDialect.h"

#include "CSyntaxTree.h"
#include "DialectPrinter.h"

#include "neverd/loader/SymbolSpelling.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringSet.h"

using namespace neverd;
using namespace neverd::csyntax;

llvm::StringRef neverd::sourceDialectKey(SourceDialect Dialect) {
  switch (Dialect) {
#define NEVERD_SOURCE_DIALECT(Id, Key, Display)                                \
  case SourceDialect::Id:                                                      \
    return Key;
#include "neverd/backend/c/dialect/SourceDialects.def"
  }
  return "c";
}

llvm::StringRef neverd::sourceDialectDisplayName(SourceDialect Dialect) {
  switch (Dialect) {
#define NEVERD_SOURCE_DIALECT(Id, Key, Display)                                \
  case SourceDialect::Id:                                                      \
    return Display;
#include "neverd/backend/c/dialect/SourceDialects.def"
  }
  return "C";
}

std::optional<SourceDialect> neverd::sourceDialectFromKey(llvm::StringRef Key) {
#define NEVERD_SOURCE_DIALECT(Id, DialectKey, Display)                         \
  if (Key == DialectKey)                                                       \
    return SourceDialect::Id;
#include "neverd/backend/c/dialect/SourceDialects.def"
  return std::nullopt;
}

std::optional<SourceDialect>
neverd::dialectOfRuntime(SourceLanguageRuntime Runtime) {
  switch (Runtime) {
  case SourceLanguageRuntime::CxxItanium:
  case SourceLanguageRuntime::CxxMSVC:
    return SourceDialect::Cpp;
  case SourceLanguageRuntime::Rust:
    return SourceDialect::Rust;
  case SourceLanguageRuntime::Go:
    return SourceDialect::Go;
  default:
    return std::nullopt;
  }
}

std::vector<SourceDialect>
neverd::offeredSourceDialects(const LanguageRuntimeInfo &Language) {
  std::vector<SourceDialect> Offered = {SourceDialect::C};
  // In the order SourceDialects.def lists them.
  for (SourceDialect Dialect :
       {SourceDialect::Cpp, SourceDialect::Rust, SourceDialect::Go}) {
    bool Runs = dialectOfRuntime(Language.Runtime) == Dialect;
    for (SourceLanguageRuntime Runtime : Language.SecondaryRuntimes)
      Runs |= dialectOfRuntime(Runtime) == Dialect;
    if (Runs)
      Offered.push_back(Dialect);
  }
  return Offered;
}

SourceDialect
neverd::sourceDialectOfFunction(llvm::StringRef Symbol, bool Named,
                                const LanguageRuntimeInfo &Language,
                                SourceLanguageRuntime HandlerRuntime) {
  if (dialectOfRuntime(HandlerRuntime) == SourceDialect::Cpp)
    return SourceDialect::Cpp;
  const SourceDialect Image =
      dialectOfRuntime(Language.Runtime).value_or(SourceDialect::C);
  SourceDialect Chosen = SourceDialect::C;
  if (!Named) {
    Chosen = Image;
  } else {
    switch (symbolScheme(Symbol)) {
    case SymbolScheme::Itanium:
    case SymbolScheme::Microsoft:
      if (!readableSymbolName(Symbol).empty())
        Chosen = SourceDialect::Cpp;
      break;
    case SymbolScheme::RustLegacy:
    case SymbolScheme::RustV0:
      Chosen = SourceDialect::Rust;
      break;
    case SymbolScheme::None:
      // C-linkage entry points (including main) in a C++ image still read
      // in C++; their names alone cannot classify their implementation.
      if (Image == SourceDialect::Cpp)
        Chosen = Image;
      // Go names a function by its package path: `main.main`.
      if (Image == SourceDialect::Go && Symbol.contains('.'))
        Chosen = SourceDialect::Go;
      break;
    default:
      break;
    }
  }
  // A symbol cannot offer a language the image has not established.
  const std::vector<SourceDialect> Offered = offeredSourceDialects(Language);
  if (!llvm::is_contained(Offered, Chosen))
    return SourceDialect::C;
  return Chosen;
}

SourceDialectText neverd::spellInDialect(llvm::StringRef C,
                                         const SourceDialectOptions &Options) {
  Tree T(CDataModel::forTarget(Options.TheArch, Options.Format));
  T.Format = Options.Format;
  std::string Headers;
  if (Options.Types) {
    auto Tokens = lex(C);
    if (Tokens) {
      llvm::StringSet<> Seen;
      std::vector<const types::LocalType *> Referenced;
      for (const Token &Tok : *Tokens)
        if (Tok.Kind == TokenKind::Identifier && Seen.insert(Tok.Text).second)
          if (const types::LocalType *Type = Options.Types->find(Tok.Text);
              Type && (Type->Origin == types::TypeOrigin::Header ||
                       llvm::is_contained(Options.HeaderTypes, Type->Name)))
            Referenced.push_back(Type);
      types::RenderOptions Render;
      Render.Spelling = types::BuiltinSpelling::Portable;
      if (!Referenced.empty())
        Headers = types::renderTypes(*Options.Types, Referenced, Render);
    } else {
      llvm::consumeError(Tokens.takeError());
    }
  }
  parse(C, T, Headers);
  check(T);
  std::unique_ptr<DialectPrinter> Printer;
  switch (Options.Dialect) {
  case SourceDialect::Rust:
    Printer = makeRustPrinter(T, Options);
    break;
  case SourceDialect::Go:
    Printer = makeGoPrinter(T, Options);
    break;
  case SourceDialect::C:
  case SourceDialect::Cpp:
    Printer = makeCPrinter(T, Options);
    break;
  }
  return Printer->run();
}
