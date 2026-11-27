//===- InstructionReport.cpp - Python instruction evidence to JSON --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Write the same instruction fields to a materialized JSON object or a bounded
/// report stream. The stream keeps the compact records until their function
/// is written, avoiding one hash table and key sort per instruction.
//===----------------------------------------------------------------------===//

#include "InstructionReport.h"

#include "Artifacts.h"

#include "llvm/ADT/STLExtras.h"

#include <array>
#include <utility>

using namespace llvm;
using namespace neverd::python;

namespace {
// LLVM emits object keys in lexical order. Keep streaming output byte-identical
// to that representation, including null successors on an incomplete decode.
template <class Field, class Edges>
void instructionFields(const InstructionRecord &I, Field Emit,
                       Edges EmitEdges) {
  Emit("bytes", I.End - I.PC);
  if (I.OpcodePC)
    Emit("cache_units", I.CacheUnits);
  if (I.Complete && I.Handler)
    Emit("handler", *I.Target);
  Emit("offset", I.Offset);
  Emit("opcode", I.Mnemonic);
  if (I.OpcodePC)
    Emit("opcode_pc", *I.OpcodePC);
  Emit("opcode_value", I.Opcode);
  Emit("operand", I.Operand);
  if (I.OriginalLine)
    Emit("original_line", *I.OriginalLine);
  Emit("pc", I.PC);
  if (I.SourceLine)
    Emit("source_line", *I.SourceLine);
  if (I.Complete) {
    std::array<unsigned, 2> Destinations;
    unsigned Count = 0;
    if (I.FallsThrough)
      Destinations[Count++] = I.End;
    if (I.Target && !I.Handler && (!I.FallsThrough || *I.Target != I.End))
      Destinations[Count++] = *I.Target;
    EmitEdges(ArrayRef<unsigned>(Destinations.data(), Count));
  } else {
    Emit("successors", nullptr);
  }
  if (I.Target)
    Emit("target", *I.Target);
}

json::Object instructionObject(const InstructionRecord &I) {
  json::Object Row;
  instructionFields(
      I, [&](StringRef Key, auto Value) { Row.try_emplace(Key, Value); },
      [&](ArrayRef<unsigned> Edges) {
        Row.try_emplace("successors", json::Array(Edges));
      });
  return Row;
}

void writeInstruction(json::OStream &OS, const InstructionRecord &I) {
  OS.object([&] {
    instructionFields(
        I, [&](StringRef Key, auto Value) { OS.attribute(Key, Value); },
        [&](ArrayRef<unsigned> Edges) {
          OS.attributeArray("successors", [&] {
            for (unsigned PC : Edges)
              OS.value(PC);
          });
        });
  });
}

void writeFunction(json::OStream &OS, const json::Object &Fn,
                   ArrayRef<InstructionRecord> Rows) {
  OS.object([&] {
    for (const auto *Field : json::sortedElements(Fn)) {
      if (Field->first == "instructions") {
        OS.attributeArray("instructions", [&] {
          for (const auto &Row : Rows)
            writeInstruction(OS, Row);
        });
      } else {
        OS.attribute(Field->first, Field->second);
      }
    }
  });
}
} // namespace

void neverd::python::attachInstructionRows(Job &J, Function &F,
                                           json::Object &Row) {
  const bool HasDiskRows = F.Node.Body->Version != 310;
  if (HasDiskRows && !F.Lines.empty()) {
    size_t N = 0;
    for (auto &Row : F.DiskInstructions) {
      while (N < F.Code.size() && F.Code[N].Byte == Row.PC) {
        if (!Row.SourceLine && F.Lines[N])
          Row.SourceLine = F.Lines[N];
        ++N;
      }
    }
  }
  const size_t Count = HasDiskRows ? F.DiskInstructions.size() : F.Code.size();
  if (J.InstructionTables && Count) {
    auto &Table = J.InstructionTables->emplace_back();
    Table.FunctionIndex = J.Functions.size();
    if (HasDiskRows) {
      Table.Rows = std::move(F.DiskInstructions);
    } else {
      Table.Rows.reserve(Count);
      for (size_t N = 0; N < Count; ++N)
        Table.Rows.push_back(instructionRecord(F, N));
    }
    Row["instructions"] = json::Array{};
    return;
  }
  json::Array Instructions;
  Instructions.reserve(Count);
  for (size_t N = 0; N < Count; ++N)
    Instructions.emplace_back(instructionObject(
        HasDiskRows ? F.DiskInstructions[N] : instructionRecord(F, N)));
  Row["instructions"] = std::move(Instructions);
}

void neverd::python::writeInstructionReport(
    json::OStream &OS, const json::Object &Report,
    ArrayRef<FunctionInstructions> Tables) {
  OS.object([&] {
    for (const auto *Field : json::sortedElements(Report)) {
      if (Field->first != "functions") {
        OS.attribute(Field->first, Field->second);
        continue;
      }
      auto Table = Tables.begin();
      OS.attributeArray("functions", [&] {
        for (const auto &[Index, Fn] :
             llvm::enumerate(*Field->second.getAsArray())) {
          if (Table == Tables.end() || Table->FunctionIndex != Index) {
            OS.value(Fn);
            continue;
          }
          writeFunction(OS, *Fn.getAsObject(), Table->Rows);
          ++Table;
        }
      });
    }
  });
}
