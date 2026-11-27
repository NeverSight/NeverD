//===- SourceDetail.h - Structured Python source emission ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps Python expressions, closure bindings and compiler-only stack
/// values distinct until a statement can consume them.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_PYTHON_SOURCEDETAIL_H
#define NEVERD_ANALYSIS_BYTECODE_PYTHON_SOURCEDETAIL_H

#include "Artifacts.h"

#include <map>

namespace neverd::python {
struct Expr {
  enum Kind {
    Plain,
    Method,
    Constant,
    Tuple,
    Cell,
    Cells,
    Iterator,
    Awaitable,
    YieldFrom,
    Definition,
    KeywordDefaults,
    Import,
    Imported,
    Augment
  } Type = Plain;
  std::string Text, Extra;
  const Object *Value = nullptr;
  std::vector<std::string> Values;
  std::map<std::string, std::string> Keywords;
};
struct Comprehension {
  std::string Element, Target, Condition;
  std::string apply(llvm::StringRef Iterable) const;
};
struct Body {
  std::string Text;
  std::optional<Comprehension> ComprehensionBody;
};
class Emitter {
  Job &J;
  Function &F;
  Code &C;
  std::vector<Function> &Functions;
  std::vector<Body> &Bodies;
  Body &Result;
  unsigned NextLine;
  bool Module;
  size_t Current = 0;
  unsigned ExpressionDepth = 0;
  using Stack = std::vector<Expr>;
  std::map<size_t, size_t> EquivalentExits;
  size_t normalized(size_t P) const;
  [[noreturn]] void reject(llvm::StringRef Why) const;
  [[noreturn]] void invalid(llvm::StringRef Why) const;
  std::string literal(const Object &V, unsigned Depth = 0);
  std::string text(Expr E);
  std::vector<std::string> tuple(Expr E, bool Strings = false);
  std::string name(const Instruction &I) const;
  Expr pop(Stack &S) const;
  std::vector<std::string> args(Stack &S, unsigned Count);
  bool expression(size_t &P, size_t End, Stack &S);
  std::string expressionRange(size_t B, size_t E);
  std::string signature(const Code &Child, llvm::StringRef Name,
                        const Expr &Binding);
  void line(unsigned Indent, llvm::StringRef Text);
  void nested(unsigned Indent, const Expr &Binding, llvm::StringRef Target);
  void block(size_t B, size_t E, unsigned Depth);
  size_t exceptionBlock(size_t P, size_t End, unsigned Depth, Stack &S);
  void comprehension();
  size_t target(size_t P) const;
  bool at(size_t P, Op Opcode) const;
  bool same(size_t A, size_t B, size_t N) const;

public:
  Emitter(Job &J, Function &F, std::vector<Function> &All,
          std::vector<Body> &Bodies);
  void run();
};
bool identifier(llvm::StringRef S);
} // namespace neverd::python

#endif // NEVERD_ANALYSIS_BYTECODE_PYTHON_SOURCEDETAIL_H
