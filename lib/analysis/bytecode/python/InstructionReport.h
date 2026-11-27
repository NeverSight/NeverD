//===- InstructionReport.h - Python instruction report fields ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Instruction evidence shared by the object and streaming report writers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODE_PYTHON_INSTRUCTIONREPORT_H
#define NEVERD_ANALYSIS_BYTECODE_PYTHON_INSTRUCTIONREPORT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace neverd::python {
struct InstructionRecord {
  llvm::StringRef Mnemonic; // Static opcode table entry.
  size_t Offset;
  unsigned PC, End, Opcode, Operand;
  std::optional<unsigned> Target, OriginalLine, SourceLine, OpcodePC;
  unsigned CacheUnits = 0;
  bool Complete = false, FallsThrough = false, Handler = false;
};

struct FunctionInstructions {
  size_t FunctionIndex; // Index in the report, including source-only functions.
  std::vector<InstructionRecord> Rows;
};

struct Function;
struct Job;
InstructionRecord instructionRecord(const Function &F, size_t Index);
void attachInstructionRows(Job &J, Function &F, llvm::json::Object &Row);
void writeInstructionReport(llvm::json::OStream &OS,
                            const llvm::json::Object &Report,
                            llvm::ArrayRef<FunctionInstructions> Tables);
} // namespace neverd::python

#endif // NEVERD_ANALYSIS_BYTECODE_PYTHON_INSTRUCTIONREPORT_H
