//===- Chunk.h - Serialized Lua prototypes -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Format readers retain file offsets and share a bounded cursor and prototype
/// inventory.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_LUA_CHUNK_H
#define NEVERD_ANALYSIS_BYTECODE_LUA_CHUNK_H

#include "Limits.h"

#include "neverd/analysis/LuaRecovery.h"

#include <bit>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::lua {
struct Failure {
  const char *Status;
  std::string Message;
  size_t Offset;
};
[[noreturn]] inline void malformed(size_t At, std::string Why) {
  throw Failure{"malformed", std::move(Why), At};
}
[[noreturn]] inline void unsupported(size_t At, std::string Why) {
  throw Failure{"unsupported", std::move(Why), At};
}
struct Budget {
  Cancel Stop;
  size_t Items = limits::kItems, Words = limits::kInstructions,
         Text = limits::kTextBytes;
  void check(size_t At) const {
    if (Stop())
      throw Failure{"cancelled", "Recovery cancelled", At};
  }
  void use(size_t &Left, uint64_t N, size_t At) {
    check(At);
    if (N > Left)
      throw Failure{"budget_exceeded", "Lua recovery budget exceeded", At};
    Left -= N;
  }
};

struct Reader {
  llvm::StringRef Bytes;
  Budget &Job;
  size_t Pos = 0, Base = 0;
  bool Big = false;
  size_t at() const { return Base + Pos; }
  size_t left() const { return Bytes.size() - Pos; }
  llvm::StringRef take(uint64_t N);
  Reader slice(size_t N);
  uint8_t byte() { return take(1).front(); }
  bool boolean();
  uint64_t word(unsigned Width);
  uint64_t varint(bool Little = true, bool HighTerminates = false,
                  unsigned Bits = 32);
  unsigned count(uint64_t N);
  void align(unsigned Width);
  void expect(llvm::StringRef Literal);
  void end();
};

struct Constant {
  enum Kind {
    Nil,
    Boolean,
    Integer,
    Number,
    String,
    Table,
    Import,
    Closure,
    Vector,
    CData,
    ClassShape
  } Type;
  int64_t I = 0;
  double F = 0;
  llvm::StringRef Bytes;
};
struct Upvalue {
  bool Local = false;
  unsigned Index = 0, Kind = 0;
  llvm::StringRef Name;
};
struct Local {
  llvm::StringRef Name;
  unsigned Start, End;
  int Reg = -1;
};
struct StackInstruction {
  llvm::StringRef Name, Operation;
  unsigned PC, Width;
  int64_t Operand;
  unsigned Extra = 0;
};
struct StackCode {
  std::vector<StackInstruction> Instructions;
  std::vector<std::pair<unsigned, llvm::StringRef>> LocalEvents;
  unsigned FirstNumber = 0;
  bool HasFrame = true, HasParameters = true;
};
struct Proto {
  std::unique_ptr<StackCode> Legacy;
  unsigned Id, Params = 0, Stack = 0, Flags = 0;
  bool Vararg = false, Complete = false;
  size_t Start = 0, End = 0, CodeAt = 0;
  unsigned WordBytes = 4;
  size_t codeOffset(unsigned PC) const {
    return CodeAt + size_t(PC) * WordBytes;
  }
  unsigned FirstLine = 0, LastLine = 0;
  llvm::StringRef Name, SourceName;
  std::vector<uint32_t> Code, Lines;
  std::vector<Constant> K, GC;
  std::vector<unsigned> Children;
  llvm::json::Object Types;
  std::optional<unsigned> FeedbackSlots;
  std::optional<uint64_t> InlineCost;
  std::vector<Local> Locals;
  std::vector<Upvalue> Up;
};
enum class Family { Puc, Jit, Luau };
struct Chunk {
  Budget &Job;
  Family Dialect;
  llvm::json::Object &Evidence;
  int Version = 0;
  unsigned Root = 0;
  bool FR2 = false;
  std::deque<Proto> Functions;
  Proto &proto(Reader &R);
  void code(Proto &P, Reader &R, uint64_t Words);
};

void readPuc(Chunk &C, Reader &R, llvm::StringRef Profile);
void readStackChunk(Chunk &C, Reader &R);
void readJit(Chunk &C, Reader &R);
void readLuau(Chunk &C, Reader &R, bool Pinned);
void render(Chunk &C, llvm::json::Object &Result);
std::string quote(llvm::StringRef S, bool DecimalEscapes = true);
std::string number(double F, bool FloatSubtype);
std::string integer(int64_t I);
} // namespace neverd::lua
#endif // NEVERD_ANALYSIS_BYTECODE_LUA_CHUNK_H
