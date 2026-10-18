//===- CSyntaxTree.h - The C NeverD emits, read back ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The syntax tree and C types of emitted C (CSyntax.h), so that another
/// language can spell the same code.  The parser keeps every token's meaning:
/// parentheses, casts and comments as written.  The checker then gives each
/// expression its C type and the type C performs its operation in, so that a
/// language without C's implicit conversions spells them out.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_DIALECT_CSYNTAXTREE_H
#define NEVERD_LIB_BACKEND_C_DIALECT_CSYNTAXTREE_H

#include "CSyntax.h"

#include "neverd/Common.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd {
namespace csyntax {

/// The sizes C leaves to the target.
struct CDataModel {
  unsigned PointerBits = 64;
  unsigned LongBits = 64;
  unsigned LongDoubleBits = 80;
  unsigned WCharBits = 32;
  bool WCharSigned = true;
  bool CharSigned = true;

  static CDataModel forTarget(Arch TheArch, BinaryFormat Format);
};

struct RecordDef;

/// A C type.  Types are interned by their TypeContext: one object per type,
/// so that pointer equality is type identity.
struct CType {
  enum class Kind : uint8_t {
    Void,
    Bool,
    Integer,
    Floating,
    Pointer,
    Array,
    Function,
    Record,
    /// A type a header names whose layout the reader does not need, such as
    /// `FILE` or `__m128i`.
    Named,
  };
  Kind TheKind = Kind::Void;
  /// The width of an integer or floating type, and of a named type when its
  /// header gives one.
  unsigned Bits = 0;
  bool Signed = false;
  /// A bit-precise `_BitInt(N)` integer, which C never promotes.
  bool BitPrecise = false;
  bool Const = false;
  bool Volatile = false;
  unsigned AddressSpace = 0;
  /// The pointee, the element, or the return type.
  const CType *Inner = nullptr;
  std::optional<uint64_t> Count;
  std::vector<const CType *> Params;
  bool Variadic = false;
  /// False for a function declared with `()`, which takes any arguments.
  bool Prototyped = true;
  /// How C spells the type without its qualifiers: a typedef name
  /// (`uint64_t`), a keyword type (`unsigned __int128`), a record
  /// (`struct QDomNode`) or a header's type (`__m128i`); empty for derived
  /// types.
  std::string Spelling;
  const RecordDef *Record = nullptr;

  bool isVoid() const { return TheKind == Kind::Void; }
  bool isInteger() const {
    return TheKind == Kind::Integer || TheKind == Kind::Bool;
  }
  bool isFloating() const { return TheKind == Kind::Floating; }
  bool isArithmetic() const { return isInteger() || isFloating(); }
  bool isPointer() const { return TheKind == Kind::Pointer; }
  bool isScalar() const { return isArithmetic() || isPointer(); }
  bool isArray() const { return TheKind == Kind::Array; }
  bool isFunction() const { return TheKind == Kind::Function; }
  bool isRecord() const { return TheKind == Kind::Record; }
  /// The result of an intrinsic whose signature the text leaves to its
  /// header: a reader converts nothing to or from it.
  bool isUnknown() const { return TheKind == Kind::Named && Spelling.empty(); }
};

struct RecordField {
  /// Empty for a bit-field the record leaves unnamed, and for an anonymous
  /// structure or union, whose members are the record's own (C17 6.7.2.1).
  std::string Name;
  const CType *Type = nullptr;
  /// A bit-field's width in bits.
  std::optional<unsigned> BitWidth;

  bool isAnonymousRecord() const {
    return Name.empty() && !BitWidth && Type && Type->isRecord();
  }
};

struct RecordDef {
  /// `struct QDomNode`, `union _LARGE_INTEGER`.
  std::string Spelling;
  std::vector<RecordField> Fields;
  bool Complete = false;
  bool Union = false;
  /// A structure or union declared without a tag, as a member.
  bool Anonymous = false;
  /// The `#pragma pack` the record is defined under; 0 for none.
  unsigned Pack = 0;
  /// The alignment `DECLSPEC_ALIGN(N)` declares; 0 for the natural one.
  uint64_t Alignment = 0;
};

/// Owns and interns the C types of one text.
class TypeContext {
public:
  explicit TypeContext(const CDataModel &Model);

  const CDataModel &model() const { return Model; }
  const CType *voidType() const { return Void; }
  const CType *boolType() const { return Bool; }
  /// `int`, `unsigned int`, `long`, `char`... as the data model sizes them.
  const CType *intType() const { return Int; }
  const CType *unsignedIntType() const { return UnsignedInt; }
  const CType *charType() const { return Char; }
  const CType *sizeType() const { return Size; }
  const CType *ptrdiffType() const { return Ptrdiff; }
  const CType *doubleType() const { return Double; }
  const CType *unknownType() const { return Unknown; }
  const CType *integer(unsigned Bits, bool Signed, llvm::StringRef Spelling,
                       bool BitPrecise = false);
  const CType *floating(unsigned Bits, llvm::StringRef Spelling);
  const CType *named(llvm::StringRef Spelling, unsigned Bits);
  const CType *pointerTo(const CType *Pointee);
  const CType *arrayOf(const CType *Element, std::optional<uint64_t> Count);
  const CType *function(const CType *Return, std::vector<const CType *> Params,
                        bool Variadic, bool Prototyped);
  const CType *record(llvm::StringRef Spelling);
  /// A structure or union declared without a tag, as a record's member.
  const CType *anonymousRecord(bool Union);
  /// `enum Name`: an integer of int's width, as MSVC and GCC give an
  /// enumeration whose values int holds.
  const CType *enumeration(llvm::StringRef Name);
  /// \p T with \p Const and \p Volatile added, or in \p AddressSpace.
  const CType *qualified(const CType *T, bool Const, bool Volatile,
                         unsigned AddressSpace = 0);
  const CType *unqualified(const CType *T);
  /// A typedef name's type keeps that spelling.
  const CType *spelled(const CType *T, llvm::StringRef Spelling);
  RecordDef *recordDef(llvm::StringRef Spelling);

  /// The type C promotes an integer operand to (C17 6.3.1.1).
  const CType *promoted(const CType *T);
  /// The common type of two arithmetic operands (C17 6.3.1.8).
  const CType *usualArithmetic(const CType *A, const CType *B);
  /// The size of \p T in bytes, when C defines one.
  std::optional<uint64_t> sizeOf(const CType *T) const;

private:
  const CType *intern(CType T);
  CDataModel Model;
  std::deque<CType> Types;
  std::map<std::string, const CType *> Interned;
  std::deque<RecordDef> Records;
  llvm::StringMap<RecordDef *> RecordsByName;
  unsigned Anonymous = 0;
  const CType *Void, *Bool, *Int, *UnsignedInt, *Char, *Size, *Ptrdiff, *Double,
      *Unknown;
};

struct Comment {
  llvm::StringRef Text;
  size_t Offset = 0;
  /// Whether nothing but blanks precede it on its line.
  bool OwnLine = false;
};
using CommentList = llvm::SmallVector<Comment, 0>;

struct Decl;
struct Stmt;

enum class ExprKind : uint8_t {
  Name,
  Integer,
  Floating,
  Character,
  String,
  Paren,
  /// `-x`, `~x`, `!x`, `*p`, `&x`, prefix and postfix `++`/`--`.
  Unary,
  Binary,
  /// `=` and the compound assignments.
  Assign,
  Conditional,
  Comma,
  Cast,
  Call,
  Subscript,
  /// `.` and `->`.
  Member,
  SizeofExpr,
  SizeofType,
  AlignofType,
  CompoundLiteral,
  InitList,
  /// `[N] = value` or `.field = value` in an initializer list.
  Designated,
  /// GNU `({ ... })`.
  StatementExpr,
  /// `__builtin_bit_cast(T, x)`.
  BitCast,
  /// `__builtin_offsetof(T, field)`, or `offsetof` as `<stddef.h>` spells
  /// it.
  Offsetof,
  /// `va_arg(ap, T)`, whose operand is the list and whose type is written.
  VaArg,
  /// `__asm__(...)` where the emitter wrote it as a value; kept as written.
  Asm,
};

struct Expr {
  ExprKind Kind = ExprKind::Name;
  /// The half-open byte range of the expression in the C text.
  size_t Begin = 0;
  size_t End = 0;
  /// A name, a literal or an operator as written.
  llvm::StringRef Text;
  bool Postfix = false;
  llvm::SmallVector<Expr *, 2> Ops;
  /// The type a cast, sizeof, compound literal or bit cast names.
  const CType *Written = nullptr;
  Stmt *Body = nullptr;
  CommentList Leading;
  CommentList Trailing;

  // The checker's results.
  /// The expression's type before lvalue conversion: an array stays an
  /// array, a function a function.
  const CType *Ty = nullptr;
  /// The type an operator computes in after C's promotions and usual
  /// arithmetic conversions; for a comparison, the operands' common type.
  const CType *OpTy = nullptr;
  bool LValue = false;
  /// The declaration a name refers to.
  const Decl *Ref = nullptr;
  /// The field a member access or `offsetof` names, and whether it reaches
  /// the field through anonymous records.
  const RecordField *Field = nullptr;
  bool ThroughAnonymous = false;
  /// The value an integer constant expression has, when the checker knows it.
  std::optional<int64_t> Value;
};

struct Handler {
  enum class Kind : uint8_t { Except, Finally, Catch };
  Kind TheKind = Kind::Except;
  /// `__except (filter)`.
  Expr *Filter = nullptr;
  /// `catch (const T &name)` as written, or `...`.
  llvm::StringRef Parameter;
  Stmt *Body = nullptr;
  CommentList Leading;
};

enum class StmtKind : uint8_t {
  Compound,
  Declaration,
  Expression,
  If,
  While,
  DoWhile,
  For,
  Switch,
  Case,
  Default,
  Break,
  Continue,
  Return,
  Goto,
  Label,
  Empty,
  /// A comment on its own between statements.
  Comment,
  /// `__try` with `__except` or `__finally`; `try` with `catch`.
  Try,
  Throw,
  /// `__leave;`.
  Leave,
  /// An `__asm__` statement or an MSVC `__asm { }` block, kept as written.
  Asm,
};

struct Stmt {
  StmtKind Kind = StmtKind::Empty;
  size_t Begin = 0;
  size_t End = 0;
  /// A label, a goto target or the assembly text.
  llvm::StringRef Text;
  /// `try` (C++) rather than `__try`.
  bool CxxTry = false;
  /// The condition, the value returned or thrown, the expression, the case
  /// value; the For init expression.
  Expr *Value = nullptr;
  /// For: the step.
  Expr *Step = nullptr;
  Stmt *Then = nullptr;
  Stmt *Else = nullptr;
  std::vector<Stmt *> Body;
  std::vector<Decl *> Decls;
  std::vector<Handler> Handlers;
  Comment TheComment;
  CommentList Leading;
  CommentList Trailing;
};

enum class DeclKind : uint8_t {
  Variable,
  Function,
  Parameter,
  Typedef,
  Record,
  /// `enum Name { ... };`, its enumerators in Fields.
  Enumeration,
  /// A constant an enumeration declares, its value in Init.
  Enumerator
};

struct Decl {
  DeclKind Kind = DeclKind::Variable;
  size_t Begin = 0;
  size_t End = 0;
  llvm::StringRef Name;
  const CType *Ty = nullptr;
  /// Opening parenthesis of a function's C declaration with no prototype.
  /// A definition still has a known parameter count, including zero.
  std::optional<size_t> UnprototypedParams;
  bool Extern = false;
  bool Static = false;
  bool Inline = false;
  bool Register = false;
  bool Noreturn = false;
  std::optional<uint64_t> AlignAs;
  /// An enumerator's value, as the checker computes it.
  std::optional<int64_t> Value;
  /// The string literal of `__asm__("symbol")` after a declarator, as
  /// written.
  llvm::StringRef AsmLabel;
  /// `__attribute__((...))` lists as written.
  llvm::SmallVector<llvm::StringRef, 1> Attributes;
  Expr *Init = nullptr;
  std::vector<Decl *> Params;
  Stmt *Body = nullptr;
  /// A record definition's fields; an enumeration's enumerators.
  std::vector<Decl *> Fields;
  CommentList Leading;
  CommentList Trailing;
};

struct TopLevel {
  enum class Kind : uint8_t {
    /// One declaration statement: a function, prototypes, variables, a
    /// typedef or a record.
    Declaration,
    /// `_Static_assert(condition, "message");`, or `static_assert` as C23,
    /// C++ and `<assert.h>` spell it, the keyword in Text.
    StaticAssert,
    Comment,
    /// A preprocessor line.
    Directive,
    /// A declaration the reader could not read, kept as C text.
    Unread,
  };
  Kind TheKind = Kind::Declaration;
  size_t Begin = 0;
  size_t End = 0;
  std::vector<Decl *> Decls;
  Expr *Assertion = nullptr;
  Expr *Message = nullptr;
  Comment TheComment;
  llvm::StringRef Text;
  /// Why an Unread declaration was not read.
  std::string Reason;
  CommentList Trailing;
};

/// A C text read back: its declarations in order, and the nodes they own.
class Tree {
public:
  explicit Tree(const CDataModel &Model) : Types(Model) {}
  Tree(const Tree &) = delete;
  Tree &operator=(const Tree &) = delete;

  Expr *newExpr(ExprKind Kind, size_t Begin) {
    Expr &E = Exprs.emplace_back();
    E.Kind = Kind;
    E.Begin = E.End = Begin;
    return &E;
  }
  Stmt *newStmt(StmtKind Kind, size_t Begin) {
    Stmt &S = Stmts.emplace_back();
    S.Kind = Kind;
    S.Begin = S.End = Begin;
    return &S;
  }
  Decl *newDecl(DeclKind Kind, size_t Begin) {
    Decl &D = Decls.emplace_back();
    D.Kind = Kind;
    D.Begin = D.End = Begin;
    return &D;
  }

  llvm::StringRef Source;
  /// Whose C runtime declares what the text leaves to its headers.
  BinaryFormat Format = BinaryFormat::Unknown;
  TypeContext Types;
  std::vector<TopLevel> Items;
  /// Declarations read from elsewhere, which the nodes point into.
  std::deque<std::string> Sources;

private:
  std::deque<Expr> Exprs;
  std::deque<Stmt> Stmts;
  std::deque<Decl> Decls;
};

/// Reads \p Source into \p Result.  A top-level declaration it cannot read
/// becomes an Unread item with the reason; the rest is still read.
void parse(llvm::StringRef Source, Tree &Result);

/// Reads the declarations of \p Source, which must outlive \p Nodes, with
/// \p Nodes' types and nodes but not into its items.
std::vector<TopLevel> parseDeclarations(llvm::StringRef Source, Tree &Nodes);

/// Gives every expression of \p Result's declarations its C type.  A
/// declaration whose code the checker cannot type becomes Unread with the
/// reason, as one the parser could not read.
void check(Tree &Result);

} // namespace csyntax
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_DIALECT_CSYNTAXTREE_H
