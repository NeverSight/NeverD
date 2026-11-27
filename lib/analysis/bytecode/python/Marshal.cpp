//===- Marshal.cpp - Bounded CPython marshal graph reader ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reads inert values. References are published after their referent is
/// complete so recursive graphs cannot become recursive source expressions.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Endian.h"

#include <bit>
#include <cstdint>

using namespace llvm;
using namespace neverd::python;

StringRef Marshal::take(size_t Size) {
  if (Size > Input.size() - Position)
    malformed(Position, "Truncated marshal value");
  auto Result = Input.substr(Position, Size);
  Position += Size;
  return Result;
}

unsigned Marshal::count() {
  const int32_t N = support::endian::read32le(take(4).data());
  if (N < 0)
    malformed(Position - 4, "Negative marshal size or code field");
  return N;
}

void Marshal::code(Object &O, unsigned Depth) {
  J.charge(J.CodeObjects, 1, limits::kCodeObjects, O.Offset);
  O.Body = std::make_unique<Code>();
  auto &C = *O.Body;
  C.Version = Version;
  C.Id = Codes.size();
  Codes.push_back(&O);
  auto CodeCount = [&]() -> unsigned {
    return Version < 203 ? support::endian::read16le(take(2).data()) : count();
  };
  if (Version >= 103) {
    C.Args = CodeCount();
    C.PosOnly = Version >= 308 ? CodeCount() : 0;
    C.KwOnly = Version >= 300 ? CodeCount() : 0;
    if (Version < 311)
      C.Locals = CodeCount();
    C.Stack = Version >= 105 ? CodeCount() : 0;
    C.Flags = CodeCount();
  }
  auto Field = [&](Object::Kind Expected) {
    Object *Field = object(Depth + 1);
    if (Field->Type != Expected)
      malformed(Field->Offset, "Invalid CPython code field type");
    return Field;
  };
  C.Bytes = Field(Object::Bytes);
  auto Sequence = Version <= 100 ? Object::List : Object::Tuple;
  C.Consts = Field(Sequence);
  auto IsName = [&](const Object *V) {
    return V->Type == Object::Unicode ||
           (Version < 300 && V->Type == Object::Bytes && json::isUTF8(V->Data));
  };
  auto Names = [&] {
    auto *Tuple = Field(Sequence);
    if (!llvm::all_of(Tuple->Items, IsName))
      malformed(Tuple->Offset, "Non-string name in code object");
    for (const auto *Name : Tuple->Items)
      J.charge(J.Text, Name->Data.size(), limits::kTextBytes, Name->Offset);
    return Tuple;
  };
  C.Names = Names();
  C.Vars = Version >= 103 ? Names() : &Nodes.emplace_back();
  if (Version < 103)
    C.Vars->Type = Object::Tuple;
  if (Version < 311) {
    C.Free = Version >= 201 ? Names() : &EmptyTuple;
    C.Cells = Version >= 201 ? Names() : &EmptyTuple;
    // The pre-2.5 compiler also recorded module bindings in co_varnames.
    if (C.Locals > C.Vars->Items.size() ||
        (Version >= 205 && C.Locals != C.Vars->Items.size()))
      malformed(O.Offset, "Inconsistent local name count");
  } else {
    // 3.11 stores fast locals, cells and free variables in one indexed table.
    C.LocalKinds = Field(Object::Bytes);
    if (C.LocalKinds->Data.size() != C.Vars->Items.size())
      malformed(C.LocalKinds->Offset,
                "Local kinds do not match localsplus names");
    C.Locals = 0;
    for (unsigned char Kind : C.LocalKinds->Data) {
      if (Kind & 0x20) {
        if (Version >= 312)
          Kind &= ~0x10u; // Hidden local in an inlined comprehension.
        if (Version >= 314)
          Kind &=
              ~0x0eu; // Positional, keyword and variadic argument attributes.
      }
      if (Kind != 0x20 && Kind != 0x40 && Kind != 0x60 && Kind != 0x80)
        malformed(C.LocalKinds->Offset, "Unknown localsplus kind");
      C.Locals += bool(Kind & 0x20);
    }
  }
  auto NameField = [&] {
    auto *V = object(Depth + 1);
    if (!IsName(V))
      malformed(V->Offset, "Invalid code-object name or filename");
    return V;
  };
  C.Filename = NameField();
  C.Name = Version == 8 ? &Unnamed : NameField();
  if (Version >= 311)
    C.QualifiedName = Field(Object::Unicode);
  J.charge(J.Text,
           C.Name->Data.size() + C.Filename->Data.size() +
               (C.QualifiedName ? C.QualifiedName->Data.size() : 0),
           limits::kTextBytes, O.Offset);
  C.FirstLine = Version >= 105 ? CodeCount() : 0;
  C.Lines = Version >= 105 ? Field(Object::Bytes) : &EmptyLines;
  if (Version >= 311)
    C.Exceptions = Field(Object::Bytes);
  uint64_t Params =
      uint64_t(C.Args) + C.KwOnly + bool(C.Flags & 4) + bool(C.Flags & 8);
  if (C.PosOnly > C.Args || Params > C.Locals || C.Bytes->Data.empty() ||
      (Version >= 306 && C.Bytes->Data.size() % 2))
    malformed(O.Offset, "Inconsistent CPython code dimensions");
}
Object *Marshal::object(unsigned Depth, bool AllowEnd) {
  const size_t At = Position;
  J.charge(J.Objects, 1, limits::kObjects, At);
  if (Depth > limits::kDepth)
    throw Failure{"budget_exceeded", At, "Marshal nesting budget exceeded"};
  uint8_t Tag = take(1).front();
  bool Register = Tag & 128;
  Tag &= 127;
  if (Version < 304 && Register)
    malformed(At, "Marshal reference flag predates this code layout");
  if (Tag == 'R' && Version >= 204 && Version < 300) {
    unsigned Id = count();
    if (Id >= Interned.size())
      malformed(At, "Invalid interned string reference");
    return Interned[Id];
  }
  if (Tag == 'r') {
    unsigned Id = count();
    if (Register || Id >= References.size())
      malformed(At, "Invalid marshal reference");
    if (!References[Id])
      unsupported(At, "Recursive marshal value");
    return References[Id];
  }
  auto TextFloat = [&] {
    auto Text = take(uint8_t(take(1).front()));
    double Value;
    if (Text.getAsDouble(Value))
      malformed(At, "Invalid textual marshal float");
    return Value;
  };
  auto BinaryFloat = [&] {
    return std::bit_cast<double>(support::endian::read64le(take(8).data()));
  };
  auto Index = References.size();
  if (Register)
    References.push_back(nullptr);
  Object &O = Nodes.emplace_back();
  O.Offset = At;
  switch (Tag) {
  case '0':
    if (!AllowEnd || Register)
      malformed(At, "Unexpected marshal terminator");
    O.Type = Object::End;
    break;
  case 'N':
    O.Type = Object::None;
    break;
  case 'F':
    O.Type = Object::False;
    break;
  case 'T':
    O.Type = Object::True;
    break;
  case '.':
    O.Type = Object::Ellipsis;
    break;
  case 'i':
    O.Type = Object::Int;
    O.Number = int32_t(support::endian::read32le(take(4).data()));
    break;
  case 'I':
    O.Type = Object::Int;
    O.Number =
        std::bit_cast<int64_t>(support::endian::read64le(take(8).data()));
    break;
  case 'l': {
    int64_t N = int32_t(support::endian::read32le(take(4).data()));
    unsigned Digits = N < 0 ? -N : N;
    // Decimal conversion is superlinear. Keep exceptionally large constants
    // explicit.
    if (Digits > 256)
      unsupported(At, "Large integer exceeds the numeric rendering profile");
    APInt Integer(std::max(1u, Digits * 15), 0);
    for (unsigned I = 0; I < Digits; ++I) {
      unsigned D = support::endian::read16le(take(2).data());
      if (D >= 32768 || (I + 1 == Digits && !D))
        malformed(At, "Invalid marshal integer digit");
      Integer |= APInt(Integer.getBitWidth(), D).shl(I * 15);
    }
    SmallString<64> S;
    Integer.toStringUnsigned(S);
    O.Type = Object::Long;
    O.Digits = (N < 0 ? "-" : "") + S.str().str();
    break;
  }
  case 'f':
  case 'g':
    O.Type = Object::Float;
    O.Real = Tag == 'f' ? TextFloat() : BinaryFloat();
    break;
  case 'x':
  case 'y':
    O.Type = Object::Complex;
    if (Tag == 'x') {
      TextFloat();
      TextFloat();
    } else
      take(16);
    break;
  case 'S':
    O.Type = Object::StopIteration;
    break;
  case ':':
    if (Version < 314)
      unsupported(At, "Slice constant predates marshal slice support");
    O.Type = Object::Slice;
    for (unsigned I = 0; I < 3; ++I)
      O.Items.push_back(object(Depth + 1));
    break;
  case 's':
  case 'u':
  case 'a':
  case 'A':
  case 'z':
  case 'Z':
  case 't': {
    unsigned N = Tag == 'z' || Tag == 'Z' ? uint8_t(take(1).front()) : count();
    O.Type = Tag == 's' || (Tag == 't' && Version < 300) ? Object::Bytes
                                                         : Object::Unicode;
    if (Tag == 't' && Version >= 204 && Version < 300)
      Interned.push_back(&O);
    O.DataOffset = Position;
    O.Data = take(N);
    if ((Tag == 'a' || Tag == 'A' || Tag == 'z' || Tag == 'Z') &&
        llvm::any_of(O.Data, [](unsigned char C) { return C >= 128; }))
      malformed(At, "Non-ASCII byte in marshal ASCII string");
    if (O.Type == Object::Unicode && !json::isUTF8(O.Data))
      unsupported(At, "Non-scalar Unicode string in marshal data");
    break;
  }
  case '(':
  case ')':
  case '[':
  case '<':
  case '>': {
    unsigned N = Tag == ')' ? uint8_t(take(1).front()) : count();
    J.charge(J.Objects, N, limits::kObjects, At);
    if (N > Input.size() - Position)
      malformed(Position, "Truncated marshal sequence");
    O.Type = Tag == '['   ? Object::List
             : Tag == '<' ? Object::Set
             : Tag == '>' ? Object::FrozenSet
                          : Object::Tuple;
    O.Items.reserve(N);
    for (unsigned I = 0; I < N; ++I)
      O.Items.push_back(object(Depth + 1));
    break;
  }
  case '{':
    O.Type = Object::Dict;
    for (;;) {
      Object *Key = object(Depth + 1, true);
      if (Key->Type == Object::End)
        break;
      O.Items.push_back(Key);
      O.Items.push_back(object(Depth + 1));
    }
    break;
  case 'C':
  case 'c':
    if ((Tag == 'C') != (Version <= 102))
      malformed(At, "Code type tag conflicts with release layout");
    O.Type = Object::CodeObject;
    code(O, Depth);
    break;
  default:
    unsupported(At, "Unqualified marshal type tag " + std::to_string(Tag));
  }
  O.EndOffset = Position;
  if (Register)
    References[Index] = &O;
  return &O;
}
Object &Marshal::read() {
  Object &Root = *object(0);
  if (Position != Input.size())
    malformed(Position, "Trailing bytes after marshal root");
  return Root;
}
