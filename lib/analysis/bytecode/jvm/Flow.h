//===- Flow.h - JVM operand stack and local identities --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Transfer results used by the typed IR view and the Java renderer.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_BYTECODE_JVM_FLOW_H
#define NEVERD_ANALYSIS_BYTECODE_JVM_FLOW_H
#include "Class.h"

#include <map>
namespace neverd::jvm {
struct Value {
  std::string Type;
  int64_t Id = 0;
  bool operator==(const Value &) const = default;
};
struct State {
  std::vector<Value> Locals, Stack;
  unsigned StackSlots = 0;
  bool Initialized = true;
};
struct Operation {
  std::vector<Value> Inputs;
  std::optional<Value> Result;
};
struct Block {
  unsigned Begin, End;
  std::optional<State> Entry;
};
struct Flow {
  std::vector<Block> Blocks;
  std::vector<unsigned> BlockForRow;
  std::vector<Operation> Ops;
  std::map<int64_t, std::set<int64_t>> Merges;
};
Flow lift(const Class &C, const Member &M, const Instructions &Code, Job &J);
llvm::json::Object irReport(const Flow &G, const Instructions &Rows,
                            const Code &Body, Job &J);
struct JavaBody {
  std::string Text;
  std::vector<std::vector<unsigned>> Lines;
};
JavaBody renderJava(const Class &C, const Member &M, const Instructions &Code,
                    const Flow &G);
} // namespace neverd::jvm
#endif
