//===- DialectPrinter.h - Printing emitted C in another syntax --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the Rust and Go printers share: the text they build with the C bytes
/// each statement spells, the source names of functions and objects, the
/// loop and switch facts C's `break` and `continue` depend on, and the rule
/// that a declaration a printer cannot spell is shown as C with the reason.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_DIALECT_DIALECTPRINTER_H
#define NEVERD_LIB_BACKEND_C_DIALECT_DIALECTPRINTER_H

#include "CSyntaxTree.h"

#include "neverd/backend/c/dialect/SourceDialect.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringMap.h"

namespace neverd {
namespace csyntax {

/// How tightly an expression's outermost operator binds; a context
/// parenthesizes an operand that binds less tightly than it needs.
enum Precedence : int {
  PrecLowest = 0,
  PrecAssign = 10,
  PrecOr = 20,
  PrecAnd = 30,
  PrecCompare = 40,
  PrecBitOr = 50,
  PrecBitXor = 55,
  PrecBitAnd = 60,
  PrecShift = 65,
  PrecAdd = 70,
  PrecMul = 80,
  PrecCast = 85,
  PrecUnary = 90,
  PrecAtom = 100,
};

/// A printed expression.
struct Printed {
  std::string Text;
  int Prec = PrecAtom;
  /// An integer constant the printer may respell for any integer type.
  bool Literal = false;
  std::optional<int64_t> Value;
  /// A boolean of the dialect standing for C's `int` 0 or 1.
  bool Bool = false;
  /// A trap standing for a value the emitter does not know; no conversion
  /// applies to it.
  bool Diverges = false;
  /// The C binary operator at the top, for the clarity parentheses a
  /// reader expects when shifts and bitwise operators mix with others.
  llvm::StringRef Operator;
};

/// One case group of a switch: its labels and the statements they run.
struct SwitchArm {
  std::vector<const Stmt *> Labels;
  std::vector<const Stmt *> Body;
  /// Whether its statements can run on into the next group.
  bool FallsThrough = false;
};

class DialectPrinter {
public:
  DialectPrinter(const Tree &T, const SourceDialectOptions &Opts);
  virtual ~DialectPrinter() = default;
  SourceDialectText run();

protected:
  //===--- What each dialect provides ---===//

  /// The comment lines that say how the dialect reads.
  virtual void header() = 0;
  /// A declaration item; the printer refuses with unsupported().
  virtual void declaration(const TopLevel &Item) = 0;
  virtual void staticAssert(const TopLevel &Item) = 0;
  virtual std::string escapeIdentifier(llvm::StringRef Name) const = 0;
  /// The source language's name for a symbol, when it has one.
  virtual std::optional<std::string>
  sourceName(llvm::StringRef Symbol) const = 0;
  virtual std::optional<std::string> sourceTypeName(llvm::StringRef) const {
    return std::nullopt;
  }
  /// A line comment's leader: `//`.
  virtual llvm::StringRef lineComment() const { return "//"; }
  /// A preprocessor line: C's headers are C's, and another language keeps
  /// the other lines as comments.
  virtual void directive(const TopLevel &Item);

  //===--- Output ---===//

  void write(llvm::StringRef Text);
  void endLine();
  void line(llvm::StringRef Text) {
    write(Text);
    endLine();
  }
  void indent() { ++Depth; }
  void dedent() { --Depth; }
  /// Records that the text from \p Begin to here spells C [CBegin, CEnd).
  void piece(size_t CBegin, size_t CEnd, size_t Begin);
  size_t mark() const { return Buffer.size(); }
  /// Prints statements into a string at one more level of indentation.
  template <typename Fn> std::string capture(Fn &&Print) {
    std::string Saved;
    std::swap(Saved, Buffer);
    const bool SavedStart = AtLineStart;
    AtLineStart = true;
    const size_t SavedPieces = Pieces.size();
    indent();
    Print();
    dedent();
    Pieces.resize(SavedPieces);
    std::swap(Saved, Buffer);
    AtLineStart = SavedStart;
    return Saved;
  }
  std::string indentation(unsigned Levels) const {
    return std::string(Levels * 4, ' ');
  }
  unsigned depth() const { return Depth; }

  //===--- Literals ---===//

  /// The bytes a narrow string literal spells, its parts joined, without its
  /// terminator; none for another kind of string.
  static std::optional<std::string> stringBytes(const Expr *E);
  /// The bytes an initializer list of an array of \p Count bytes gives, in
  /// order and at their designated indexes, up to its last one; none when an
  /// element is not a byte constant.
  static std::optional<std::string> listBytes(const Expr *Init, uint64_t Count);
  /// Whether \p Ty is an array of a known number of bytes (unsigned 8-bit
  /// integers), as the emitter declares an image's bytes.
  static bool byteArray(const CType *Ty);

  //===--- The program's types ---===//

  /// How the language reaches the member \p E names, a `.` or `->` access,
  /// when its record is one of the program's types
  /// (SourceDialectOptions::LocalTypes): the hops to it, or for a bit-field
  /// to the record whose methods read and write it; no hops, after
  /// refusing, for a member they reach no other way.
  std::optional<SourceDialectLocalTypes::Path> localMember(const Expr *E) const;
  /// Whether the program's types define \p Ty, a record, in the language.
  bool localRecord(const CType *Ty) const;

  //===--- Comments ---===//

  /// A C comment as the dialect writes it.
  std::string commentText(const Comment &C) const;
  void leadingComments(const CommentList &Comments);
  void trailingComments(const CommentList &Comments);
  /// \p P with an expression's comments around it.
  Printed withComments(const Expr *E, Printed P) const;

  //===--- Names ---===//

  /// The dialect's name for a C identifier of the text.  A source name is
  /// marked so that the finished text can say where it is.
  std::string name(llvm::StringRef Identifier) const;
  bool sourceType(llvm::StringRef Identifier) const;
  /// The dialect's spelling of a source name, unmarked.
  std::optional<std::string> sourceSpelling(llvm::StringRef Identifier) const {
    auto It = SourceNames.find(Identifier);
    if (It == SourceNames.end())
      return std::nullopt;
    return It->second;
  }

  //===--- Facts about C statements ---===//

  /// Whether a `continue` in \p Body continues the loop around it.
  static bool continuesLoop(const Stmt *Body);
  /// Whether a `break` in \p Body leaves the loop or switch around it.
  static bool breaksOut(const Stmt *Body);
  /// Whether a statement never completes normally.
  bool jumps(const Stmt *S) const;
  /// The case groups of a switch body, or none when a statement precedes
  /// the first label.
  std::optional<std::vector<SwitchArm>> switchArms(const Stmt *Switch) const;
  /// Whether \p E calls a function that does not return.
  bool callsNoreturn(const Expr *E) const;

  /// `(__builtin_trap(), 0 /* unknown value */)`: a value the emitter does
  /// not know, which traps when computed.
  static bool isUnknownValue(const Expr *E);
  /// The address `(__builtin_memcpy(&v, src, sizeof(v)), v)` loads from.
  static const Expr *unalignedLoadSource(const Expr *E);
  /// Notes the locals of a function used only as the scratch of unaligned
  /// loads, which a load then need not assign.
  void noteLoadScratch(const Stmt *Body);
  bool isLoadScratch(const Expr *Name) const {
    return Name->Ref && LoadScratch.contains(Name->Ref);
  }

  /// \p E without the parentheses around it.
  static const Expr *skipParens(const Expr *E) {
    while (E->Kind == ExprKind::Paren)
      E = E->Ops.front();
    return E;
  }
  /// The value of \p E converted to the integer type \p Want computes the
  /// same low bits when \p E's operator does (`+ - * & | ^`, `-`, `~`) or
  /// when \p E only widened a value \p Want narrows again: the expression
  /// that can then be printed at \p Want, or null.
  static const Expr *narrowable(const Expr *E, const CType *Want);
  /// Calls \p Visit for every expression of \p S.
  static void forEachExpr(const Stmt *S,
                          llvm::function_ref<void(const Expr *)> Visit);
  static void forEachSubExpr(const Expr *E,
                             llvm::function_ref<void(const Expr *)> Visit);

  /// Refuses the current declaration: it is shown as C with \p Reason.
  void unsupported(const llvm::Twine &Reason) const;
  bool refused() const { return !Refusal.empty(); }

  /// The readable name the C writer puts in a comment before a definition
  /// it names differently.
  bool repeatsName(const TopLevel &Comment, const TopLevel *Next) const;

  const Tree &T;
  const SourceDialectOptions &Opts;

private:
  void item(const TopLevel &Item, const TopLevel *Next);
  void unread(size_t CBegin, size_t CEnd, llvm::StringRef Reason);

  std::string Buffer;
  std::vector<SourceDialectPiece> Pieces;
  std::vector<std::string> Unread;
  unsigned Depth = 0;
  bool AtLineStart = true;
  mutable std::string Refusal;
  /// Removes the marks name() puts around source names, recording where
  /// each name and piece ends up.
  void unmark(SourceDialectText &Result);

  /// C identifier to the dialect's spelling of the symbol it stands for.
  llvm::StringMap<std::string> SourceNames;
  /// C identifier to its entry in Opts.Names.
  llvm::StringMap<const CSourceName *> NameEntries;
  llvm::DenseSet<const Decl *> LoadScratch;
};

/// The printers for each dialect.
std::unique_ptr<DialectPrinter>
makeRustPrinter(const Tree &T, const SourceDialectOptions &Opts);
std::unique_ptr<DialectPrinter> makeGoPrinter(const Tree &T,
                                              const SourceDialectOptions &Opts);
std::unique_ptr<DialectPrinter> makeCPrinter(const Tree &T,
                                             const SourceDialectOptions &Opts);

} // namespace csyntax
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_DIALECT_DIALECTPRINTER_H
