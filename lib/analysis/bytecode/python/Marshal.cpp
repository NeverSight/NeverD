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
  C.Args = count();
  C.PosOnly = count();
  C.KwOnly = count();
  if (Version == 310)
    C.Locals = count();
  C.Stack = count();
  C.Flags = count();
  auto Field = [&](Object::Kind Expected) {
    Object *Field = object(Depth + 1);
    if (Field->Type != Expected)
      malformed(Field->Offset, "Invalid CPython code field type");
    return Field;
  };
  C.Bytes = Field(Object::Bytes);
  C.Consts = Field(Object::Tuple);
  auto Names = [&] {
    auto *Tuple = Field(Object::Tuple);
    if (!llvm::all_of(Tuple->Items, [](const Object *V) {
          return V->Type == Object::Unicode;
        }))
      malformed(Tuple->Offset, "Non-string name in code object");
    for (const auto *Name : Tuple->Items)
      J.charge(J.Text, Name->Data.size(), limits::kTextBytes, Name->Offset);
    return Tuple;
  };
  C.Names = Names();
  C.Vars = Names();
  if (Version == 310) {
    C.Free = Names();
    C.Cells = Names();
    if (C.Locals != C.Vars->Items.size())
      malformed(O.Offset, "Inconsistent local name count");
  } else {
    // 3.11 stores fast locals, cells and free variables in one indexed table.
    C.LocalKinds = Field(Object::Bytes);
    if (C.LocalKinds->Data.size() != C.Vars->Items.size())
      malformed(C.LocalKinds->Offset,
                "Local kinds do not match localsplus names");
    C.Locals = 0;
    for (unsigned char Kind : C.LocalKinds->Data) {
      if (Kind != 0x20 && Kind != 0x40 && Kind != 0x60 && Kind != 0x80)
        malformed(C.LocalKinds->Offset, "Unknown localsplus kind");
      C.Locals += bool(Kind & 0x20);
    }
  }
  C.Filename = Field(Object::Unicode);
  C.Name = Field(Object::Unicode);
  if (Version == 311)
    C.QualifiedName = Field(Object::Unicode);
  J.charge(J.Text,
           C.Name->Data.size() + C.Filename->Data.size() +
               (C.QualifiedName ? C.QualifiedName->Data.size() : 0),
           limits::kTextBytes, O.Offset);
  C.FirstLine = count();
  C.Lines = Field(Object::Bytes);
  if (Version == 311)
    C.Exceptions = Field(Object::Bytes);
  uint64_t Params =
      uint64_t(C.Args) + C.KwOnly + bool(C.Flags & 4) + bool(C.Flags & 8);
  if (C.PosOnly > C.Args || Params > C.Locals || C.Bytes->Data.empty() ||
      C.Bytes->Data.size() % 2)
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
  if (Tag == 'r') {
    unsigned Id = count();
    if (Register || Id >= References.size())
      malformed(At, "Invalid marshal reference");
    if (!References[Id])
      unsupported(At, "Recursive marshal value");
    return References[Id];
  }
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
  case 'g':
    O.Type = Object::Float;
    O.Real = std::bit_cast<double>(support::endian::read64le(take(8).data()));
    break;
  case 's':
  case 'u':
  case 'a':
  case 'A':
  case 'z':
  case 'Z':
  case 't': {
    unsigned N = Tag == 'z' || Tag == 'Z' ? uint8_t(take(1).front()) : count();
    O.Type = Tag == 's' ? Object::Bytes : Object::Unicode;
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
  case 'c':
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
