//===- Puc.cpp - PUC Lua binary chunks ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The serialized layouts, numeric tags and debug tables are selected by chunk
/// version.
//===----------------------------------------------------------------------===//

#include "Chunk.h"

#include <algorithm>
#include <climits>

using namespace neverd::lua;
namespace {
class PucReader {
  Chunk &C;
  Reader &R;
  unsigned V, SizeT = 0, Integer = 8, Number = 8, WordBytes = 4;
  bool DebugFirst;
  std::vector<llvm::StringRef> Strings;
  uint64_t size() {
    return V < 0x54 ? R.word(4) : R.varint(false, V == 0x54, 64);
  }
  unsigned index() {
    auto N = size();
    if (N > INT_MAX)
      malformed(R.at(), "Lua index exceeds int range");
    return N;
  }
  unsigned count() { return R.count(size()); }
  llvm::StringRef string() {
    uint64_t N;
    if (V <= 0x52)
      N = R.word(SizeT);
    else if (V == 0x53) {
      N = R.byte();
      if (N == 255)
        N = R.word(SizeT);
    } else
      N = size();
    if (!N) {
      if (V != 0x55)
        return {};
      auto Ref = size();
      if (Ref > Strings.size())
        malformed(R.at(), "Invalid shared string index");
      return Ref ? Strings[Ref - 1] : llvm::StringRef();
    }
    auto S = R.take(N - 1);
    if ((V <= 0x52 || V == 0x55) && R.byte() != 0)
      malformed(R.at() - 1, "Missing string terminator");
    if (V == 0x55) {
      R.count(1);
      Strings.push_back(S);
    }
    return S;
  }
  Constant constant() {
    auto T = R.byte();
    if (!T)
      return {Constant::Nil};
    if (T == 1 || (T == 17 && V >= 0x54))
      return {Constant::Boolean, V >= 0x54 ? T == 17 : R.boolean()};
    if (T == 4 || (T == 20 && V >= 0x53)) {
      auto S = string();
      if (!S.data())
        malformed(R.at(), "Missing constant string");
      return {Constant::String, 0, 0, S};
    }
    if (T == (V >= 0x54 ? 19 : 3))
      return {Constant::Number, 0, std::bit_cast<double>(R.word(Number))};
    if (V >= 0x53 && T == (V == 0x53 ? 19 : 3)) {
      uint64_t Bits = V == 0x55 ? R.varint(false, false, 64) : R.word(Integer);
      if (V == 0x55)
        Bits = (Bits >> 1) ^ (uint64_t(0) - (Bits & 1));
      return {Constant::Integer, std::bit_cast<int64_t>(Bits)};
    }
    malformed(R.at() - 1, "Unknown constant tag");
  }
  void lines(Proto &P) {
    auto N = count();
    if (N && N != P.Code.size())
      malformed(R.at(), "Line table does not cover the code");
    if (V < 0x54) {
      for (unsigned I = 0; I < N; ++I)
        P.Lines.push_back(index());
      return;
    }
    auto Deltas = R.take(N);
    auto Anchors = count();
    if (Anchors && V == 0x55)
      R.align(4);
    // Absolute anchors replace the signed -128 escape at the same PC.
    unsigned Seen = 0;
    int64_t Line = P.FirstLine;
    for (unsigned PC = 0; PC < N; ++PC) {
      int Delta = int8_t(Deltas[PC]);
      if (Delta == -128) {
        if (++Seen > Anchors)
          malformed(R.at(), "Missing absolute line");
        auto PcValue = V == 0x55 ? R.word(4) : index();
        if (PcValue != PC)
          malformed(R.at(), "Invalid absolute line PC");
        Line = V == 0x55 ? R.word(4) : index();
      } else
        Line += Delta;
      if (Line < 0 || Line > INT_MAX)
        malformed(R.at(), "Invalid source line");
      P.Lines.push_back(Line);
    }
    if (Seen != Anchors)
      malformed(R.at(), "Unused absolute line anchors");
  }
  Proto &function(unsigned Depth, llvm::StringRef ParentSource = {}) {
    if (Depth > limits::kDepth)
      throw Failure{"budget_exceeded",
                    "Prototype nesting exceeds recovery budget", R.at()};
    auto &P = C.proto(R);
    if (V != 0x52 && V != 0x55) {
      P.SourceName = string();
      if (!P.SourceName.data())
        P.SourceName = ParentSource;
    }
    P.FirstLine = index();
    if (V != 0x50)
      P.LastLine = index();
    if (V <= 0x51)
      P.Up.resize(R.count(R.byte()));
    P.Params = R.byte();
    P.Flags = R.byte();
    if (P.Flags & ~(V == 0x51 || V == 0x55 ? 7u : 1u))
      unsupported(R.at() - 1, "Unknown prototype flags");
    P.Vararg = V == 0x51 ? (P.Flags & 2) : V == 0x55 ? (P.Flags & 3) : P.Flags;
    P.Stack = R.byte();
    P.WordBytes = WordBytes;
    if (DebugFirst) {
      for (unsigned I = count(); I; --I)
        P.Lines.push_back(index());
      localNames(P);
      upvalueNames(P);
      for (unsigned I = count(); I; --I)
        P.K.push_back(constant());
      for (unsigned I = count(); I; --I)
        P.Children.push_back(function(Depth + 1, P.SourceName).Id);
      C.code(P, R, size());
      if (!P.Lines.empty() && P.Lines.size() != P.Code.size())
        malformed(P.CodeAt, "Line table does not cover the code");
      for (const auto &L : P.Locals)
        if (L.End > P.Code.size())
          malformed(P.CodeAt, "Invalid local lifetime");
      P.End = R.at();
      P.Complete = true;
      return P;
    }
    auto N = size();
    if (V == 0x55)
      R.align(4);
    C.code(P, R, N);
    for (unsigned I = count(); I; --I)
      P.K.push_back(constant());
    auto Children = [&] {
      for (unsigned I = count(); I; --I)
        P.Children.push_back(function(Depth + 1, P.SourceName).Id);
    };
    if (V <= 0x52)
      Children();
    if (V >= 0x52) {
      for (unsigned I = count(); I; --I) {
        auto Local = R.boolean();
        auto Id = R.byte();
        P.Up.push_back({Local, Id, V >= 0x54 ? unsigned(R.byte()) : 0});
      }
    }
    if (V >= 0x53)
      Children();
    if (V == 0x52 || V == 0x55)
      P.SourceName = string();
    if (V != 0x51)
      for (auto Id : P.Children)
        for (const auto &Up : C.Functions[Id].Up)
          if (Up.Index >= (Up.Local ? P.Stack : P.Up.size()))
            malformed(R.at(), "Invalid parent capture index");
    lines(P);
    localNames(P);
    upvalueNames(P);
    P.End = R.at();
    P.Complete = true;
    return P;
  }
  void localNames(Proto &P) {
    for (unsigned I = count(); I; --I) {
      auto S = string();
      unsigned Begin = index(), End = index();
      if (Begin > End || (!DebugFirst && End > P.Code.size()))
        malformed(R.at(), "Invalid local lifetime");
      P.Locals.push_back({S, Begin, End});
    }
  }
  void upvalueNames(Proto &P) {
    auto Names = count();
    if (Names && Names != P.Up.size())
      malformed(R.at(), "Invalid upvalue name count");
    for (unsigned I = 0; I < Names; ++I)
      P.Up[I].Name = string();
  }
  void detectEndian(unsigned Width, uint64_t Expected) {
    auto Probe = R.slice(Width);
    if (Probe.word(Width) == Expected)
      return;
    Probe.Pos = 0;
    Probe.Big = true;
    if (Probe.word(Width) != Expected)
      unsupported(Probe.Base, "Unrecognised integer representation");
    R.Big = true;
  }

public:
  PucReader(Chunk &C, Reader &R, unsigned V, bool Early51)
      : C(C), R(R), V(V), DebugFirst(V == 0x50 || Early51) {}
  void run() {
    auto &E = C.Evidence;
    if (V >= 0x51) {
      unsigned Format = R.byte();
      E["format"] = Format;
      if (Format)
        unsupported(R.at() - 1, "Custom PUC Lua format");
    }
    constexpr llvm::StringLiteral Marker("\x19\x93\x0d\x0a\x1a\x0a");
    if (V >= 0x53)
      R.expect(Marker);
    if (V <= 0x52) {
      R.Big = !R.boolean();
      E["byte_order"] = R.Big ? "big" : "little";
    }
    if (V <= 0x53) {
      unsigned Int = R.byte();
      E["c_int_bytes"] = Int;
      if (Int != 4)
        unsupported(R.at() - 1, "Unsupported C int width");
      SizeT = R.byte();
      E["size_t_bytes"] = SizeT;
      if (SizeT != 4 && SizeT != 8)
        unsupported(R.at() - 1, "Unsupported size_t width");
    } else if (V == 0x55) {
      if (R.byte() != 4)
        unsupported(R.at() - 1, "Unsupported C int width");
      detectEndian(4, uint32_t(-0x5678));
    }
    WordBytes = R.byte();
    E["instruction_bytes"] = WordBytes;
    if (WordBytes != 4 && !(V == 0x50 && WordBytes == 8))
      unsupported(R.at() - 1, "Unsupported instruction width");
    if (V == 0x55 && R.word(4) != 0x12345678)
      unsupported(R.at() - 4, "Unsupported instruction representation");
    if (V >= 0x53)
      Integer = R.byte();
    if (V == 0x55 && (Integer != 8 || R.word(8) != uint64_t(-0x5678)))
      unsupported(R.at(), "Unsupported Lua integer representation");
    if (V == 0x50)
      R.expect(llvm::StringRef("\x06\x08\x09\x09", 4));
    Number = R.byte();
    E["number_bytes"] = Number;
    if (V >= 0x53)
      E["integer_bytes"] = Integer;
    if (Integer != 8 || Number != 8)
      unsupported(
          R.at(),
          "Only binary64 numbers and native 64-bit Lua integers are qualified");
    if (V == 0x50) {
      if (std::bit_cast<double>(R.word(Number)) != 3.14159265358979323846e7)
        unsupported(R.at() - Number,
                    "Unsupported Lua 5.0 number representation");
    } else if (V <= 0x52) {
      auto Integral = R.byte();
      E["integral_number"] = bool(Integral);
      if (Integral)
        unsupported(R.at() - 1, "Integral lua_Number build");
      if (V == 0x52)
        R.expect(Marker);
    } else {
      if (V != 0x55)
        detectEndian(Integer, 0x5678);
      if (std::bit_cast<double>(R.word(Number)) != (V == 0x55 ? -370.5 : 370.5))
        unsupported(R.at() - Number, "Unsupported number representation");
    }
    E["byte_order"] = R.Big ? "big" : "little";
    int UpCount = V >= 0x53 ? R.byte() : -1;
    auto &Root = function(0);
    if (UpCount >= 0 && size_t(UpCount) != Root.Up.size())
      malformed(R.at(), "Root upvalue count mismatch");
    R.end();
  }
};
} // namespace
void neverd::lua::readPuc(Chunk &C, Reader &R, llvm::StringRef Profile) {
  R.expect("\x1bLua");
  auto V = R.byte();
  C.Version = V;
  C.Evidence["version_byte"] = V;
  if (Profile.starts_with("luajit-1.")) {
    if (V != 0x51)
      unsupported(R.at() - 1,
                  "Selected LuaJIT 1.x profile requires a Lua 5.1 chunk");
    C.Evidence["declared_runtime"] = Profile;
    if (Profile == "luajit-1.0")
      C.Evidence["prototype_layout"] = "Lua 5.1 alpha";
  }
  if (V == 0x23 || V == 0x25 || V == 0x31 || V == 0x32 || V == 0x40) {
    readStackChunk(C, R);
    return;
  }
  if (V < 0x50 || V > 0x55)
    unsupported(R.at() - 1, "Unknown PUC Lua version");
  C.Evidence["dialect"] = "Lua 5." + std::to_string(V - 0x50);
  if (V != 0x51 || Profile != "auto") {
    PucReader(C, R, V, Profile == "luajit-1.0").run();
    return;
  }
  // Lua 5.1 alpha and the final release have the same ABI signature.
  // Try the earlier record order only after the stable layout fails.
  auto Original = R;
  try {
    PucReader(C, R, V, false).run();
  } catch (const Failure &Stable) {
    if (C.Functions.empty() || llvm::StringRef(Stable.Status) == "cancelled")
      throw;
    Chunk Early{C.Job, Family::Puc, C.Evidence, V};
    try {
      PucReader(Early, Original, V, true).run();
    } catch (const Failure &Alternative) {
      if (llvm::StringRef(Alternative.Status) == "cancelled")
        throw;
      throw Stable;
    }
    C.Functions = std::move(Early.Functions);
    C.Evidence["prototype_layout"] = "Lua 5.1 alpha";
    R.Pos = Original.Pos;
  }
}
