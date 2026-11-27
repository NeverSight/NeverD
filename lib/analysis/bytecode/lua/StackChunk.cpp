//===- StackChunk.cpp - PUC Lua stack-machine chunk formats --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read variable-width stack bytecode, relocation chains and the Lua 4 word
/// format. Keep the disk bytes intact when resolving a relocation.
//===----------------------------------------------------------------------===//

#include "Chunk.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

#include <bit>

using namespace llvm;
using namespace neverd::lua;

namespace {
enum class Encoding {
  None,
  Byte,
  Word,
  BytePair,
  WordByte,
  Real,
  Pointer,
  Record,
  Native
};
struct DiskOp {
  StringRef Name, Operation;
  Encoding Fields;
  unsigned Implicit;
};
#include "StackOpcodes.def"

struct StackReader {
  Chunk &C;
  Reader &R;
  int V;
  unsigned SizeT = 4, Number = 8, WordBytes = 4, PointerBytes = 4;
  bool NativeNumber = true, NumberBig = false;

  uint64_t size(unsigned Width) { return R.word(Width); }
  unsigned count(unsigned Width) { return R.count(size(Width)); }
  StringRef string(unsigned Width) {
    uint64_t Bytes = size(Width);
    if (!Bytes)
      return {};
    auto S = R.take(Bytes);
    if (S.back() != '\0')
      malformed(R.at() - 1, "Missing legacy string terminator");
    return S.drop_back();
  }
  double numeric() {
    if (!NativeNumber) {
      auto S = R.take(R.byte());
      double Value;
      if (S.getAsDouble(Value))
        malformed(R.at() - S.size(), "Invalid portable Lua number");
      return Value;
    }
    bool Saved = R.Big;
    R.Big = NumberBig;
    auto Bits = R.word(Number);
    R.Big = Saved;
    return Number == 4 ? std::bit_cast<float>(uint32_t(Bits))
                       : std::bit_cast<double>(Bits);
  }
  void probe(double Expected) {
    double Rounded = Number == 4 ? double(float(Expected)) : Expected;
    auto Start = R.Pos;
    if (numeric() == Rounded)
      return;
    if (V == 0x32) {
      R.Pos = Start;
      NumberBig = !NumberBig;
      if (numeric() == Rounded)
        return;
    }
    unsupported(R.Base + Start, "Unqualified legacy number representation");
  }
  void header() {
    if (V <= 0x25) {
      if (V == 0x25) {
        if (R.byte() != 2 || R.byte() != 4)
          unsupported(R.at(), "Unsupported word or float representation");
        PointerBytes = R.byte();
        if (PointerBytes != 4 && PointerBytes != 8)
          unsupported(R.at() - 1, "Unsupported function reference width");
      }
      auto Marker = R.word(2);
      if (Marker != 0x1234 && Marker != 0x3412)
        malformed(R.at() - 2, "Invalid legacy byte-order marker");
      R.Big = Marker == 0x3412;
      NumberBig = R.Big;
      Number = 4;
      probe(0.123456789e-23);
    } else if (V == 0x40) {
      R.Big = !R.boolean();
      if (R.byte() != 4)
        unsupported(R.at() - 1, "Unsupported Lua 4 count width");
      SizeT = R.byte();
      WordBytes = R.byte();
      if ((SizeT != 4 && SizeT != 8) || (WordBytes != 4 && WordBytes != 8))
        unsupported(R.at() - 2, "Unsupported Lua 4 native field width");
      R.expect(StringRef("\x20\x06\x09", 3));
      Number = R.byte();
      if (Number != 4 && Number != 8)
        unsupported(R.at() - 1, "Unsupported Lua 4 number width");
      NumberBig = R.Big;
      probe(3.14159265358979323846e8);
    } else {
      R.Big = true;
      if (V == 0x31) {
        unsigned Format = R.byte();
        Number = R.byte();
        if (!((Format == 'd' && Number == 8) || (Format == 'f' && Number == 4)))
          unsupported(R.at() - 2, "Unsupported Lua 3.1 numeric signature");
        NumberBig = true;
        probe(3.14159265358979323846e8);
      } else {
        Number = R.byte();
        NativeNumber = Number != 0;
        if (NativeNumber) {
          if (Number != 4 && Number != 8)
            unsupported(R.at() - 1, "Unsupported Lua 3.2 number width");
          probe(3.14159265358979323846e8);
        }
      }
    }
    C.Evidence["byte_order"] = R.Big ? "big" : "little";
    if (NativeNumber)
      C.Evidence["number_bytes"] = Number;
    else
      C.Evidence["number_encoding"] = "decimal text";
    C.Evidence["instruction_unit_bytes"] = V == 0x40 ? WordBytes : 1;
  }

  void instructions(Proto &P) {
    ArrayRef<DiskOp> Table = V == 0x23   ? ArrayRef(Lua23)
                             : V == 0x25 ? ArrayRef(Lua25)
                             : V == 0x31 ? ArrayRef(Lua31)
                             : V == 0x32 ? ArrayRef(Lua32)
                                         : ArrayRef(Lua40);
    auto &Rows = P.Legacy->Instructions;
    unsigned Line = 0;
    for (unsigned PC = 0; PC < P.Code.size();) {
      C.Job.check(P.codeOffset(PC));
      uint32_t Raw = P.Code[PC];
      auto ID = V == 0x40 ? Raw & 63 : Raw;
      if (ID >= Table.size())
        unsupported(P.codeOffset(PC),
                    "Unknown legacy disk opcode " + std::to_string(ID));
      const auto &Op = Table[ID];
      unsigned Next = PC + 1;
      auto Take = [&](unsigned N, bool Big = false) {
        if (N > P.Code.size() - Next)
          malformed(P.codeOffset(PC), "Truncated stack bytecode operand");
        uint64_t Value = 0;
        for (unsigned I = 0; I < N; ++I)
          Value |= uint64_t(P.Code[Next++]) << (8 * (Big ? N - 1 - I : I));
        return Value;
      };
      int64_t Arg = Op.Implicit;
      unsigned Extra = 0;
      bool Big = V >= 0x31 || R.Big;
      switch (Op.Fields) {
      case Encoding::Byte:
        Arg = Take(1);
        break;
      case Encoding::Word:
        Arg = Take(2, Big);
        break;
      case Encoding::BytePair:
        Arg = Take(1);
        Extra = Take(1);
        break;
      case Encoding::WordByte:
        Arg = Take(2, Big);
        Extra = Take(1);
        break;
      case Encoding::Real:
        Arg = Take(4, Big);
        break;
      case Encoding::Pointer:
        Take(PointerBytes, Big);
        break;
      case Encoding::Record:
        Arg = Take(1);
        if (uint64_t(Arg) * 2 > P.Code.size() - Next)
          malformed(P.codeOffset(PC), "Truncated table-field operands");
        Next += 2 * Arg;
        break;
      case Encoding::Native:
        Arg = Raw >> 6;
        break;
      case Encoding::None:
        break;
      }
      Rows.push_back({Op.Name, Op.Operation, PC, Next - PC, Arg, Extra});
      if (Op.Operation == "SETLINE") {
        Line = Arg;
        if (P.Lines.empty())
          P.Lines.resize(P.Code.size());
      }
      if (Line)
        std::fill(P.Lines.begin() + PC, P.Lines.begin() + Next, Line);
      PC = Next;
    }
  }

  void flat() {
    while (R.left()) {
      R.expect("F");
      auto &P = C.proto(R);
      P.Legacy = std::make_unique<StackCode>();
      P.Legacy->HasFrame = false;
      P.WordBytes = 1;
      unsigned High = R.word(2);
      unsigned Length = (High << 16) | R.word(2);
      P.FirstLine = R.word(2);
      if (!P.Id && !P.FirstLine)
        P.SourceName = string(2);
      else if (P.Id && P.FirstLine) {
        auto Site = R.word(2);
        auto &Main = C.Functions.front();
        auto Binding =
            llvm::lower_bound(Main.Legacy->Instructions, Site,
                              [](const StackInstruction &I, unsigned At) {
                                return I.PC + 1 < At;
                              });
        if (Binding == Main.Legacy->Instructions.end() ||
            Binding->PC + 1 != Site || Binding->Operation != "PUSHFUNCTION" ||
            Binding->Operand)
          malformed(R.at() - 2, "Invalid function relocation");
        Binding->Operand = P.Id;
        Main.Children.push_back(P.Id);
        P.SourceName = Main.SourceName;
      } else
        malformed(P.Start, "Invalid legacy main function ordering");
      C.code(P, R, Length);
      instructions(P);
      DenseMap<unsigned, bool> Sites;
      for (const auto &I : P.Legacy->Instructions) {
        if (I.Operation == "PUSHGLOBAL" || I.Operation == "STOREGLOBAL")
          Sites[I.PC + 1] = true;
        else if (I.Operation == "PUSHSTRING" || I.Operation == "PUSHSELF")
          Sites[I.PC + 1] = false;
        else if (I.Operation == "STORERECORD")
          for (unsigned N = I.PC + 2; N < I.PC + I.Width; N += 2)
            Sites[N] = false;
      }
      DenseMap<unsigned, unsigned> Resolved;
      while (R.left() && (R.Bytes[R.Pos] == 'V' || R.Bytes[R.Pos] == 'S')) {
        bool Global = R.byte() == 'V';
        unsigned At = R.word(2);
        auto Name = string(2);
        if (!Name.data() || !At)
          malformed(R.at(), "Empty legacy symbol relocation");
        unsigned Index = P.K.size();
        R.Job.use(R.Job.Items, 1, R.at());
        P.K.push_back({Constant::String, 0, 0, Name});
        while (At) {
          auto Site = Sites.find(At);
          if (Site == Sites.end() || Site->second != Global ||
              !Resolved.try_emplace(At, Index).second)
            malformed(P.CodeAt, "Invalid or repeated relocation operand");
          unsigned A = P.Code[At], B = P.Code[At + 1];
          At = R.Big ? (A << 8) | B : A | (B << 8);
        }
      }
      if (Resolved.size() != Sites.size())
        malformed(P.CodeAt, "Unbound name operand");
      for (auto &I : P.Legacy->Instructions)
        if (auto It = Resolved.find(I.PC + 1); It != Resolved.end())
          I.Operand = It->second;
      if (P.Id) {
        auto Prologue =
            llvm::find_if(P.Legacy->Instructions, [](const auto &I) {
              return I.Operation != "SETLINE";
            });
        P.Legacy->HasParameters = Prologue != P.Legacy->Instructions.end() &&
                                  (Prologue->Operation == "ADJUST" ||
                                   Prologue->Operation == "ADJUST0");
        if (P.Legacy->HasParameters)
          P.Params = Prologue->Operand;
      }
      P.Stack = 255; // No frame size is stored in these streams.
      P.End = R.at();
      P.Complete = true;
    }
    if (C.Functions.empty())
      malformed(R.at(), "Missing legacy main function");
    for (const auto &I : C.Functions.front().Legacy->Instructions)
      if (I.Operation == "PUSHFUNCTION" && !I.Operand)
        malformed(C.Functions.front().codeOffset(I.PC),
                  "Unbound function relocation");
  }

  void lines40(Proto &P, Reader Runs) {
    if (!Runs.left())
      return;
    unsigned First = 0;
    int64_t Line = 0;
    bool Terminated = false;
    while (Runs.left()) {
      int32_t Entry = Runs.word(4);
      ++Line;
      if (Entry < 0) {
        Line -= int64_t(Entry);
        Entry = Runs.word(4);
      }
      if (Entry == INT32_MAX - 2) {
        Runs.end();
        Terminated = true;
        break;
      }
      if (Line > INT32_MAX || Entry < 0 || unsigned(Entry) >= P.Code.size() ||
          (P.Lines.empty() ? Entry != 0 : unsigned(Entry) <= First))
        malformed(Runs.at() - 4, "Invalid Lua 4 line boundary");
      First = Entry;
      if (P.Lines.size() <= First)
        P.Lines.resize(First + 1, P.Lines.empty() ? 0 : P.Lines.back());
      P.Lines[First] = Line;
    }
    if (!Terminated || P.Lines.empty())
      malformed(Runs.at(), "Incomplete Lua 4 line table");
    P.Lines.resize(P.Code.size(), P.Lines.back());
  }

  Proto &function(unsigned Depth) {
    if (Depth > limits::kDepth)
      throw Failure{"budget_exceeded",
                    "Prototype nesting exceeds recovery budget", R.at()};
    auto &P = C.proto(R);
    P.Legacy = std::make_unique<StackCode>();
    unsigned Width = V == 0x31 ? 2 : 4;
    if (V == 0x40) {
      P.SourceName = string(SizeT);
      P.FirstLine = R.word(4);
      auto Params = R.word(4);
      P.Vararg = R.boolean();
      auto Stack = R.word(4);
      if (Stack > 255 || Params > Stack)
        malformed(R.at(), "Invalid Lua 4 frame");
      P.Params = Params;
      P.Stack = Stack;
      for (unsigned N = count(4); N; --N) {
        auto Name = string(SizeT);
        unsigned Start = R.word(4), End = R.word(4);
        if (Start > End)
          malformed(R.at(), "Invalid local lifetime");
        P.Locals.push_back({Name, Start, End});
      }
      auto LineRuns = R.slice(uint64_t(count(4)) * 4);
      for (unsigned N = count(4); N; --N)
        P.K.push_back({Constant::String, 0, 0, string(SizeT)});
      P.Legacy->FirstNumber = P.K.size();
      for (unsigned N = count(4); N; --N)
        P.K.push_back({Constant::Number, 0, numeric()});
      for (unsigned N = count(4); N; --N)
        P.Children.push_back(function(Depth + 1).Id);
      P.WordBytes = WordBytes;
      C.code(P, R, R.word(4));
      lines40(P, LineRuns);
      if (P.Code.back() != 0)
        malformed(P.CodeAt, "Missing Lua 4 END");
      for (const auto &L : P.Locals)
        if (L.End > P.Code.size())
          malformed(P.CodeAt, "Invalid local lifetime");
    } else {
      P.FirstLine = R.word(Width);
      P.SourceName = string(Width);
      unsigned Bytes = R.word(4);
      if (Bytes < 3)
        malformed(R.at(), "Missing stack frame or instructions");
      P.Stack = R.byte();
      unsigned Args = R.byte(), Vararg = V == 0x31 ? 64 : 128;
      P.Vararg = Args >= Vararg;
      P.Params = P.Vararg ? Args - Vararg : Args;
      P.WordBytes = 1;
      C.code(P, R, Bytes - 2);
      if (P.Code.back() != 0)
        malformed(P.CodeAt, "Missing legacy ENDCODE");
      for (unsigned N = count(Width); N; --N) {
        unsigned Line = R.word(Width);
        P.Legacy->LocalEvents.emplace_back(Line, string(Width));
      }
      for (unsigned N = count(Width); N; --N) {
        switch (R.byte()) {
        case 1:
          P.K.push_back({Constant::Number, 0, numeric()});
          break;
        case 2:
          P.K.push_back({Constant::String, 0, 0, string(Width)});
          break;
        case 4: {
          auto ID = function(Depth + 1).Id;
          P.Children.push_back(ID);
          P.K.push_back({Constant::Closure, ID});
          break;
        }
        case 6:
          P.K.push_back({Constant::Nil});
          break;
        default:
          malformed(R.at() - 1, "Invalid legacy constant tag");
        }
      }
    }
    instructions(P);
    P.End = R.at();
    P.Complete = true;
    return P;
  }
};
} // namespace

void neverd::lua::readStackChunk(Chunk &C, Reader &R) {
  C.Evidence["dialect"] = C.Version == 0x23   ? "Lua 2.4"
                          : C.Version == 0x25 ? "Lua 2.5/3.0"
                          : C.Version == 0x31 ? "Lua 3.1"
                          : C.Version == 0x32 ? "Lua 3.2"
                                              : "Lua 4.0";
  StackReader Reader{C, R, C.Version};
  Reader.header();
  if (C.Version <= 0x25)
    Reader.flat();
  else
    Reader.function(0);
  R.end();
}
