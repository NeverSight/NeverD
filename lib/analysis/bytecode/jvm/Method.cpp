//===- Method.cpp - JVM inventory and per-method recovery ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish each completed stage independently, including disassembly when a
/// typed or source-level proof is incomplete.
//===----------------------------------------------------------------------===//

#include "Flow.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>

using namespace llvm;
namespace neverd::jvm {
namespace {
json::Array indices(ArrayRef<unsigned> Indices) {
  json::Array A;
  A.reserve(Indices.size());
  for (auto I : Indices)
    A.emplace_back(I);
  return A;
}
json::Array names(ArrayRef<std::string> Names) {
  json::Array A;
  for (auto &N : Names)
    A.emplace_back(N);
  return A;
}
StringRef tagName(unsigned Tag) {
  static constexpr StringRef Names[]{
      "reserved",      "Utf8",       "",
      "Integer",       "Float",      "Long",
      "Double",        "Class",      "String",
      "Fieldref",      "Methodref",  "InterfaceMethodref",
      "NameAndType",   "",           "",
      "MethodHandle",  "MethodType", "Dynamic",
      "InvokeDynamic", "Module",     "Package"};
  return Names[Tag];
}
json::Object instructions(const Class &C, const Code &B, const Instruction &I) {
  auto Op = I.Opcode;
  json::Object O{{"pc", I.PC},
                 {"offset", B.At + I.PC},
                 {"bytes", I.Width},
                 {"opcode", mnemonic(Op)},
                 {"successors", indices(I.Targets)},
                 {"source_spans", json::Array{}}};
  if (I.Wide)
    O["wide"] = true;
  if ((Op >= 21 && Op <= 45) || (Op >= 54 && Op <= 78) || Op == 132 ||
      Op == 169)
    O["local"] = I.Local;
  if (Op == 132)
    O["increment"] = I.Increment;
  if (Op == 16 || Op == 17 || Op == 185 || Op == 188 || Op == 197)
    O["operand"] = I.Operand;
  if (I.Pool) {
    O["pool_index"] = I.Pool;
    auto &K = C.Pool[I.Pool];
    if (K.Tag >= 9 && K.Tag <= 11 || K.Tag == 17 || K.Tag == 18) {
      const auto &NT = C.nameAndType(K);
      O["name"] = C.text(NT.A, K.At);
      O["descriptor"] = C.text(NT.B, K.At);
      if (K.Tag <= 11)
        O["owner"] = C.className(K.A, K.At);
      else {
        O["bootstrap_index"] = K.A;
        O["resolution"] = "unresolved_dynamic";
      }
    }
  }
  if (Op == 170 || Op == 171) {
    json::Array A;
    for (unsigned K = 0; K < I.Keys.size(); ++K)
      A.emplace_back(
          json::Object{{"key", I.Keys[K]}, {"target", I.Targets[K + 1]}});
    O["cases"] = std::move(A);
    O["default_target"] = I.Targets.front();
  }
  return O;
}
} // namespace
json::Object classInventory(const Class &C, bool Detailed) {
  json::Array Constants, Fields, BS, Refs;
  if (Detailed)
    Constants.reserve(C.Pool.size() - 1);
  for (unsigned I = 1; Detailed && I < C.Pool.size(); ++I) {
    auto &K = C.Pool[I];
    if (!K.Tag)
      continue;
    json::Object O{{"index", I}, {"tag", tagName(K.Tag)}, {"offset", K.At}};
    switch (K.Tag) {
    case 1:
      O["text"] = K.Text;
      if (K.Escaped)
        O["unpaired_surrogate"] = true;
      break;
    case 3:
    case 4:
    case 5:
    case 6:
      O["bits"] = utohexstr(K.Bits);
      break;
    case 7:
    case 8:
    case 16:
    case 19:
    case 20:
      O["index_a"] = K.A;
      break;
    default:
      O["index_a"] = K.A;
      O["index_b"] = K.B;
      break;
    }
    Constants.emplace_back(std::move(O));
  }
  for (auto &F : C.Fields)
    Fields.emplace_back(
        json::Object{{"name", F.Name},
                     {"descriptor", F.Desc},
                     {"flags", F.Flags},
                     {"offset", F.At},
                     {"attributes", json::Array(F.Attributes)}});
  for (auto &B : C.Bootstraps)
    BS.emplace_back(json::Object{{"method_handle", B.Handle},
                                 {"arguments", indices(B.Arguments)},
                                 {"resolution", "symbolic"}});
  for (auto &N : C.References)
    Refs.emplace_back(N);
  json::Object O{{"class", C.Name},
                 {"super", C.Super},
                 {"flags", C.Flags},
                 {"major", C.Major},
                 {"minor", C.Minor},
                 {"preview", C.Major >= 56 && C.Minor == 65535},
                 {"interfaces", names(C.Interfaces)},
                 {"attributes", json::Array(C.Attributes)},
                 {"fields", std::move(Fields)},
                 {"constant_pool_slots", C.Pool.size() - 1},
                 {"bootstraps", std::move(BS)},
                 {"references", std::move(Refs)}};
  if (Detailed)
    O["constants"] = std::move(Constants);
  if (C.Major == 45)
    O["release_candidates"] =
        C.Minor <= 3 ? json::Array{"1.0", "1.1"} : json::Array{"1.1"};
  else
    O["java_release"] = C.Major - 44;
  if (!C.Module.empty())
    O["module"] = json::Object(C.Module);
  if (!C.Kotlin.empty())
    O["kotlin_metadata"] = json::Object(C.Kotlin);
  return O;
}
json::Object analyzeMethod(const Class &C, const Member &M, Job &J) {
  json::Object Row{{"name", M.Name},
                   {"descriptor", M.Desc},
                   {"flags", M.Flags},
                   {"offset", M.At},
                   {"attributes", json::Array(M.Attributes)},
                   {"status", "metadata_only"},
                   {"instructions", json::Array{}},
                   {"diagnostics", json::Array{}}};
  if (!M.Body && !M.Error) {
    Row["kind"] =
        (M.Flags & 0x100) ? "native_declaration" : "abstract_declaration";
    Row["stage"] = "declaration";
    return Row;
  }
  bool Decoded = false;
  try {
    if (M.Error)
      throw *M.Error;
    const auto &B = *M.Body;
    Row["code_offset"] = B.At;
    Row["code_bytes"] = B.Bytes.size();
    Row["max_stack"] = B.MaxStack;
    Row["max_locals"] = B.MaxLocals;
    Row["local_variables"] = json::Array(B.Variables);
    Row["code_attributes"] = json::Array(B.Attributes);
    json::Array Handlers, Frames, Lines;
    for (auto &H : B.Handlers)
      Handlers.emplace_back(json::Object{
          {"priority", Handlers.size()},
          {"start", H.Start},
          {"end", H.End},
          {"handler", H.Target},
          {"type", H.Catch ? json::Value(C.className(H.Catch, B.At))
                           : json::Value(nullptr)}});
    for (auto &F : B.Frames)
      Frames.emplace_back(json::Object{{"pc", F.PC},
                                       {"locals", names(F.Locals)},
                                       {"stack", names(F.Stack)}});
    for (auto [PC, L] : B.Lines)
      Lines.emplace_back(json::Array{PC, L});
    Row["exception_table"] = std::move(Handlers);
    Row["stack_maps"] = std::move(Frames);
    Row["line_number_table"] = std::move(Lines);
    auto Code = decode(C, B, J);
    Decoded = true;
    auto &Rows = *Row.getArray("instructions");
    Rows.reserve(Code.Rows.size());
    auto Sorted = B.Lines;
    std::stable_sort(
        Sorted.begin(), Sorted.end(),
        [](const auto &L, const auto &R) { return L.first < R.first; });
    unsigned Line = 0, Next = 0;
    for (auto &I : Code.Rows) {
      auto O = instructions(C, B, I);
      while (Next < Sorted.size() && Sorted[Next].first <= I.PC)
        Line = Sorted[Next++].second;
      if (Line)
        O["original_line"] = Line;
      Rows.emplace_back(std::move(O));
    }
    Row["stage"] = "decoded";
    Row["status"] = "disassembly_only";
    auto G = lift(C, M, Code, J);
    Row["ir"] = irReport(G, Code, B, J);
    Row["stage"] = "typed_ir";
    for (unsigned I = 0; I < Rows.size(); ++I)
      (*Rows[I].getAsObject())["reachable"] =
          bool(G.Blocks[G.BlockForRow[I]].Entry);
    auto Java = renderJava(C, M, Code, G);
    json::Array Reverse;
    std::map<unsigned, std::vector<unsigned>> PCs;
    for (unsigned I = 0; I < Rows.size(); ++I) {
      auto &O = *Rows[I].getAsObject();
      auto &Spans = *O.getArray("source_spans");
      if (!Java.Lines[I].empty())
        O["source_line"] = Java.Lines[I].front();
      for (unsigned L : Java.Lines[I]) {
        Spans.emplace_back(json::Object{{"line", L}, {"end_line", L}});
        PCs[L].push_back(Code.Rows[I].PC);
      }
    }
    for (auto &[L, Ids] : PCs)
      Reverse.emplace_back(json::Object{{"line", L}, {"pcs", indices(Ids)}});
    Row["source_map"] = std::move(Reverse);
    Row["source"] = std::move(Java.Text);
    Row["status"] = "reconstructed_source";
    Row["stage"] = "java";
  } catch (const Failure &F) {
    if (StringRef(F.Status) == "cancelled" ||
        (StringRef(F.Code) == "job_items" || StringRef(F.Code) == "job_bytes"))
      throw;
    Row["status"] = StringRef(F.Status) == "unsupported" && Decoded
                        ? "disassembly_only"
                        : F.Status;
    Row.getArray("diagnostics")->emplace_back(diagnostic(F));
  }
  return Row;
}
} // namespace neverd::jvm
