//===- Flow.cpp - JVM stack and locals dataflow ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Track category widths, allocations and constructor aliases across normal
/// and exception edges. Values have stable bytecode-derived identities.
//===----------------------------------------------------------------------===//

#include "Flow.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <deque>

using namespace llvm;
namespace neverd::jvm {
namespace {
std::string referenceType(StringRef Name) {
  return Name.starts_with("[") ? Name.str() : "L" + Name.str() + ";";
}
std::string norm(StringRef T) {
  return verificationKind(T) == 'I' ? "I" : T.str();
}
bool uninitialized(StringRef T) {
  return T == "uninitializedThis" || T.starts_with("new@");
}
char kind(StringRef T) {
  return T == "null" || uninitialized(T) ? 'A' : verificationKind(T);
}
int64_t definition(unsigned PC) { return int64_t(PC) + 1; }
int64_t exceptionId(unsigned Target) { return FirstExceptionId - Target; }
int64_t phi(unsigned PC, unsigned Slot, bool Stack) {
  return (int64_t(PC + 1) << 18) + (Stack ? 65536 : 0) + Slot;
}
class Analysis {
  const Class &C;
  const Member &M;
  const Code &Body;
  const Instructions &CodeRows;
  Job &J;
  Signature Sig;
  Flow G;
  std::deque<unsigned> Queue;
  std::vector<bool> Queued;
  std::vector<const Frame *> Frames;
  unsigned At = 0;
  size_t Remaining;
  void spend(size_t N) {
    J.charge(N, Body.At + At);
    if (N > Remaining)
      throw Failure{"budget_exceeded", "method_work",
                    "Method dataflow budget exceeded", Body.At + At};
    Remaining -= N;
  }
  [[noreturn]] void bad(const char *Code, StringRef Why) const {
    invalid(Body.At + At, Code, Why.str());
  }
  [[noreturn]] void partial(const char *Code, StringRef Why) const {
    unsupported(Body.At + At, Code, Why.str());
  }
  void compatible(const Value &V, StringRef Expected,
                  bool Uninit = false) const {
    if (V.Type.empty() || V.Type == "tail" || kind(V.Type) != kind(Expected))
      bad("stack_type", "Incompatible JVM operand type");
    if (uninitialized(V.Type) && !Uninit)
      bad("uninitialized_use",
          "Uninitialized reference used before a constructor");
  }
  void assign(const Value &V, StringRef T) {
    compatible(V, T);
    if (kind(T) != 'A' || T == "A" || V.Type == T || V.Type == "null" ||
        T == "Ljava/lang/Object;")
      return;
    if (V.Type == referenceType(C.Name) &&
        (T == referenceType(C.Super) ||
         llvm::any_of(C.Interfaces,
                      [&](auto &N) { return T == referenceType(N); })))
      return;
    if (StringRef(V.Type).starts_with("[") &&
        (T == "Ljava/lang/Cloneable;" || T == "Ljava/io/Serializable;"))
      return;
    if (J.Parents && StringRef(V.Type).starts_with("L") && T.starts_with("L")) {
      std::vector<std::string> Pending{
          StringRef(V.Type).drop_front().drop_back().str()};
      std::set<std::string> Visited;
      while (!Pending.empty()) {
        auto Child = std::move(Pending.back());
        Pending.pop_back();
        spend(1);
        if (!Visited.insert(Child).second)
          continue;
        auto P = J.Parents->find(Child);
        if (P == J.Parents->end())
          continue;
        for (auto &Parent : P->second) {
          if (T == referenceType(Parent))
            return;
          Pending.push_back(Parent);
        }
      }
    }
    partial("unresolved_hierarchy",
            "Reference assignability requires additional hierarchy evidence");
  }
  void push(State &S, Value V) {
    J.text(V.Type.size(), Body.At + At);
    S.StackSlots += slots(V.Type);
    if (S.StackSlots > Body.MaxStack)
      bad("stack_overflow", "Operand stack exceeds max_stack");
    S.Stack.push_back(std::move(V));
  }
  Value pop(State &S) {
    if (S.Stack.empty())
      bad("stack_underflow", "Operand stack underflow");
    auto V = std::move(S.Stack.back());
    S.Stack.pop_back();
    S.StackSlots -= slots(V.Type);
    return V;
  }
  void store(State &S, unsigned Slot, Value V) {
    if (S.Locals[Slot].Type == "tail" && Slot)
      S.Locals[Slot - 1] = {};
    if (slots(S.Locals[Slot].Type) == 2)
      S.Locals[Slot + 1] = {};
    if (slots(V.Type) == 2) {
      if (slots(S.Locals[Slot + 1].Type) == 2)
        S.Locals[Slot + 2] = {};
      S.Locals[Slot + 1] = {"tail", 0};
    }
    S.Locals[Slot] = std::move(V);
  }
  void permutation(unsigned Op, State &S, Operation &O) {
    // The dup forms duplicate exactly one or two slots, separated by one
    // or two slots. Cutting a category-2 value in half is invalid.
    auto group = [&](unsigned Width) {
      std::vector<Value> V;
      while (Width) {
        auto T = pop(S);
        auto N = slots(T.Type);
        if (N > Width)
          bad("stack_category", "Stack permutation splits a category-2 value");
        Width -= N;
        V.push_back(std::move(T));
      }
      std::reverse(V.begin(), V.end());
      return V;
    };
    auto put = [&](auto &Vs) {
      for (auto &V : Vs)
        push(S, V);
    };
    auto Top = group(Op == 88 || (Op >= 92 && Op <= 94) ? 2 : 1);
    O.Inputs = Top;
    if (Op == 87 || Op == 88)
      return;
    unsigned Below = (Op == 90 || Op == 93 || Op == 95) ? 1
                     : (Op == 91 || Op == 94)           ? 2
                                                        : 0;
    auto Under = group(Below);
    O.Inputs.insert(O.Inputs.begin(), Under.begin(), Under.end());
    put(Top);
    put(Under);
    if (Op != 95)
      put(Top);
  }
  void transfer(const Instruction &I, State &S, Operation &O) {
    unsigned Op = I.Opcode;
    At = I.PC;
    auto read = [&](StringRef T, bool U = false) {
      auto V = pop(S);
      compatible(V, T, U);
      O.Inputs.push_back(V);
      return V;
    };
    auto result = [&](std::string T) {
      O.Result = Value{std::move(T), definition(I.PC)};
      push(S, *O.Result);
    };
    auto binary = [&](char T, char Right = 0) {
      read(std::string(1, Right ? Right : T));
      read(std::string(1, T));
      std::reverse(O.Inputs.begin(), O.Inputs.end());
      result(std::string(1, T));
    };
    constexpr StringLiteral Scalars("IJFD");
    if (Op == 0)
      return;
    if (Op == 1) {
      result("null");
      return;
    }
    if (Op >= 2 && Op <= 17) {
      result(Op >= 9 && Op <= 10    ? "J"
             : Op >= 11 && Op <= 13 ? "F"
             : Op == 14 || Op == 15 ? "D"
                                    : "I");
      return;
    }
    if (Op >= 18 && Op <= 20) {
      const auto &K = C.Pool[I.Pool];
      switch (K.Tag) {
      case 3:
        result("I");
        break;
      case 4:
        result("F");
        break;
      case 5:
        result("J");
        break;
      case 6:
        result("D");
        break;
      case 7:
        result("Ljava/lang/Class;");
        break;
      case 8:
        result("Ljava/lang/String;");
        break;
      case 15:
        result("Ljava/lang/invoke/MethodHandle;");
        break;
      case 16:
        result("Ljava/lang/invoke/MethodType;");
        break;
      default:
        result(norm(C.text(C.nameAndType(K).B, K.At)));
        break;
      }
      return;
    }
    if ((Op >= 21 && Op <= 45) || (Op >= 54 && Op <= 78)) {
      bool Load = Op < 54;
      unsigned Group = Load ? (Op < 26 ? Op - 21 : (Op - 26) / 4)
                            : (Op < 59 ? Op - 54 : (Op - 59) / 4);
      std::string T(1, StringRef("IJFDA")[Group]);
      if (Load) {
        auto V = S.Locals[I.Local];
        compatible(V, T, true);
        O.Inputs.push_back(V);
        if (uninitialized(V.Type)) {
          O.Result = V;
          push(S, V);
        } else
          result(V.Type);
      } else {
        auto V = read(T, true);
        store(S, I.Local, V);
      }
      return;
    }
    if ((Op >= 46 && Op <= 53) || (Op >= 79 && Op <= 86)) {
      bool Load = Op < 79;
      unsigned Group = Op - (Load ? 46 : 79);
      char K = StringRef("IJFDABCS")[Group];
      std::optional<Value> V;
      if (!Load)
        V = read(std::string(1, K));
      read("I");
      auto Arr = read("A");
      std::reverse(O.Inputs.begin(), O.Inputs.end());
      std::string Element;
      if (Arr.Type == "null")
        Element = K == 'A' ? "Ljava/lang/Object;" : std::string(1, K);
      else {
        StringRef A = Arr.Type;
        if (!A.consume_front("[") ||
            (K == 'A' ? kind(A) != 'A'
                      : A != std::string(1, K) && !(K == 'B' && A == "Z")))
          bad("array_operand", "Array opcode has an incompatible element type");
        Element = A.str();
      }
      if (Load)
        result(norm(Element));
      return;
    }
    if (Op >= 87 && Op <= 95) {
      permutation(Op, S, O);
      return;
    }
    if (Op >= 96 && Op <= 115) {
      binary(Scalars[(Op - 96) % 4]);
      return;
    }
    if (Op >= 116 && Op <= 119) {
      std::string T(1, Scalars[Op - 116]);
      read(T);
      result(T);
      return;
    }
    if (Op >= 120 && Op <= 131) {
      binary(Op % 2 ? 'J' : 'I', Op <= 125 ? 'I' : 0);
      return;
    }
    if (Op == 132) {
      compatible(S.Locals[I.Local], "I");
      O.Inputs.push_back(S.Locals[I.Local]);
      O.Result = Value{"I", definition(I.PC)};
      store(S, I.Local, *O.Result);
      return;
    }
    if (Op >= 133 && Op <= 147) {
      static constexpr StringLiteral From("IIIJJJFFFDDDIII");
      static constexpr char Targets[] = {'J', 'F', 'D', 'I', 'F', 'D', 'I', 'J',
                                         'D', 'I', 'J', 'F', 'I', 'I', 'I'};
      read(std::string(1, From[Op - 133]));
      result(std::string(1, Targets[Op - 133]));
      return;
    }
    if (Op >= 148 && Op <= 152) {
      std::string T = Op == 148 ? "J" : Op <= 150 ? "F" : "D";
      read(T);
      read(T);
      std::reverse(O.Inputs.begin(), O.Inputs.end());
      result("I");
      return;
    }
    if ((Op >= 153 && Op <= 166) || Op == 198 || Op == 199 || Op == 170 ||
        Op == 171) {
      std::string T = (Op == 165 || Op == 166 || Op >= 198) ? "A" : "I";
      read(T);
      if (Op >= 159 && Op <= 166) {
        read(T);
        std::reverse(O.Inputs.begin(), O.Inputs.end());
      }
      return;
    }
    if (Op == 167 || Op == 200)
      return;
    if (Op == 168 || Op == 169 || Op == 201)
      partial("legacy_subroutine",
              "Legacy subroutine return-address flow is unresolved");
    if (Op >= 172 && Op <= 177) {
      if (Op == 177) {
        if (Sig.Result != "V")
          bad("return_type", "Void return in a value method");
      } else {
        char T = StringRef("IJFDA")[Op - 172];
        if (kind(Sig.Result) != T)
          bad("return_type", "Return opcode disagrees with method descriptor");
        auto V = read(std::string(1, T));
        assign(V, Sig.Result);
      }
      if (!S.Initialized)
        bad("constructor_return",
            "Constructor returns before initializing this");
      return;
    }
    if (Op >= 178 && Op <= 186) {
      auto &K = C.Pool[I.Pool], &NT = C.nameAndType(K);
      auto &Name = C.text(NT.A, K.At), &D = C.text(NT.B, K.At);
      auto Owner =
          Op == 186 ? std::string() : referenceType(C.className(K.A, K.At));
      if (Op <= 181) {
        if (Op == 179 || Op == 181) {
          auto V = read(norm(D));
          assign(V, D);
        }
        if (Op == 180 || Op == 181) {
          auto V = read("A", Op == 181);
          if (!(V.Type == "uninitializedThis" && Op == 181 &&
                Owner == referenceType(C.Name)))
            assign(V, Owner);
        }
        std::reverse(O.Inputs.begin(), O.Inputs.end());
        if (Op == 178 || Op == 180)
          result(norm(D));
        return;
      }
      auto Called = methodDescriptor(D, K.At);
      for (auto It = Called.Arguments.rbegin(); It != Called.Arguments.rend();
           ++It) {
        auto V = read(norm(*It));
        assign(V, *It);
      }
      if (Op != 184 && Op != 186) {
        auto Receiver = read("A", Name == "<init>");
        if (Name == "<init>") {
          if (!uninitialized(Receiver.Type))
            bad("double_initialization",
                "Constructor expects an uninitialized receiver");
          std::string Initialized;
          if (Receiver.Type == "uninitializedThis") {
            if (Owner != referenceType(C.Name) &&
                Owner != referenceType(C.Super))
              bad("constructor_owner", "Invalid constructor owner for this");
            Initialized = referenceType(C.Name);
            S.Initialized = true;
          } else {
            unsigned PC;
            bool Invalid =
                StringRef(Receiver.Type).drop_front(4).getAsInteger(10, PC);
            if (Invalid || PC >= CodeRows.Index.size() ||
                CodeRows.Index[PC] < 0)
              bad("allocation", "Unknown allocation identity");
            auto &New = CodeRows.Rows[CodeRows.Index[PC]];
            Initialized = referenceType(C.className(New.Pool, Body.At + PC));
            if (Owner != Initialized)
              bad("constructor_owner",
                  "Constructor differs from allocated class");
          }
          auto fix = [&](Value &V) {
            if (V.Type == Receiver.Type)
              V.Type = Initialized;
          };
          for (auto &V : S.Locals)
            fix(V);
          for (auto &V : S.Stack)
            fix(V);
        } else
          assign(Receiver, Owner);
      }
      std::reverse(O.Inputs.begin(), O.Inputs.end());
      if (Called.Result != "V")
        result(norm(Called.Result));
      return;
    }
    if (Op == 187) {
      result("new@" + std::to_string(I.PC));
      return;
    }
    if (Op == 188 || Op == 189 || Op == 197) {
      unsigned N = Op == 197 ? I.Operand : 1;
      for (unsigned K = 0; K < N; ++K)
        read("I");
      std::reverse(O.Inputs.begin(), O.Inputs.end());
      if (Op == 188)
        result("[" + std::string(1, StringRef("ZCFDBSIJ")[I.Operand - 4]));
      else if (Op == 189)
        result("[" + referenceType(C.className(I.Pool, Body.At + At)));
      else
        result(C.className(I.Pool, Body.At + At));
      return;
    }
    if (Op == 190) {
      auto V = read("A");
      if (V.Type != "null" && !StringRef(V.Type).starts_with("["))
        bad("array_operand", "arraylength requires an array");
      result("I");
      return;
    }
    if (Op == 191) {
      auto V = read("A");
      assign(V, "Ljava/lang/Throwable;");
      return;
    }
    if (Op == 192 || Op == 193) {
      read("A");
      result(Op == 192 ? referenceType(C.className(I.Pool, Body.At + At))
                       : "I");
      return;
    }
    if (Op == 194 || Op == 195) {
      read("A");
      return;
    }
    partial("typed_opcode", "Instruction has no qualified JVM transfer");
  }
  void queue(unsigned B) {
    if (!Queued[B]) {
      Queued[B] = true;
      Queue.push_back(B);
    }
  }
  void join(unsigned B, const State &Incoming) {
    for (auto &V : Incoming.Locals)
      J.text(V.Type.size(), Body.At + At);
    for (auto &V : Incoming.Stack)
      J.text(V.Type.size(), Body.At + At);
    std::optional<State> Checked;
    if (Frames[B]) {
      spend(Incoming.Locals.size() + Incoming.Stack.size() + 1);
      Checked = Incoming;
      frame(*Checked, *Frames[B]);
    }
    const auto &From = Checked ? *Checked : Incoming;
    auto &Block = G.Blocks[B];
    auto &Entry = Block.Entry;
    spend(From.Locals.size() + From.Stack.size() + 1);
    if (!Entry) {
      Entry = From;
      queue(B);
      return;
    }
    if (Entry->Stack.size() != From.Stack.size() ||
        Entry->StackSlots != From.StackSlots)
      bad("stack_join",
          "Control-flow inputs have incompatible operand stack heights");
    bool Changed = false;
    if (Entry->Initialized && !From.Initialized) {
      Entry->Initialized = false;
      Changed = true;
    }
    auto merge = [&](Value &Dest, const Value &V, unsigned Slot, bool Stack) {
      if (Dest == V)
        return;
      if (Dest.Type != V.Type) {
        if (kind(Dest.Type) == 'A' && kind(V.Type) == 'A' &&
            !uninitialized(Dest.Type) && !uninitialized(V.Type)) {
          auto T = Dest.Type == "null" ? V.Type
                   : V.Type == "null"  ? Dest.Type
                                       : "Ljava/lang/Object;";
          Changed |= T != Dest.Type;
          Dest.Type = std::move(T);
        } else if (!Stack) {
          if (!Dest.Type.empty()) {
            Dest = {};
            Changed = true;
          }
          return;
        } else
          bad("stack_join", "Incompatible operand types at control-flow join");
      }
      if (!Dest.Type.empty() && Dest.Type != "tail" && Dest.Id != V.Id) {
        auto Id = phi(CodeRows.Rows[Block.Begin].PC, Slot, Stack);
        auto &Inputs = G.Merges[Id];
        if (Dest.Id != Id)
          Inputs.insert(Dest.Id);
        if (V.Id != Id)
          Inputs.insert(V.Id);
        Changed |= Dest.Id != Id;
        Dest.Id = Id;
      }
    };
    for (unsigned I = 0; I < From.Locals.size(); ++I)
      merge(Entry->Locals[I], From.Locals[I], I, false);
    for (unsigned I = 0; I < From.Stack.size(); ++I)
      merge(Entry->Stack[I], From.Stack[I], I, true);
    if (Changed)
      queue(B);
  }
  void frame(State &S, const Frame &F) {
    auto verify = [&](Value &V, StringRef T) {
      if (T == "top") {
        V = {};
        return;
      }
      if (V.Type == T)
        return;
      if (uninitialized(T) || uninitialized(V.Type) || T == "null" ||
          kind(T) != kind(V.Type))
        bad("stack_map_type", "Stack map disagrees with inferred value");
      if (kind(T) == 'A')
        assign(V, T);
      V.Type = T.str();
    };
    if (S.Stack.size() != F.Stack.size())
      bad("stack_map_height", "Stack map disagrees with inferred stack height");
    unsigned Slot = 0;
    for (auto &T : F.Locals) {
      verify(S.Locals[Slot], T);
      if (slots(T) == 2)
        S.Locals[Slot + 1] = {"tail", 0};
      Slot += slots(T);
    }
    std::fill(S.Locals.begin() + Slot, S.Locals.end(), Value{});
    for (unsigned I = 0; I < F.Stack.size(); ++I)
      verify(S.Stack[I], F.Stack[I]);
  }

public:
  Analysis(const Class &C, const Member &M, const Instructions &CodeRows,
           Job &J)
      : C(C), M(M), Body(*M.Body), CodeRows(CodeRows), J(J),
        Sig(methodDescriptor(M.Desc, M.At)), Remaining(J.Work) {}
  Flow run() {
    const auto &Rows = CodeRows.Rows;
    std::vector<bool> Leaders(Rows.size());
    Leaders[0] = true;
    auto leader = [&](unsigned PC) {
      if (PC < Body.Bytes.size())
        Leaders[CodeRows.Index[PC]] = true;
    };
    for (unsigned I = 0; I < Rows.size(); ++I) {
      auto &R = Rows[I];
      if (R.Targets.size() == 1 && R.Targets[0] == R.PC + R.Width &&
          !(R.Opcode >= 153 && R.Opcode <= 171) && R.Opcode != 200)
        continue;
      for (unsigned T : R.Targets)
        leader(T);
      if (I + 1 < Rows.size())
        Leaders[I + 1] = true;
    }
    for (auto &H : Body.Handlers) {
      leader(H.Start);
      leader(H.End);
      leader(H.Target);
    }
    for (auto &F : Body.Frames)
      leader(F.PC);
    for (unsigned I = 0; I < Rows.size(); ++I) {
      if (Leaders[I]) {
        if (!G.Blocks.empty())
          G.Blocks.back().End = I;
        G.Blocks.push_back({I, unsigned(Rows.size()), {}});
      }
      G.BlockForRow.push_back(G.Blocks.size() - 1);
    }
    Queued.resize(G.Blocks.size());
    G.Ops.resize(Rows.size());
    Frames.resize(G.Blocks.size());
    for (auto &F : Body.Frames)
      Frames[G.BlockForRow[CodeRows.Index[F.PC]]] = &F;
    if (C.Major >= 51) {
      auto required = [&](unsigned PC) {
        if (PC && !Frames[G.BlockForRow[CodeRows.Index[PC]]])
          invalid(Body.At + PC, "missing_stack_map",
                  "Branch or exception target lacks its stack-map frame");
      };
      for (auto &R : Rows) {
        if (R.Opcode == 170 || R.Opcode == 171) {
          for (unsigned PC : R.Targets)
            required(PC);
        } else if ((R.Opcode >= 153 && R.Opcode <= 167) || R.Opcode == 198 ||
                   R.Opcode == 199 || R.Opcode == 200)
          required(R.Targets.front());
      }
      for (auto &H : Body.Handlers)
        required(H.Target);
    }
    State Start;
    spend(Body.MaxLocals);
    Start.Locals.resize(Body.MaxLocals);
    unsigned Slot = 0;
    Start.Initialized = M.Name != "<init>" || C.Name == "java/lang/Object";
    auto param = [&](std::string T) {
      unsigned N = slots(T);
      store(Start, Slot, {std::move(T), -int64_t(Slot + 1)});
      Slot += N;
    };
    if (!(M.Flags & 8))
      param(Start.Initialized ? referenceType(C.Name) : "uninitializedThis");
    for (auto &T : Sig.Arguments)
      param(norm(T));
    join(0, Start);
    while (!Queue.empty()) {
      unsigned B = Queue.front();
      Queue.pop_front();
      Queued[B] = false;
      auto &Block = G.Blocks[B];
      State S = *Block.Entry;
      for (unsigned I = Block.Begin; I < Block.End; ++I) {
        auto &R = Rows[I];
        At = R.PC;
        spend(1 + Body.Handlers.size());
        for (auto &H : Body.Handlers)
          if (R.PC >= H.Start && R.PC < H.End) {
            spend(S.Locals.size() + 1);
            State E;
            E.Locals = S.Locals;
            E.Initialized = S.Initialized;
            push(E,
                 {H.Catch ? referenceType(C.className(H.Catch, Body.At + R.PC))
                          : "Ljava/lang/Throwable;",
                  exceptionId(H.Target)});
            join(G.BlockForRow[CodeRows.Index[H.Target]], E);
          }
        auto &O = G.Ops[I];
        O = {};
        transfer(R, S, O);
        for (auto &V : O.Inputs)
          J.text(V.Type.size(), Body.At + R.PC);
      }
      for (unsigned PC : Rows[Block.End - 1].Targets)
        join(G.BlockForRow[CodeRows.Index[PC]], S);
    }
    return std::move(G);
  }
};
} // namespace
Flow lift(const Class &C, const Member &M, const Instructions &Code, Job &J) {
  return Analysis(C, M, Code, J).run();
}
json::Object irReport(const Flow &G, const Instructions &CodeRows,
                      const Code &Body, Job &J) {
  auto value = [](const Value &V) {
    return json::Object{{"id", V.Id}, {"type", V.Type}};
  };
  json::Array Blocks;
  Blocks.reserve(G.Blocks.size());
  for (auto &B : G.Blocks) {
    json::Object O{{"pc", CodeRows.Rows[B.Begin].PC},
                   {"reachable", bool(B.Entry)}};
    json::Array Normal, Exceptional;
    auto PC = CodeRows.Rows[B.Begin].PC;
    for (auto Target : CodeRows.Rows[B.End - 1].Targets)
      Normal.emplace_back(Target);
    J.charge(Body.Handlers.size(), Body.At + PC);
    for (unsigned K = 0; K < Body.Handlers.size(); ++K) {
      auto &H = Body.Handlers[K];
      if (PC >= H.Start && PC < H.End)
        Exceptional.emplace_back(
            json::Object{{"priority", K}, {"target", H.Target}});
    }
    O["normal_successors"] = std::move(Normal);
    O["exception_successors"] = std::move(Exceptional);
    if (B.Entry) {
      json::Array Stack, Locals, Ops;
      Ops.reserve(B.End - B.Begin);
      for (auto &V : B.Entry->Stack)
        Stack.emplace_back(value(V));
      for (unsigned I = 0; I < B.Entry->Locals.size(); ++I) {
        auto &V = B.Entry->Locals[I];
        if (V.Type.empty() || V.Type == "tail")
          continue;
        auto L = value(V);
        L["slot"] = I;
        Locals.emplace_back(std::move(L));
      }
      for (unsigned I = B.Begin; I < B.End; ++I) {
        auto &Node = G.Ops[I];
        json::Array Inputs;
        Inputs.reserve(Node.Inputs.size());
        for (auto &V : Node.Inputs)
          Inputs.emplace_back(value(V));
        json::Object Op{{"pc", CodeRows.Rows[I].PC},
                        {"opcode", mnemonic(CodeRows.Rows[I].Opcode)},
                        {"inputs", std::move(Inputs)}};
        if (Node.Result)
          Op["result"] = value(*Node.Result);
        Ops.emplace_back(std::move(Op));
      }
      O["locals"] = std::move(Locals);
      O["stack"] = std::move(Stack);
      O["operations"] = std::move(Ops);
    }
    Blocks.emplace_back(std::move(O));
  }
  json::Array Merges;
  for (auto &[Id, Inputs] : G.Merges) {
    json::Array V;
    for (auto I : Inputs)
      V.emplace_back(I);
    Merges.emplace_back(json::Object{{"id", Id}, {"inputs", std::move(V)}});
  }
  return json::Object{{"schema", "jvm-stack-ir-v1"},
                      {"blocks", std::move(Blocks)},
                      {"merges", std::move(Merges)}};
}
} // namespace neverd::jvm
