//===- Decode.h - Lua-family source operations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalizes checked operands and control-flow edges for the structured
/// emitter.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_LUA_DECODE_H
#define NEVERD_ANALYSIS_BYTECODE_LUA_DECODE_H

#include "Chunk.h"

#include <bitset>

namespace neverd::lua {
enum class Form {
  Unsupported,
  Statement,
  Jump,
  Branch,
  Call,
  TailCall,
  Vararg,
  Return,
  Closure,
  ForPrep,
  ForPrepJump,
  ForLoop,
  GenericPrep,
  GenericLoop,
  MarkClose,
  Close
};
struct Capture {
  bool ByValue;
  std::string Name;
  int Slot = -1;
};
struct Inst {
  unsigned PC, Width = 1;
  llvm::StringRef Op;
  Form Kind = Form::Unsupported;
  std::string Text, Taken;
  int Target = -1, Base = 0, First = 0, Args = 0, Results = 0;
  // A known physical start for a LuaJIT open argument/result suffix.
  int OpenAt = -1;
  std::vector<Capture> Captures;
  std::bitset<256> Registers, Writes, Fresh;
};
struct Decoder {
  Chunk &C;
  Proto &P;
  const std::vector<std::string> &Up;
  Inst &I;
  std::string reg(int N) const;
  std::string up(int N) const;
  const Constant &constant(int N, bool GC = false) const;
  std::string literal(const Constant &K) const;
  std::string k(int N) const { return literal(constant(N)); }
  std::string global(const Constant &K) const;
  uint32_t extra();
  void set(int A, std::string Value);
  void statement(std::string Text);
  void branch(std::string Condition, bool WhenTrue, int Target);
  void call(int Base, int First, int Args, int Results, bool Tail = false);
  void returns(int First, int N);
  void varargs(int A, int N);
  void nils(int A, int End);
  Capture localCapture(int Slot, bool ByValue = false) const {
    return {ByValue, reg(Slot), Slot};
  }
  void capture(unsigned Child, int A);
  [[noreturn]] void bad(std::string Why) const;
};
void decodePuc(Decoder &D);
void decodeStack(Decoder &D, const StackInstruction &I, int &Top);
void decodeJit(Decoder &D);
void decodeLuau(Decoder &D);
llvm::StringRef mnemonic(Family F, int Version, uint32_t Word);
llvm::StringRef binary(llvm::StringRef Op);
} // namespace neverd::lua
#endif // NEVERD_ANALYSIS_BYTECODE_LUA_DECODE_H
