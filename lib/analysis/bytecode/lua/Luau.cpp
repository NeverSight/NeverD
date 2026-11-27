//===- Luau.cpp - Versioned Luau bytecode inventory ===//
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
void typeInfo(Reader &R, Proto &P, unsigned Types, int Version) {
  unsigned Signature = Types == 1 ? R.left() : R.count(R.varint());
  unsigned Upvalues = Types == 1 ? 0 : R.count(R.varint());
  unsigned Locals = Types == 1 ? 0 : R.count(R.varint());
  llvm::json::Array Params, Up, Variables;
  auto Type = [&] {
    unsigned Tag = R.byte(), Base = Tag & 127;
    unsigned Last = Version >= 10 ? 12 : Version >= 8 ? 10 : 9;
    if (Base > Last && Base != 15 && !(Types == 3 && Base >= 64 && Base < 96))
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
void neverd::lua::readLuau(Chunk &C, Reader &R, bool Pinned) {
  auto &E = C.Evidence;
  E["dialect"] = "Luau";
  C.Version = R.byte();
  E["bytecode_version"] = C.Version;
  E["reader_release"] = Pinned ? "0.659" : "0.501–0.748";
  if (Pinned)
    E["reader_revision"] = "f8a1e0129dce62c8c07ed5828781438e74452671";
  if (C.Version < 1 || C.Version > 14 || (Pinned && C.Version != 6))
    unsupported(0, "Unsupported serialized Luau version");
  unsigned Types = C.Version >= 4 ? R.byte() : 0;
  E["type_version"] =
      Types ? llvm::json::Value(Types) : llvm::json::Value(nullptr);
  if (C.Version >= 4 &&
      (!Types || Types > 3 || (C.Version == 4 && Types == 3) ||
       (Pinned && Types != 3)))
    unsupported(1, "Unsupported Luau type-information version");
  E["byte_order"] = "little";
  std::vector<llvm::StringRef> Strings;
  for (unsigned I = R.count(R.varint()); I; --I)
    Strings.push_back(R.take(R.varint()));
  auto String = [&](Reader &R, bool Nullable = false) {
    auto Id = R.varint();
    if (Id > Strings.size() || (!Id && !Nullable))
      malformed(R.at(), "Invalid Luau string index");
    return Id ? Strings[Id - 1] : llvm::StringRef();
  };
  llvm::json::Array UserTypes;
  while (Types == 3) {
    auto Id = R.byte();
    if (!Id)
      break;
    R.count(1);
    auto Name = String(R);
    C.Job.use(C.Job.Text, 4 * Name.size() + 2, R.at());
    UserTypes.push_back(llvm::json::Object{{"id", Id}, {"name", quote(Name)}});
  }
  E["userdata_types"] = std::move(UserTypes);
  auto ReadProto = [&](Reader &R) {
    auto &P = C.proto(R);
    P.Stack = R.byte();
    P.Params = R.byte();
    P.Up.resize(R.count(R.byte()));
    P.Vararg = R.boolean();
    if (C.Version >= 4)
      P.Flags = R.byte();
    auto TypeData = R.slice(Types ? R.varint() : 0);
    C.code(P, R, R.varint());
    if (TypeData.left())
      typeInfo(TypeData, P, Types, C.Version);
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
        K = {Constant::String, 0, 0, String(R)};
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
      case 8:
        if (Tag == 8 && C.Version < 7)
          malformed(R.at() - 1, "Table template predates its format version");
        for (unsigned J = R.count(R.varint()); J; --J) {
          if (R.varint() >= P.K.size())
            malformed(R.at(), "Invalid constant table key");
          if (Tag == 8 && int32_t(R.word(4)) >= int(P.K.size()))
            malformed(R.at() - 4, "Invalid table initializer constant");
        }
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
      case 11:
        if (C.Version < (Tag == 7 ? 5 : 13))
          malformed(R.at() - 1, "Vector constant predates its format version");
        K = {Constant::Vector, 0, 0, R.take(Tag == 7 ? 16 : 32)};
        break;
      case 9: {
        if (C.Version < 8)
          malformed(R.at() - 1, "Integer constant predates its format version");
        bool Negative = R.boolean();
        auto Magnitude = R.varint(true, false, 64);
        if (Magnitude > uint64_t(INT64_MAX) + Negative)
          malformed(R.at(), "Luau integer exceeds its signed range");
        K = {Constant::Integer,
             std::bit_cast<int64_t>(Negative ? 0 - Magnitude : Magnitude)};
        break;
      }
      case 10: {
        if (C.Version < 10)
          malformed(R.at() - 1, "Class shape predates its format version");
        auto Name = [&] {
          auto Index = R.varint();
          if (Index >= P.K.size() || P.K[Index].Type != Constant::String)
            malformed(R.at(), "Class shape requires a string constant");
        };
        Name();
        unsigned Fields = R.count(R.varint()), Methods = R.count(R.varint());
        for (unsigned J = Fields + Methods; J; --J)
          Name();
        K = {Constant::ClassShape};
        break;
      }
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
    if (C.Version >= 2)
      P.FirstLine = R.varint();
    P.Name = String(R, true);
    if (R.boolean())
      readLines(R, P);
    if (R.boolean()) {
      for (unsigned I = R.count(R.varint()); I; --I) {
        auto Name = String(R);
        unsigned Start = R.varint(), End = R.varint();
        auto Reg = R.byte();
        if (Reg >= P.Stack || Start > End || End > P.Code.size())
          malformed(R.at(), "Invalid debug local");
        P.Locals.push_back({Name, Start, End, Reg});
      }
      if (R.varint() != P.Up.size())
        malformed(R.at(), "Invalid debug upvalue count");
      for (auto &U : P.Up)
        U.Name = String(R);
    }
    if (C.Version >= 11) {
      auto Count = R.count(R.varint());
      for (unsigned J = 0; J < Count; ++J) {
        if (R.byte() != 0)
          unsupported(R.at() - 1, "Unqualified feedback slot kind");
        if (R.varint() >= P.Code.size())
          malformed(R.at(), "Feedback PC outside prototype");
      }
      P.FeedbackSlots = Count;
    }
    if (C.Version >= 12 && (P.Flags & 8))
      P.InlineCost = R.varint(true, false, 64);
    P.Complete = true;
    P.End = R.at();
  };
  for (unsigned N = R.count(R.varint()); N; --N) {
    if (C.Version < 12) {
      ReadProto(R);
      continue;
    }
    auto Body = R.slice(R.varint());
    ReadProto(Body);
    Body.end();
  }
  C.Root = R.varint();
  if (C.Root >= C.Functions.size())
    malformed(R.at(), "Invalid Luau entry prototype");
  R.end();
}
