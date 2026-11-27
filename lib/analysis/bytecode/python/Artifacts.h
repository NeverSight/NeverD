//===- Artifacts.h - Python artifact and bytecode evidence ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Defines the bounded reader, marshal graph and qualified code-object layout.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_PYTHON_ARTIFACTS_H
#define NEVERD_ANALYSIS_BYTECODE_PYTHON_ARTIFACTS_H

#include "neverd/analysis/PythonRecovery.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::python {
namespace limits {
inline constexpr size_t kInputBytes =
    256ULL << 20; // Aggregate files in a packaging job.
inline constexpr size_t kMemberBytes = 32ULL
                                       << 20; // One expanded archive member.
inline constexpr size_t kExpandedBytes = 128ULL
                                         << 20; // All nested decompressions.
inline constexpr size_t kMembers = 4096;
inline constexpr size_t kObjects =
    131072; // Includes container slots and references.
inline constexpr size_t kInstructions = 16384;
inline constexpr size_t kCodeObjects = 1024;
inline constexpr size_t kTextBytes = 1 << 20;
inline constexpr size_t kReportBytes =
    4 << 20; // Fits the worker response envelope.
inline constexpr unsigned kDepth = 64;
// A protected region may inspect an instruction once per enclosing level.
inline constexpr size_t kRegionComparisons = kInstructions * kDepth;
} // namespace limits

struct Failure {
  llvm::StringRef Status;
  size_t Offset;
  std::string Message;
};
[[noreturn]] void malformed(size_t Offset, llvm::StringRef Message);
[[noreturn]] void unsupported(size_t Offset, llvm::StringRef Message);

struct Job {
  llvm::StringRef Profile;
  Cancel Stop;
  llvm::json::Array Components, Functions, Diagnostics;
  std::optional<unsigned> Root;
  size_t Objects = 0, Instructions = 0, CodeObjects = 0, Text = 0, Expanded = 0,
         Comparisons = 0;
  void check(size_t Offset) const;
  void charge(size_t &Used, size_t Amount, size_t Max, size_t Offset);
  unsigned component(llvm::StringRef Name, llvm::StringRef Bytes,
                     std::optional<unsigned> Parent);
  void diagnose(unsigned Id, const Failure &F);
  void inspect(unsigned Id, llvm::StringRef Bytes, llvm::StringRef Name,
               unsigned Depth = 0, unsigned PythonVersion = 0,
               bool RawCode = false);
  std::optional<unsigned> Selection, NativeSelection;
  std::string NativeBytes;
};

std::string hash(llvm::StringRef Bytes);
bool safeMemberName(llvm::StringRef Name);
void archive(Job &J, unsigned Id, llvm::StringRef Bytes, size_t Cookie,
             unsigned Depth);
void pyz(Job &J, unsigned Id, llvm::StringRef Bytes, unsigned Depth);
size_t findArchiveCookie(llvm::StringRef Bytes);
inline constexpr unsigned Magic310 = 3439;
inline constexpr unsigned Magic311 = 3495;
inline unsigned pythonVersion(unsigned Magic) {
  return Magic == Magic310 ? 310 : Magic == Magic311 ? 311 : 0;
}

struct Object;
struct Code {
  unsigned Version, Id, Args, PosOnly, KwOnly, Locals, Stack, Flags, FirstLine;
  Object *Bytes, *Consts, *Names, *Vars, *Free, *Cells, *Filename, *Name,
      *Lines;
  Object *LocalKinds = nullptr, *QualifiedName = nullptr, *Exceptions = nullptr;
};
struct Object {
  enum Kind {
    End,
    None,
    False,
    True,
    Ellipsis,
    Int,
    Long,
    Float,
    Unicode,
    Bytes,
    Tuple,
    List,
    Dict,
    FrozenSet,
    Set,
    CodeObject
  } Type = None;
  size_t Offset = 0, DataOffset = 0, EndOffset = 0;
  int64_t Number = 0;
  double Real = 0;
  llvm::StringRef Data;
  std::string Digits;
  std::vector<Object *> Items;
  std::unique_ptr<Code> Body;
};

class Marshal {
  Job &J;
  llvm::StringRef Input;
  size_t Position;
  unsigned Version;
  std::deque<Object> Nodes;
  std::vector<Object *> References;
  std::vector<Object *> Codes;
  llvm::StringRef take(size_t Size);
  unsigned count();
  Object *object(unsigned Depth, bool AllowEnd = false);
  void code(Object &O, unsigned Depth);

public:
  Marshal(Job &J, llvm::StringRef Input, size_t Offset = 0,
          unsigned Version = 310)
      : J(J), Input(Input), Position(Offset), Version(Version) {}
  Object &read();
  llvm::ArrayRef<Object *> codes() const { return Codes; }
};

enum class Op : uint8_t {
#define PYTHON_OPCODE(ID, NAME, OPERAND) NAME = ID,
#include "Opcodes310.def"
#undef PYTHON_OPCODE
};
struct Instruction {
  Op Opcode;
  unsigned Byte, End;
  uint32_t Arg;
  std::optional<unsigned> Target, Line;
};
struct Function {
  const Object &Node;
  std::vector<Instruction> Code;
  std::vector<int> Starts;
  std::vector<unsigned> Lines;
  std::string Source;
  std::optional<Failure> Error;
  llvm::json::Array DiskInstructions, ExceptionRegions;
  bool Decoded = false, BindingKnown = false;
  explicit Function(const Object &Node) : Node(Node) {}
};
llvm::StringRef mnemonic(Op Opcode);
void decode(Job &J, Function &F);
void decode311(Job &J, Function &F);
void render(Job &J, std::vector<Function> &Functions, unsigned Component);
llvm::json::Array instructionRows(const Function &F);
void recoverCode(Job &J, unsigned Id, llvm::StringRef Bytes, size_t At,
                 unsigned Version);
} // namespace neverd::python

#endif // NEVERD_ANALYSIS_BYTECODE_PYTHON_ARTIFACTS_H
