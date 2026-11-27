//===- Luau.cpp - Luau bytecode v6 inventory ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Only an explicit upstream compiler profile admits this signatureless format.
//===----------------------------------------------------------------------===//

#include "Chunk.h"

#include <climits>

using namespace neverd::lua;
namespace {
void typeInfo(Reader &R, Proto &P) {
  unsigned Signature = R.count(R.varint()), Upvalues = R.count(R.varint()),
           Locals = R.count(R.varint());
  llvm::json::Array Params, Up, Variables;
  auto Type = [&] {
    unsigned Tag = R.byte(), Base = Tag & 127;
    if (Base > 9 && Base != 15 && !(Base >= 64 && Base < 96))
      unsupported(R.at() - 1, "Unknown Luau type tag");
    return Tag;
  };
  if (Signature) {
    if (Signature != P.Params + 2 || R.byte() != 5 || R.byte() != P.Params)
      malformed(R.at(), "Invalid function type signature");
    for (unsigned N = 0; N < P.Params; ++N)
      Params.push_back(Type());
  }
  if (Upvalues && Upvalues != P.Up.size())
    malformed(R.at(), "Invalid typed upvalue count");
  for (unsigned N = 0; N < Upvalues; ++N)
    Up.push_back(Type());
  for (unsigned N = 0; N < Locals; ++N) {
    unsigned Tag = Type(), Slot = R.byte();
    auto Start = R.varint(), End = Start + R.varint();
    if (Slot >= P.Stack || End > P.Code.size())
      malformed(R.at(), "Invalid typed local range");
    Variables.push_back(llvm::json::Object{{"register", Slot},
                                           {"type", Tag},
                                           {"start_pc", Start},
                                           {"end_pc", End}});
  }
  R.end();
  P.Types["parameters"] = std::move(Params);
  P.Types["upvalues"] = std::move(Up);
  P.Types["locals"] = std::move(Variables);
}
void readLines(Reader &R, Proto &P) {
  auto Shift = R.byte();
  if (Shift >= 32)
    malformed(R.at() - 1, "Invalid line table interval");
  uint8_t Offset = 0;
  for (unsigned PC = 0; PC < P.Code.size(); ++PC) {
    Offset += R.byte();
    P.Lines.push_back(Offset);
  }
  int64_t Base = 0;
  unsigned Mask = (1u << Shift) - 1;
  for (unsigned PC = 0; PC < P.Lines.size(); ++PC) {
    if (!(PC & Mask))
      Base += int32_t(R.word(4));
    int64_t Line = Base + P.Lines[PC];
    if (Line < 0 || Line > INT_MAX)
      malformed(R.at(), "Invalid debug line");
    P.Lines[PC] = Line;
  }
}
} // namespace
void neverd::lua::readLuau(Chunk &C, Reader &R) {
  auto &E = C.Evidence;
  E["dialect"] = "Luau";
  C.Version = R.byte();
  E["bytecode_version"] = C.Version;
  E["reader_release"] = "0.659";
  E["reader_revision"] = "f8a1e0129dce62c8c07ed5828781438e74452671";
  if (C.Version != 6)
    unsupported(0, "Unsupported serialized Luau version");
  auto Types = R.byte();
  E["type_version"] = Types;
  if (Types != 3)
    unsupported(1, "Unsupported Luau type-information version");
  E["byte_order"] = "little";
  std::vector<llvm::StringRef> Strings;
  for (unsigned I = R.count(R.varint()); I; --I)
    Strings.push_back(R.take(R.varint()));
  auto String = [&](bool Nullable = false) {
    auto Id = R.varint();
    if (Id > Strings.size() || (!Id && !Nullable))
      malformed(R.at(), "Invalid Luau string index");
    return Id ? Strings[Id - 1] : llvm::StringRef();
  };
  llvm::json::Array UserTypes;
  while (auto Id = R.byte()) {
    R.count(1);
    auto Name = String();
    C.Job.use(C.Job.Text, 4 * Name.size() + 2, R.at());
    UserTypes.push_back(llvm::json::Object{{"id", Id}, {"name", quote(Name)}});
  }
  E["userdata_types"] = std::move(UserTypes);
  for (unsigned N = R.count(R.varint()); N; --N) {
    auto &P = C.proto(R);
    P.Stack = R.byte();
    P.Params = R.byte();
    P.Up.resize(R.count(R.byte()));
    P.Vararg = R.boolean();
    P.Flags = R.byte();
    auto Types = R.slice(R.varint());
    C.code(P, R, R.varint());
    if (Types.left())
      typeInfo(Types, P);
    for (unsigned I = R.count(R.varint()); I; --I) {
      unsigned Tag = R.byte();
      Constant K{Constant::Nil};
      switch (Tag) {
      case 0:
        break;
      case 1:
        K = {Constant::Boolean, R.boolean()};
        break;
      case 2:
        K = {Constant::Number, 0, std::bit_cast<double>(R.word(8))};
        break;
      case 3:
        K = {Constant::String, 0, 0, String()};
        break;
      case 4: {
        uint32_t Word = R.word(4);
        auto Parts = Word >> 30;
        if (!Parts)
          malformed(R.at(), "Empty import path");
        for (unsigned J = 0; J < Parts; ++J) {
          auto Id = (Word >> (20 - 10 * J)) & 1023;
          if (Id >= P.K.size() || P.K[Id].Type != Constant::String)
            malformed(R.at(), "Invalid import path component");
        }
        K = {Constant::Import, Word};
        break;
      }
      case 5:
        for (unsigned J = R.count(R.varint()); J; --J)
          if (R.varint() >= P.K.size())
            malformed(R.at(), "Invalid constant table key");
        K = {Constant::Table};
        break;
      case 6: {
        auto Id = R.varint();
        if (Id >= P.Id)
          malformed(R.at(), "Invalid closure constant");
        K = {Constant::Closure, int64_t(Id)};
        break;
      }
      case 7:
        K = {Constant::Vector, 0, 0, R.take(16)};
        break;
      default:
        malformed(R.at() - 1, "Unknown Luau constant tag");
      }
      P.K.push_back(K);
    }
    for (unsigned I = R.count(R.varint()); I; --I) {
      auto Id = R.varint();
      if (Id >= P.Id)
        malformed(R.at(), "Invalid nested prototype");
      P.Children.push_back(Id);
    }
    P.FirstLine = R.varint();
    P.Name = String(true);
    if (R.boolean())
      readLines(R, P);
    if (R.boolean()) {
      for (unsigned I = R.count(R.varint()); I; --I) {
        auto Name = String();
        unsigned Start = R.varint(), End = R.varint();
        auto Reg = R.byte();
        if (Reg >= P.Stack || Start > End || End > P.Code.size())
          malformed(R.at(), "Invalid debug local");
        P.Locals.push_back({Name, Start, End, Reg});
      }
      if (R.varint() != P.Up.size())
        malformed(R.at(), "Invalid debug upvalue count");
      for (auto &U : P.Up)
        U.Name = String();
    }
    P.Complete = true;
    P.End = R.at();
  }
  C.Root = R.varint();
  if (C.Root >= C.Functions.size())
    malformed(R.at(), "Invalid Luau entry prototype");
  R.end();
}
