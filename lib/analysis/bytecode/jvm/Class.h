//===- Class.h - JVM class-file evidence ----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The class reader owns serialized indices. The decoder owns instruction
/// boundaries and the flow analysis owns the operand stack.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_JVM_CLASS_H
#define NEVERD_ANALYSIS_BYTECODE_JVM_CLASS_H

#include "Limits.h"

#include "neverd/analysis/JVMRecovery.h"

#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::jvm {
struct Failure {
  const char *Status, *Code;
  std::string Message;
  size_t At;
};
[[noreturn]] void invalid(size_t At, const char *Code, std::string Why);
[[noreturn]] void unsupported(size_t At, const char *Code, std::string Why);
llvm::json::Object diagnostic(const Failure &F);
using Hierarchy = std::map<std::string, std::vector<std::string>>;
struct Job {
  Cancel Stop;
  size_t Items = limits::Items, Work = limits::MethodWork;
  size_t MemberBytes = limits::MemberBytes, Expanded = limits::ExpandedBytes;
  size_t Members = limits::Members;
  const Hierarchy *Parents = nullptr;
  size_t TextBytes = limits::MaterializedBytes;
  void text(size_t Bytes, size_t At);
  void check(size_t At) const;
  void charge(size_t N, size_t At);
};
struct Reader {
  llvm::StringRef Data;
  Job &J;
  size_t Base = 0, Pos = 0;
  size_t at() const { return Base + Pos; }
  size_t left() const { return Data.size() - Pos; }
  llvm::StringRef take(size_t N);
  uint8_t u1();
  uint16_t u2();
  uint32_t u4();
  uint64_t u8();
  Reader slice(size_t N);
  void end() const;
};
struct Signature {
  std::vector<std::string> Arguments;
  std::string Result;
  unsigned Slots = 0;
};
Signature methodDescriptor(llvm::StringRef Text, size_t At);
void fieldDescriptor(llvm::StringRef Text, size_t At);
std::string javaType(llvm::StringRef Descriptor);
unsigned slots(llvm::StringRef Type);
char verificationKind(llvm::StringRef Type);
bool validClassName(llvm::StringRef Name);
struct Constant {
  uint8_t Tag = 0;
  uint16_t A = 0, B = 0;
  uint64_t Bits = 0;
  size_t At = 0;
  std::string Text;
  bool Escaped = false;
};
struct Handler {
  unsigned Start, End, Target, Catch;
};
struct Frame {
  unsigned PC;
  std::vector<std::string> Locals, Stack;
};
struct Code {
  llvm::StringRef Bytes;
  size_t At = 0;
  unsigned MaxLocals = 0, MaxStack = 0;
  std::vector<Handler> Handlers;
  std::vector<Frame> Frames;
  std::vector<std::pair<unsigned, unsigned>> Lines;
  llvm::json::Array Variables, Attributes;
};
struct Member {
  size_t At = 0;
  unsigned Flags = 0;
  std::string Name, Desc;
  std::optional<Code> Body;
  std::optional<Failure> Error;
  llvm::json::Array Attributes;
};
struct Bootstrap {
  unsigned Handle;
  std::vector<unsigned> Arguments;
};
struct Class {
  // Active only during admission. Published and cached classes are immutable.
  Job *Admission = nullptr;
  unsigned Major = 0, Minor = 0, Flags = 0;
  std::string Name, Super;
  std::vector<std::string> Interfaces;
  std::vector<Constant> Pool;
  std::vector<Member> Fields, Methods;
  std::vector<Bootstrap> Bootstraps;
  std::set<std::string> References;
  llvm::json::Array Attributes;
  llvm::json::Object Kotlin, Module;
  const Constant &constant(unsigned I, std::initializer_list<unsigned> Tags,
                           size_t At) const;
  const std::string &text(unsigned I, size_t At) const;
  const std::string &className(unsigned I, size_t At) const;
  const Constant &nameAndType(const Constant &Ref) const;
};
Class readClass(llvm::StringRef Bytes, Job &J);
void readAttributes(Reader &R, Class &C, Member *M, Code *Body, bool Method,
                    llvm::json::Array &Out, unsigned Depth = 0);
llvm::json::Object classInventory(const Class &C, bool Detailed);
struct Instruction {
  unsigned PC, Opcode, Width = 1;
  int32_t Operand = 0, Increment = 0;
  unsigned Pool = 0, Local = 0;
  bool Wide = false;
  std::vector<unsigned> Targets;
  std::vector<int32_t> Keys;
};
struct Instructions {
  std::vector<Instruction> Rows;
  std::vector<int> Index;
};
llvm::StringRef mnemonic(unsigned Opcode);
Instructions decode(const Class &C, const Code &Body, Job &J);
llvm::json::Object analyzeMethod(const Class &C, const Member &M, Job &J);
} // namespace neverd::jvm
#endif
