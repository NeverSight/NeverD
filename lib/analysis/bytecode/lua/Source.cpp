//===- Source.cpp - Structured Lua prototype recovery ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish a body only when its reachable effects, control transfers and stack
/// windows can be expressed.
//===----------------------------------------------------------------------===//

#include "Decode.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <bitset>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::lua;
using namespace llvm;
namespace {
struct Page {
  std::string Text;
  unsigned Line = 1;
  std::vector<unsigned> Map;
};
struct OpenResults {
  int Base;
  std::string Expression;
  unsigned PC;
  bool MayCall;
};
std::string reg(const Proto &P, int Slot) {
  return "r" + std::to_string(P.Id) + "_" + std::to_string(Slot);
}
std::string registers(const Proto &P, int First, int N) {
  std::string Result;
  for (int I = 0; I < N; ++I) {
    if (I)
      Result += ", ";
    Result += reg(P, First + I);
  }
  return Result;
}
llvm::SmallVector<int, 2> successors(const Inst &I) {
  int Next = I.PC + I.Width;
  switch (I.Kind) {
  case Form::Return:
  case Form::TailCall:
  case Form::Unsupported:
    return {};
  case Form::Jump:
  case Form::GenericPrep:
  case Form::ForPrepJump:
    return {I.Target};
  case Form::Branch:
  case Form::ForPrep:
  case Form::ForLoop:
  case Form::GenericLoop:
    if (Next != I.Target)
      return {Next, I.Target};
    return {Next};
  default:
    return {Next};
  }
}
struct Function {
  Chunk &C;
  Proto &P;
  std::vector<Inst> Code;
  std::vector<int> Previous;
  std::vector<unsigned> Predecessors;
  std::bitset<256> Captured;
  Function(Chunk &C, Proto &P)
      : C(C), P(P), Code(P.Code.size()), Previous(P.Code.size() + 1, -1),
        Predecessors(P.Code.size()) {}
  void decode(const std::vector<std::string> &Up) {
    for (unsigned PC = 0; PC < Code.size();) {
      C.Job.use(C.Job.Items, 1, P.CodeAt + 4 * PC);
      auto &I = Code[PC];
      I.PC = PC;
      I.Op = mnemonic(C.Dialect, C.Version, P.Code[PC]);
      if (I.Op.empty()) {
        I.Op = "unknown";
        unsupported(
            P.CodeAt + 4 * PC,
            "Unknown opcode " +
                std::to_string(P.Code[PC] & (C.Dialect == Family::Puc
                                                 ? C.Version < 0x54 ? 63 : 127
                                                 : 255)));
      }
      Decoder D{C, P, Up, I};
      try {
        switch (C.Dialect) {
        case Family::Puc:
          decodePuc(D);
          break;
        case Family::Jit:
          decodeJit(D);
          break;
        case Family::Luau:
          decodeLuau(D);
          break;
        }
      } catch (const Failure &F) {
        if (StringRef(F.Status) != "unsupported")
          throw;
        I.Kind = Form::Unsupported;
        I.Text = F.Message;
      }
      for (const auto &Binding : I.Captures)
        if (!Binding.ByValue && Binding.Slot >= 0)
          Captured.set(Binding.Slot);
      Previous[PC + I.Width] = PC;
      PC += I.Width;
    }
    for (const auto &I : Code)
      if (!I.Op.empty()) {
        for (auto Dest : successors(I)) {
          if (Dest < 0 || size_t(Dest) >= Code.size() || Code[Dest].Op.empty())
            malformed(P.CodeAt + 4 * I.PC,
                      "Control flow leaves code or enters an auxiliary word");
          ++Predecessors[Dest];
        }
      }
  }
  [[noreturn]] void partial(const Inst &I, StringRef Reason) {
    unsupported(P.CodeAt + 4 * I.PC, I.Op.str() + " at PC " +
                                         std::to_string(I.PC) + ": " +
                                         Reason.str());
  }
  void line(Page &Out, int Depth, const std::string &Text) {
    C.Job.use(C.Job.Text, Depth * 2 + Text.size() + 1, P.CodeAt);
    Out.Text.append(Depth * 2, ' ');
    Out.Text += Text;
    Out.Text += '\n';
    ++Out.Line;
  }
  void body(Page &Out, int Depth, unsigned Nest) {
    if (P.Stack > 200 ||
        (C.Dialect == Family::Puc && C.Version == 0x51 && (P.Flags & 4)))
      partial(Code.front(),
              "frame requires a dialect-specific local or implicit arg table");
    if (P.Stack > P.Params)
      line(Out, Depth, "local " + registers(P, P.Params, P.Stack - P.Params));
    Out.Map.resize(Code.size());
    region(Out, 0, Code.size(), Depth, Nest);
  }
  void closure(Page &Out, const Inst &I, int Depth, unsigned Nest) {
    if (Nest >= limits::kDepth)
      throw Failure{"budget_exceeded", "Source nesting budget exceeded",
                    P.CodeAt + 4 * I.PC};
    auto &Child = C.Functions[I.Base];
    if (!Child.Complete)
      partial(I, "incomplete closure prototype");
    std::vector<std::string> Up;
    bool Snapshot =
        llvm::any_of(I.Captures, [](auto &Cap) { return Cap.ByValue; });
    if (Snapshot && P.Stack + I.Captures.size() > 200)
      partial(I, "capture snapshots exceed the source local limit");
    if (Snapshot) {
      line(Out, Depth, "do");
      ++Depth;
    }
    for (const auto &Cap : I.Captures) {
      if (!Cap.ByValue) {
        Up.push_back(Cap.Name);
        continue;
      }
      if (Cap.Name == I.Text)
        partial(I, "recursive by-value closure capture");
      auto Name =
          "v" + std::to_string(Child.Id) + "_" + std::to_string(Up.size());
      line(Out, Depth, "local " + Name + " = " + Cap.Name);
      Up.push_back(Name);
    }
    Function F{C, Child};
    F.decode(Up);
    line(Out, Depth, I.Text + " = function(" + parameters(Child) + ")");
    auto OuterMap = std::move(Out.Map);
    F.body(Out, Depth + 1, Nest + 1);
    Out.Map = std::move(OuterMap);
    line(Out, Depth, "end");
    if (Snapshot)
      line(Out, Depth - 1, "end");
  }
  static std::string parameters(const Proto &P) {
    auto S = registers(P, 0, P.Params);
    if (P.Vararg)
      S += S.empty() ? "..." : ", ...";
    return S;
  }
  void map(Page &Out, const Inst &I) {
    for (unsigned J = 0; J < I.Width; ++J)
      Out.Map[I.PC + J] = Out.Line;
  }
  int loop(Page &Out, const Inst &I, int End, int Depth, unsigned Nest) {
    bool Numeric = I.Kind == Form::ForPrep || I.Kind == Form::ForPrepJump;
    int Latch = I.Target;
    if (I.Kind == Form::ForPrep)
      Latch = Previous[Latch];
    if (Latch <= int(I.PC) || Latch >= End)
      partial(I, "loop exits the current region");
    auto &L = Code[Latch];
    int Start = I.PC + I.Width;
    if (L.Kind != (Numeric ? Form::ForLoop : Form::GenericLoop) ||
        L.Target != Start || (I.Kind != Form::Jump && L.Base != I.Base))
      partial(I, "loop header and latch disagree");
    int A = L.Base;
    int Exit = Latch + L.Width;
    int ControlEnd = L.First;
    bool Finalizer = !Numeric && C.Dialect == Family::Puc && C.Version >= 0x54;
    if (Finalizer) {
      if (Exit >= End || Code[Exit].Kind != Form::Close || Code[Exit].Base > A)
        partial(I, "missing iterator close scope");
      if (Code[Exit].Base != A)
        partial(I, "iterator close also affects enclosing locals");
      Exit += Code[Exit].Width;
    }
    // Hidden counter/state slots have different values in the VM after a
    // latch. Require their lexical lifetime to end with the loop.
    std::bitset<256> Hidden, Dead;
    for (int N = A; N < ControlEnd; ++N)
      Hidden.set(N);
    if ((Hidden & Captured).any())
      partial(I, "hidden loop state may be observed by a closure");
    Dead = Hidden;
    for (int N = L.First; N < L.First + (Numeric ? 1 : L.Results); ++N)
      Dead.set(N);
    for (int PC = Start; PC < Latch; PC += Code[PC].Width) {
      C.Job.use(C.Job.Items, 1, P.CodeAt + 4 * PC);
      auto &B = Code[PC];
      if ((B.Registers & Hidden).any() || (Numeric && B.Writes.test(L.First)))
        partial(I, "body observes or changes loop-control storage");
      if (Finalizer && (B.Kind == Form::Closure || B.Kind == Form::MarkClose))
        partial(I, "overlapping close scopes");
    }
    for (int PC = Exit; PC < int(Code.size()) && Dead.any();
         PC += Code[PC].Width) {
      C.Job.use(C.Job.Items, 1, P.CodeAt + 4 * PC);
      auto &S = Code[PC];
      if (Predecessors[PC] <= 1)
        Dead &= ~S.Fresh;
      if ((Dead & S.Registers).any())
        partial(I, "loop storage remains observable after exit");
      // A branch may bypass a write. From here on, only absence of use is
      // proof.
      if (S.Kind != Form::Statement) {
        for (int J = PC; J < int(Code.size()); J += Code[J].Width) {
          C.Job.use(C.Job.Items, 1, P.CodeAt + 4 * J);
          if ((Dead & Code[J].Registers).any())
            partial(I, "loop storage escapes its lexical scope");
        }
        break;
      }
    }
    std::string Header;
    if (Numeric) {
      auto Value = C.Dialect == Family::Luau ? A + 2 : A;
      auto Limit = C.Dialect == Family::Luau ? A : A + 1;
      auto Step = C.Dialect == Family::Luau ? A + 1 : A + 2;
      Header = reg(P, L.First) + " = " + reg(P, Value) + ", " + reg(P, Limit) +
               ", " + reg(P, Step);
    } else
      Header = registers(P, L.First, L.Results) + " in " +
               registers(P, A, Finalizer ? 4 : 3);
    line(Out, Depth, "for " + Header + " do");
    region(Out, Start, Latch, Depth + 1, Nest + 1);
    map(Out, L);
    if (Finalizer)
      map(Out, Code[Latch + L.Width]);
    line(Out, Depth, "end");
    return Exit;
  }
  bool region(Page &Out, int Begin, int End, int Depth, unsigned Nest) {
    if (Nest > limits::kDepth)
      throw Failure{"budget_exceeded", "Source nesting budget exceeded",
                    P.CodeAt + 4 * Begin};
    std::optional<OpenResults> Open;
    for (int PC = Begin; PC < End;) {
      const auto &I = Code[PC];
      C.Job.check(P.CodeAt + 4 * PC);
      int Next = PC + I.Width;
      map(Out, I);
      // Closing references immediately before returning has the same lifetime
      // as the implicit close at source return. It does not consume VM top.
      if ((I.Op == "UCLO" && I.Kind == Form::Jump) ||
          (I.Op == "CLOSEUPVALS" && Next < End &&
           Code[Next].Kind == Form::Return)) {
        int Dest = I.Op == "UCLO" ? I.Target : Next;
        if (Dest < Next || Dest >= End)
          partial(I, "nonterminal close boundary");
        PC = Dest;
        continue;
      }
      bool Consumer = I.Kind == Form::Call || I.Kind == Form::TailCall ||
                      I.Kind == Form::Return;
      if (Open && (!Consumer || I.Args != -1 || Predecessors[PC] != 1))
        partial(I, "open results lack an exclusive consumer");
      if (I.Kind == Form::Unsupported)
        partial(I, I.Text.empty() ? "source semantics unavailable" : I.Text);
      if (I.Kind == Form::Call || I.Kind == Form::Vararg ||
          I.Kind == Form::Return || I.Kind == Form::TailCall) {
        std::string Args;
        if (Consumer && I.Args < 0) {
          if (!Open || Open->Base < I.First ||
              (I.OpenAt >= 0 && I.OpenAt != Open->Base))
            partial(I, "unproved open stack window");
          if (Open->MayCall) {
            if (I.Kind != Form::Return && Captured.test(I.Base))
              partial(I, "open call may change its consumer's callee");
            for (int N = I.First; N < Open->Base; ++N)
              if (Captured.test(N))
                partial(I, "open call may change earlier arguments");
          }
          Args = registers(P, I.First, Open->Base - I.First);
          if (!Args.empty())
            Args += ", ";
          Args += Open->Expression;
          Open.reset();
        } else if (Consumer)
          Args = registers(P, I.First, I.Args);
        if (I.Kind == Form::Return) {
          line(Out, Depth, Args.empty() ? "return" : "return " + Args);
          return false;
        }
        std::string Expr =
            I.Kind == Form::Vararg ? "..." : reg(P, I.Base) + "(" + Args + ")";
        if (I.Kind == Form::TailCall) {
          line(Out, Depth, "return " + Expr);
          return false;
        }
        if (I.Results < 0)
          Open =
              OpenResults{I.Base, std::move(Expr), I.PC, I.Kind == Form::Call};
        else if (I.Results)
          line(Out, Depth, registers(P, I.Base, I.Results) + " = " + Expr);
        else if (I.Kind == Form::Call)
          line(Out, Depth, Expr);
      } else if (I.Kind == Form::Statement) {
        if (!I.Text.empty())
          line(Out, Depth, I.Text);
      } else if (I.Kind == Form::Closure)
        closure(Out, I, Depth, Nest);
      else if (I.Kind == Form::Branch) {
        if (I.Target < Next || I.Target > End)
          partial(I, "branch is outside its structured region");
        int Divider = Previous[I.Target];
        bool HasElse = Divider >= Next && Code[Divider].Kind == Form::Jump &&
                       Code[Divider].Target > I.Target &&
                       Code[Divider].Target <= End;
        line(Out, Depth, "if not (" + I.Text + ") then");
        bool FallsThrough = region(Out, Next, HasElse ? Divider : I.Target,
                                   Depth + 1, Nest + 1);
        if (HasElse && FallsThrough) {
          map(Out, Code[Divider]);
          // LOADBOOL/LFALSESKIP assign before jumping over the other arm.
          if (!Code[Divider].Text.empty())
            line(Out, Depth + 1, Code[Divider].Text);
        }
        if (HasElse || !I.Taken.empty()) {
          line(Out, Depth, "else");
          if (!I.Taken.empty())
            line(Out, Depth + 1, I.Taken);
          if (HasElse)
            region(Out, I.Target, Code[Divider].Target, Depth + 1, Nest + 1);
        }
        line(Out, Depth, "end");
        Next = HasElse ? Code[Divider].Target : I.Target;
      } else if (I.Kind == Form::ForPrep || I.Kind == Form::ForPrepJump ||
                 I.Kind == Form::GenericPrep ||
                 (I.Kind == Form::Jump && I.Target > PC &&
                  Code[I.Target].Kind == Form::GenericLoop))
        Next = loop(Out, I, End, Depth, Nest);
      else if (I.Kind == Form::Jump) {
        if (I.Target < Next || I.Target > End)
          partial(I, "jump is outside its structured region");
        if (!I.Text.empty())
          line(Out, Depth, I.Text);
        Next = I.Target;
      } else if (I.Kind == Form::MarkClose) {
        // A to-be-closed variable is bound to exactly this value and lexical
        // range. The narrow proof excludes reassignments and crossing captures.
        if ((Captured >> I.Base).any() || P.Stack >= 200)
          partial(I,
                  "close scope overlaps captured storage or the local limit");
        int Until = Next;
        for (; Until < End; Until += Code[Until].Width) {
          auto &S = Code[Until];
          if (S.Kind == Form::Close && S.Base == I.Base)
            break;
          if ((S.Kind != Form::Statement && S.Kind != Form::Return &&
               S.Kind != Form::Call) ||
              S.Writes.test(I.Base))
            partial(I, "unproved finalizer lifetime");
        }
        if (Until == End && End != int(Code.size()))
          partial(I, "close scope crosses a branch");
        line(Out, Depth, "do");
        line(Out, Depth + 1,
             "local v" + std::to_string(P.Id) + "_close" + std::to_string(PC) +
                 " <close> = " + reg(P, I.Base));
        region(Out, Next, Until, Depth + 1, Nest + 1);
        if (Until < End)
          map(Out, Code[Until]);
        line(Out, Depth, "end");
        Next = Until == End ? End : Until + Code[Until].Width;
      } else
        partial(I, "unpaired loop or close boundary");
      PC = Next;
    }
    if (Open)
      partial(Code[Open->PC], "open result crosses a control-flow boundary");
    return true;
  }
};
std::string display(StringRef S) {
  return json::isUTF8(S) ? S.str() : json::fixUTF8(S);
}
json::Object inventory(Chunk &C, const Proto &P) {
  json::Array Constants, Up, Locals, Children;
  Constants.reserve(P.K.size() + P.GC.size());
  auto Pool = [&](const auto &Values, StringRef Name) {
    for (const auto &K : Values) {
      static const char *const Kinds[] = {
          "nil",   "boolean", "integer", "number", "string",
          "table", "import",  "closure", "vector", "cdata"};
      json::Object O{{"pool", Name}, {"kind", Kinds[K.Type]}};
      if (K.Type == Constant::String) {
        C.Job.use(C.Job.Text, K.Bytes.size() * 4 + 2, P.Start);
        O["value"] = quote(K.Bytes);
      }
      if (K.Type == Constant::Integer || K.Type == Constant::Closure ||
          K.Type == Constant::Import)
        O["value"] = std::to_string(K.I);
      if (K.Type == Constant::Number)
        O["value"] = number(K.F, true);
      if (K.Type == Constant::Boolean)
        O["value"] = bool(K.I);
      Constants.push_back(std::move(O));
    }
  };
  Pool(P.K, "constants");
  Pool(P.GC, "gc");
  Children.reserve(P.Children.size());
  for (unsigned Id : P.Children)
    Children.emplace_back(Id);
  auto Name = [&](StringRef S) -> json::Value {
    if (!S.data() || S.empty())
      return nullptr;
    C.Job.use(C.Job.Text, S.size() * 3, P.Start);
    return display(S);
  };
  Locals.reserve(P.Locals.size());
  for (const auto &L : P.Locals)
    Locals.push_back(json::Object{
        {"name", Name(L.Name)},
        {"start_pc", L.Start},
        {"end_pc", L.End},
        {"register", L.Reg < 0 ? json::Value(nullptr) : json::Value(L.Reg)}});
  Up.reserve(P.Up.size());
  for (const auto &U : P.Up) {
    json::Object O{{"name", Name(U.Name)}};
    if (C.Dialect == Family::Jit ||
        (C.Dialect == Family::Puc && C.Version >= 0x52)) {
      O["from_stack"] = U.Local;
      O["index"] = U.Index;
      O["kind"] = U.Kind;
    }
    Up.push_back(std::move(O));
  }
  return json::Object{
      {"id", P.Id},
      {"name", Name(P.Name)},
      {"source_name", Name(P.SourceName)},
      {"offset", P.Start},
      {"end_offset", P.End},
      {"parameters", P.Params},
      {"vararg", P.Vararg},
      {"frame_size", P.Stack},
      {"flags", P.Flags},
      {"first_line", P.FirstLine},
      {"last_line", P.LastLine},
      {"locals", std::move(Locals)},
      {"upvalues", std::move(Up)},
      {"children", std::move(Children)},
      {"constants", std::move(Constants)},
      {"register_names", "generated"},
      {"line_info", P.Lines.empty() ? "unavailable" : "available"}};
}

void addInstructionMappings(json::Object &Row, ArrayRef<Inst> Code,
                            const Proto &P, ArrayRef<unsigned> SourceLines,
                            bool Decoded) {
  json::Array Instructions;
  // Auxiliary words and the unexamined tail of a partial decode have no row.
  Instructions.reserve(
      llvm::count_if(Code, [](const Inst &I) { return !I.Op.empty(); }));
  bool AllSupported = Decoded;
  for (const Inst &I : Code) {
    if (I.Op.empty())
      continue;

    json::Object Mapping{{"pc", I.PC},       {"offset", P.CodeAt + 4 * I.PC},
                         {"words", I.Width}, {"word", P.Code[I.PC]},
                         {"opcode", I.Op},   {"successors", nullptr}};
    if (I.Kind == Form::Unsupported)
      AllSupported = false;
    else if (Decoded) {
      auto Targets = successors(I);
      json::Array Edges;
      Edges.reserve(Targets.size());
      for (int Target : Targets)
        Edges.emplace_back(Target);
      Mapping["successors"] = std::move(Edges);
    }
    if (I.Width > 1) {
      json::Array AuxiliaryWords;
      AuxiliaryWords.reserve(I.Width - 1);
      for (auto Word : ArrayRef(P.Code).slice(I.PC + 1, I.Width - 1))
        AuxiliaryWords.emplace_back(Word);
      Mapping["auxiliary_words"] = std::move(AuxiliaryWords);
    }
    if (!P.Lines.empty())
      Mapping["original_line"] = P.Lines[I.PC];
    if (I.PC < SourceLines.size() && SourceLines[I.PC])
      Mapping["source_line"] = SourceLines[I.PC];
    Instructions.emplace_back(std::move(Mapping));
  }
  Row["decode_complete"] = AllSupported;
  Row["instructions"] = std::move(Instructions);
}
} // namespace
void neverd::lua::render(Chunk &C, json::Object &Result) {
  json::Array Functions;
  Functions.reserve(C.Functions.size());
  StringRef Overall = "reconstructed_source";
  for (auto &P : C.Functions) {
    if (!P.Complete)
      continue;
    auto Row = inventory(C, P);
    if (!P.Types.empty())
      Row["type_info"] = std::move(P.Types);
    Page Out;
    Function F{C, P};
    StringRef Status = "reconstructed_source";
    json::Array Diagnostics;
    bool Decoded = false;
    try {
      std::vector<std::string> Up;
      for (unsigned U = 0; U < P.Up.size(); ++U)
        Up.push_back(P.Id == C.Root && U == 0 && C.Dialect == Family::Puc &&
                             C.Version >= 0x52
                         ? "_ENV"
                         : "u" + std::to_string(P.Id) + "_" +
                               std::to_string(U));
      F.decode(Up);
      Decoded = true;
      if (P.Id == C.Root &&
          P.Up.size() >
              (C.Dialect == Family::Puc && C.Version >= 0x52 ? 1u : 0u))
        unsupported(P.Start, "Root has unbound external upvalues");
      bool Wrapper = P.Id != C.Root || P.Params;
      if (Wrapper)
        F.line(Out, 0, "return function(" + Function::parameters(P) + ")");
      F.body(Out, Wrapper ? 1 : 0, 0);
      if (Wrapper)
        F.line(Out, 0, "end");
    } catch (const Failure &E) {
      if (StringRef(E.Status) == "cancelled" ||
          StringRef(E.Status) == "budget_exceeded")
        throw;
      Status = StringRef(E.Status) == "malformed" ? "malformed"
                                                  : "partial_reconstruction";
      Diagnostics.push_back(
          json::Object{{"offset", E.Offset}, {"message", E.Message}});
      Out.Text = "-- " + Status.str() + ": " + E.Message + "\n";
      Out.Map.clear();
      if (Status == "malformed" || Overall != "malformed")
        Overall = Status;
    }
    addInstructionMappings(Row, F.Code, P, Out.Map, Decoded);
    Row["source"] = std::move(Out.Text);
    Row["status"] = Status;
    Row["diagnostics"] = std::move(Diagnostics);
    Functions.push_back(std::move(Row));
  }
  Result["root"] = C.Root;
  Result["functions"] = std::move(Functions);
  Result["status"] = Overall;
}
