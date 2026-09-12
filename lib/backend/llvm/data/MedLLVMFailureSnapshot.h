// Private first-fatal graph capture for existing CI failures. This is a
// bounded graph snapshot, not a complete proof trace or a replay format.
#ifndef NEVERD_PRIVATE_MEDLLVMFAILURESNAPSHOT_H
#define NEVERD_PRIVATE_MEDLLVMFAILURESNAPSHOT_H

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace neverd::detail::failure_snapshot {
class Buffer {
  size_t Cap;
  std::string Text;
  bool Complete = true;

public:
  explicit Buffer(size_t N) : Cap(N) { Text.reserve(N); }
  bool append(std::string_view S) noexcept {
    if (!Complete)
      return false;
    if (S.size() > Cap - Text.size()) {
      Complete = false;
      return false;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
      Text.append(S.data(), S.size());
    } catch (...) {
      Complete = false;
    }
#else
    Complete = false; // capture declines admission without exception support
#endif
    return Complete;
  }
  template <class T> bool number(T V) noexcept {
    char B[32];
    auto R = std::to_chars(B, B + sizeof(B), V);
    if (R.ec != std::errc{}) {
      stop();
      return false;
    }
    return append(std::string_view(B, static_cast<size_t>(R.ptr - B)));
  }
  bool json(std::string_view S) noexcept {
    // Byte-preserving escaped text, including non-ASCII bytes. Never walk an
    // unbounded symbol/string even when its destination has room remaining.
    if (S.size() > 4096) {
      stop();
      return false;
    }
    if (!append("\""))
      return false;
    constexpr char Hex[] = "0123456789abcdef";
    for (unsigned char C : S) {
      if (!Complete)
        return false;
      if (C == '"' || C == '\\') {
        char E[2] = {'\\', static_cast<char>(C)};
        append(std::string_view(E, 2));
      } else if (C < 32 || C >= 127) {
        char E[6] = {'\\', 'u', '0', '0', Hex[C >> 4], Hex[C & 15]};
        append(std::string_view(E, 6));
      } else {
        char E = static_cast<char>(C);
        append(std::string_view(&E, 1));
      }
    }
    return append("\"");
  }
  void stop() noexcept { Complete = false; }
  const std::string &str() const noexcept { return Text; }
  size_t size() const noexcept { return Text.size(); }
  bool complete() const noexcept { return Complete; }
};
inline void writeVar(Buffer &B, const MedVar &V) noexcept {
  B.append("{\"kind\":");
  B.number(static_cast<int>(V.Kind));
  B.append(",\"arch\":");
  B.number(static_cast<int>(V.TheArch));
  B.append(",\"rename\":");
  B.number(V.RenameTag);
  B.append(",\"id\":");
  B.number(V.Id);
  B.append(",\"ssa\":");
  B.number(V.SSAVer);
  B.append(",\"size\":");
  B.number(V.Size);
  B.append(",\"provenance\":");
  B.number(static_cast<int>(V.Provenance));
  B.append(",\"owner\":");
  B.number(V.AddressOwnerVA);
  switch (V.Kind) {
  case MedVar::Const:
    B.append(",\"constant\":");
    B.number(V.ConstVal);
    break;
  case MedVar::Reg:
  case MedVar::Param:
    B.append(",\"register\":");
    B.number(V.RegOff);
    break;
  case MedVar::Stack:
    B.append(",\"stack\":");
    B.number(V.StackOff);
    break;
  default:
    break;
  }
  B.append("}");
}

struct Budget {
  std::chrono::steady_clock::time_point Deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  size_t Records = 0;
  bool Exhausted = false;
  bool allows() noexcept {
    if (std::chrono::steady_clock::now() >= Deadline)
      Exhausted = true;
    return !Exhausted;
  }
  bool takeRecord(Buffer &B) noexcept {
    if (!B.complete() || !allows() || Records >= 20000) {
      Exhausted = true;
      B.stop();
      return false;
    }
    ++Records;
    return true;
  }
};
struct SnapshotWriter {
  Budget &O;
  Buffer &B;
  bool row(const char *Kind) noexcept {
    if (!O.takeRecord(B))
      return false;
    B.append("{\"record\":");
    B.json(Kind);
    return true;
  }
  void key(const char *K) noexcept {
    B.append(",");
    B.json(K);
    B.append(":");
  }
  template <class T> void n(const char *K, T V) noexcept {
    key(K);
    B.number(static_cast<int64_t>(V));
  }
  void u(const char *K, uint64_t V) noexcept {
    key(K);
    B.number(V);
  }
  void s(const char *K, std::string_view V) noexcept {
    key(K);
    B.json(V);
  }
  void v(const char *K, const MedVar &V) noexcept {
    key(K);
    writeVar(B, V);
  }
  void end() noexcept { B.append("}\n"); }
  void type(const TypeRef &T, unsigned Depth = 0) noexcept {
    if (!T) {
      B.append("null");
      return;
    }
    if (Depth >= 8 || !O.takeRecord(B)) {
      B.stop();
      return;
    }
    B.append("{\"kind\":");
    B.number(static_cast<int>(T->Kind));
    n("size", T->Size);
    n("signed", T->IsSigned);
    u("array_count", T->ArrayCount);
    key("pointee");
    type(T->Pointee, Depth + 1);
    key("element");
    type(T->ElemType, Depth + 1);
    key("return");
    type(T->RetType, Depth + 1);
    key("params");
    B.append("[");
    bool First = true;
    for (const auto &P : T->ParamTypes) {
      if (!O.allows() || !B.complete()) {
        B.stop();
        return;
      }
      if (!First)
        B.append(",");
      First = false;
      type(P, Depth + 1);
    }
    B.append("]}");
  }
  template <class Block> void edges(const Block &BB) noexcept {
    for (int E : BB.Succs) {
      if (!row("successor"))
        return;
      n("block", BB.Id);
      n("to", E);
      end();
    }
    for (int E : BB.Preds) {
      if (!row("predecessor"))
        return;
      n("block", BB.Id);
      n("from", E);
      end();
    }
    const auto Exceptional = [&](const auto &Edges, const char *Kind) {
      for (const auto &E : Edges) {
        if (!row(Kind))
          return;
        n("block", BB.Id);
        n("other", E.BlockId);
        u("target", E.TargetVA);
        n("kind", E.Kind);
        n("region", E.RegionIndex);
        n("state", E.State);
        end();
      }
    };
    Exceptional(BB.ExceptionalSuccs, "exceptional-successor");
    Exceptional(BB.ExceptionalPreds, "exceptional-predecessor");
  }
};
inline void writeTables(SnapshotWriter &A, const std::vector<JumpTable> &Tables,
                        const char *Level) noexcept {
  for (size_t I = 0; I < Tables.size(); ++I) {
    const auto &T = Tables[I];
    if (!A.row("jump-table"))
      break;
    A.s("level", Level);
    A.u("index", I);
    A.u("instruction", T.InsnAddr);
    A.u("base", T.BaseAddr);
    A.n("has_base", T.HasBaseAddr);
    A.n("entry_size", T.EntrySize);
    A.u("entry_stride", T.EntryStride);
    A.n("index_register", T.IndexRegOff);
    A.n("relative", T.IsRelative);
    A.n("signed", T.IsSigned);
    A.u("target_base", T.TargetBase);
    A.n("has_target_base", T.HasTargetBase);
    A.n("pe_rva", T.IsPEImageRelativeRVA);
    A.u("load_address", T.TableLoadAddr);
    A.n("prescaled", T.PreScaledIndex);
    A.n("two_table_select", T.TwoTableSelect);
    A.u("two_table_offset", T.TwoTableOffset);
    A.n("two_table_hi_positive", T.TwoTableHiPositive);
    A.n("two_level_index", T.TwoLevelIndex);
    A.n("mutated_unsafe", T.MutatedUnsafe);
    A.n("has_dispatch_slot_map", T.HasDispatchSlotMap);
    A.end();
    auto Range = [&](const auto &R, const char *Kind) {
      if (!A.row(Kind))
        return;
      A.s("level", Level);
      A.u("table_index", I);
      A.u("base", R.BaseAddr);
      A.n("entry_size", R.EntrySize);
      A.u("entry_stride", R.EntryStride);
      A.u("physical_slot_count", R.PhysicalSlotCount);
      A.end();
    };
    if (T.ExactPhysicalStorageRange)
      Range(*T.ExactPhysicalStorageRange, "table-exact-physical-range");
    for (const auto &R : T.StorageRanges) {
      if (!A.O.allows() || !A.B.complete())
        break;
      Range(R, "table-storage-range");
    }
    for (auto V : T.SuppressibleRelocationSlots) {
      if (!A.row("table-suppressed-relocation"))
        break;
      A.s("level", Level);
      A.u("table_index", I);
      A.u("slot", V);
      A.end();
    }
    for (const auto &V : T.AuthenticatedTableLoads) {
      if (!A.row("table-authenticated-load"))
        break;
      A.s("level", Level);
      A.u("table_index", I);
      A.u("address", V.Addr);
      A.n("sequence", V.Seq);
      A.n("size", V.Size);
      A.end();
    }
    auto Selector = [&](const auto &V, const char *Kind) {
      if (!A.row(Kind))
        return;
      A.s("level", Level);
      A.u("table_index", I);
      A.u("address", V.Addr);
      A.n("sequence", V.Seq);
      A.n("opcode", V.ExpectedOpcode);
      A.n("role", V.Role);
      A.n("input", V.InputNo);
      A.n("size", V.ExpectedSize);
      A.end();
    };
    for (const auto &V : T.SelectorUseRefs) {
      if (!A.O.allows() || !A.B.complete())
        break;
      Selector(V, "table-selector-use");
    }
    if (T.CompositeSelectorUseRef) {
      const auto &V = *T.CompositeSelectorUseRef;
      if (!A.row("table-composite-selector"))
        break;
      A.s("level", Level);
      A.u("table_index", I);
      A.n("kind", V.RecipeKind);
      A.u("true_offset", V.TrueOffset);
      A.u("false_offset", V.FalseOffset);
      A.n("result_size", V.ResultSize);
      A.end();
      Selector(V.ByteIndex, "table-composite-byte-index");
      Selector(V.Condition, "table-composite-condition");
    }
    for (size_t J = 0; J < T.Targets.size(); ++J) {
      if (!A.row("table-target"))
        break;
      A.s("level", Level);
      A.u("table_index", I);
      A.u("index", J);
      A.u("target", T.Targets[J]);
      A.end();
    }
    for (size_t J = 0; J < T.SlotIndices.size(); ++J) {
      if (!A.row("table-slot-index"))
        break;
      A.s("level", Level);
      A.u("table_index", I);
      A.u("index", J);
      A.u("slot", T.SlotIndices[J]);
      A.end();
    }
    for (size_t J = 0; J < T.CaseLabels.size(); ++J) {
      if (!A.row("table-case-label"))
        break;
      A.s("level", Level);
      A.u("table_index", I);
      A.u("index", J);
      A.n("label", T.CaseLabels[J]);
      A.end();
    }
    if (!A.O.allows() || !A.B.complete())
      break;
  }
}

inline void writeGraph(Budget &O, Buffer &Data, const BinaryImage &Img,
                       const MedFunc &Func) noexcept {
  const MedFunc *MF = &Func;
  SnapshotWriter M{O, Data}, A{O, Data};
  if (!M.row("backend-input-function"))
    return;
  M.s("name", MF->Name);
  M.u("entry", MF->Entry);
  M.u("size", MF->OriginalSize);
  M.n("calling_convention", MF->CC);
  M.n("frame_size", MF->FrameSize);
  M.n("frame_headroom", MF->FrameHeadroom);
  M.n("source_parameters_bound", MF->SourceParametersBound);
  M.n("return_evidence", MF->ReturnValueEvidence);
  M.n("fp_return_x87", MF->FPReturnViaX87);
  M.n("does_not_return", MF->DoesNotReturn);
  M.n("variadic", MF->IsVariadic);
  M.n("variadic_overflow_base", MF->VariadicOverflowBase);
  M.n("variadic_fixed_stack_args", MF->VariadicFixedStackArgs);
  M.n("variadic_overflow_count", MF->VariadicOverflowCount);
  M.n("continuation_analysis_complete",
      MF->CxxContinuationExitAnalysisComplete);
  M.key("return_type");
  M.type(MF->ReturnType);
  M.end();
  // These fields are present but are outside this bounded graph schema.
  // The capture manifest never claims to be a complete MedIR/proof model.
  if (M.row("omitted-med-metadata")) {
    M.n("exception", MF->ExceptionMetadata.has_value());
    M.n("source_type", MF->SourceTypeHint.has_value());
    M.u("continuations", MF->CxxContinuationExits.size());
    M.u("struct_return_candidates", MF->StructReturnCandidates.size());
    M.end();
  }
  for (const auto &V : MF->Params) {
    if (!M.row("parameter"))
      break;
    M.v("value", V);
    M.end();
  }
  for (const auto &V : MF->Locals) {
    if (!M.row("local"))
      break;
    M.v("value", V);
    M.end();
  }
  for (const auto &V : MF->MultiReturn) {
    if (!M.row("return-register"))
      break;
    M.u("register", V.RegOff);
    M.n("size", V.Size);
    M.n("fp", V.IsFP);
    M.end();
  }
  for (const auto &[Index, Off] : MF->MutableStackParamHomes) {
    if (!M.row("mutable-parameter-home"))
      break;
    M.n("parameter", Index);
    M.n("offset", Off);
    M.end();
  }
  for (const auto &V : MF->TypedParams) {
    if (!M.row("typed-parameter"))
      break;
    M.s("name", V.Name);
    M.key("type");
    M.type(V.Type);
    M.end();
  }
  for (const auto &V : MF->TypedLocals) {
    if (!M.row("typed-local"))
      break;
    M.s("name", V.Name);
    M.n("offset", V.StackOff);
    M.key("type");
    M.type(V.Type);
    M.end();
  }
  for (const auto &V : MF->CallClobbers) {
    if (!M.row("call-clobber"))
      break;
    M.n("call_site", V.CallSiteId);
    M.v("value", V.Value);
    M.v("preserved_input", V.PreservedInput);
    M.n("preserved_prefix_size", V.PreservedPrefixSize);
    M.end();
  }
  for (const auto &BB : MF->Blocks) {
    if (!M.row("block"))
      break;
    M.n("id", BB.Id);
    M.u("start", BB.StartAddr);
    M.u("end", BB.EndAddr);
    M.end();
    M.edges(BB);
    for (size_t I = 0; I < BB.Phis.size(); ++I) {
      const auto &P = BB.Phis[I];
      if (!M.row("phi"))
        break;
      M.n("block", BB.Id);
      M.u("index", I);
      M.v("output", P.Output);
      M.u("arg_count", P.Args.size());
      M.end();
      for (const auto &[Pred, V] : P.Args) {
        if (!M.row("phi-argument"))
          break;
        M.n("block", BB.Id);
        M.u("phi_index", I);
        M.n("predecessor", Pred);
        M.v("value", V);
        M.end();
      }
      if (!O.allows() || !Data.complete())
        break;
    }
    for (size_t I = 0; I < BB.Ops.size(); ++I) {
      if (!M.row("op"))
        break;
      const auto &Op = BB.Ops[I];
      M.n("block", BB.Id);
      M.u("index", I);
      M.n("opcode", Op.Opcode);
      M.n("memory_ordering", Op.MemoryOrdering);
      M.n("address_space", Op.MemoryAddressSpace);
      M.u("address", Op.Addr);
      M.n("origin_seq", Op.OriginSeq);
      M.n("call_site", Op.CallSiteId);
      M.n("dead", Op.Dead);
      M.n("preserves_caller_saved", Op.PreservesCallerSaved);
      M.n("does_not_return", Op.DoesNotReturn);
      M.n("source_call_hint_present", bool(Op.SourceCallHint));
      M.v("output", Op.Output);
      M.n("input_count", Op.NumInputs);
      M.key("inputs");
      M.B.append("[");
      if (Op.NumInputs > Op.Inputs.size()) {
        M.B.stop();
        break;
      }
      for (uint8_t J = 0; J < Op.NumInputs; ++J) {
        if (J)
          M.B.append(",");
        writeVar(M.B, Op.Inputs[J]);
      }
      M.B.append("]");
      M.end();
    }
    if (!O.allows() || !Data.complete())
      break;
  }
  for (const auto &V : MF->ScalarAddressModels) {
    if (!A.row("med-scalar-model"))
      break;
    A.n("model", V.Model);
    A.v("value", V.Value);
    A.end();
  }
  for (const auto &V : MF->I386GetPcModels) {
    if (!A.row("med-get-pc-model"))
      break;
    A.v("output", V.Output);
    A.v("value", V.Value);
    A.u("pc", V.PCValue);
    A.end();
  }
  for (const auto &S : Img.Sections) {
    if (!A.row("section"))
      break;
    A.s("name", S.Name);
    A.s("segment", S.SegmentName);
    A.u("va", S.VA);
    A.u("size", S.Size);
    A.u("file_offset", S.FileOff);
    A.u("file_size", S.FileSz);
    A.n("flags", S.Flags);
    A.n("type", S.Type);
    A.u("alignment", S.Alignment);
    A.end();
  }
  for (const auto &S : Img.Segments) {
    if (!A.row("segment"))
      break;
    A.s("name", S.Name);
    A.u("va", S.VA);
    A.u("size", S.Size);
    A.n("flags", S.Flags);
    A.u("data_size", S.Data.size());
    A.end();
  }
  for (const auto &V : Img.Relocations) {
    if (!A.row("relocation"))
      break;
    A.u("address", V.Address);
    A.n("addend", V.Addend);
    A.n("explicit_addend", V.HasExplicitAddend);
    A.n("type", V.Type);
    A.s("symbol", V.SymbolName);
    A.u("symbol_index", V.SymbolIndex);
    A.s("section", V.SectionName);
    A.end();
  }
  for (const auto &V : MF->CallInfos) {
    if (!M.row("call-info"))
      break;
    M.n("block", V.BlockId);
    M.n("op", V.OpIdx);
    M.u("target", V.TargetAddr);
    M.s("target_name", V.TargetName);
    M.n("indirect", V.IsIndirect);
    M.n("vararg_fixed_count", V.VarArgFixedCount);
    M.n("source_call_hint_present", bool(V.SourceCallHint));
    M.end();
    for (size_t J = 0; J < V.Args.size(); ++J) {
      if (!M.row("call-info-argument"))
        break;
      M.n("block", V.BlockId);
      M.n("op", V.OpIdx);
      M.u("index", J);
      M.v("value", V.Args[J]);
      M.end();
    }
    if (!O.allows() || !Data.complete())
      break;
  }
  writeTables(A, MF->JumpTables, "backend-med");
  for (const auto &[Address, V] : MF->SwitchSelectorPlans) {
    if (!A.row("med-switch-selector"))
      break;
    A.u("address", Address);
    A.n("kind", V.PlanKind);
    A.v("selector", V.Selector);
    A.v("condition", V.Condition);
    A.u("true_offset", V.TrueOffset);
    A.u("false_offset", V.FalseOffset);
    A.n("result_size", V.ResultSize);
    A.end();
    for (const auto &[Pred, Value] : V.EdgeSelectors) {
      if (!A.row("med-switch-edge-selector"))
        break;
      A.u("address", Address);
      A.n("predecessor", Pred);
      A.v("value", Value);
      A.end();
    }
    if (!O.allows() || !Data.complete())
      break;
  }
  for (const auto &V : Img.ExactDataObjects) {
    if (!A.row("exact-data-owner"))
      break;
    A.u("base", V.Base);
    A.u("size", V.Size);
    A.n("evidence", V.Evidence);
    A.n("precision", V.Precision);
    A.end();
  }
  for (const auto &[Address, V] : Img.DataAddressRelocOperands) {
    if (!A.row("data-address-relocation-operand"))
      break;
    A.u("address", Address);
    A.u("encoded", V.EncodedValue);
    A.u("target", V.TargetVA);
    A.u("owner", V.TargetOwnerVA);
    A.n("width", V.Width);
    A.n("pc_relative_end", V.PCRelativeFromInstructionEnd);
    A.n("kind", V.Kind);
    A.end();
  }
  for (auto V : Img.RelocDataAddrs) {
    if (!A.row("relocated-data-address"))
      break;
    A.u("address", V);
    A.end();
  }
  for (auto V : MF->UnsafeIndirectBranchAddresses) {
    if (!M.row("unsafe-indirect-branch"))
      break;
    M.u("address", V);
    M.end();
  }

  for (const auto &S : Img.Symbols) {
    if (!A.row("symbol"))
      break;
    A.s("name", S.Name);
    A.u("address", S.Addr);
    A.u("size", S.Size);
    A.n("function", S.IsFunc);
    A.end();
  }
}

inline bool writeFile(const std::filesystem::path &Path,
                      std::string_view Bytes) {
  std::ofstream Out(Path, std::ios::binary | std::ios::out);
  if (!Out)
    return false;
  Out.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
  Out.close();
  return !Out.fail();
}

// Supplemental rejected scalar queries from the same opt-in CI capture.
// These are observations of completed false results, not a proof history or
// necessarily the decisive query at the eventual first-fatal address.
inline void scalarOffsetRejection(const MedFunc *Func, const MedVar &Query,
                                  const MedVar *Forbidden, const char *Reason,
                                  const MedVar &Rejected, int Depth,
                                  int ProofNodesLeft, int FrameNodesLeft,
                                  int ReachNodesLeft) noexcept {
  const char *Root = std::getenv("NEVERD_CI_FAILURE_SNAPSHOT_DIR");
  const char *Selected = std::getenv("NEVERD_CI_FAILURE_SNAPSHOT_FUNCTION");
  if (!Root || !*Root || !Selected || !Func || Func->Name != Selected ||
      std::strlen(Root) > 4096)
    return;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
    namespace fs = std::filesystem;
    std::error_code EC;
    const fs::path Parent(Root);
    if (!fs::is_directory(Parent, EC) || EC)
      return;
    // At most 64 fixed slots per case, shared safely across capture threads
    // and processes through exclusive directory creation. Never overwrite.
    fs::path Dir;
    for (unsigned Slot = 0; Slot < 64; ++Slot) {
      fs::path Candidate = Parent / ("offset-rejection-" + std::to_string(Slot));
      EC.clear();
      if (fs::create_directory(Candidate, EC)) {
        Dir = std::move(Candidate);
        break;
      }
      if (EC && EC != std::errc::file_exists)
        return;
    }
    if (Dir.empty())
      return;
    Buffer Data(4096);
    Budget Limit;
    SnapshotWriter W{Limit, Data};
    if (W.row("scalar-offset-query-rejection")) {
      W.s("function", Func->Name);
      W.u("entry", Func->Entry);
      W.v("query", Query);
      if (Forbidden)
        W.v("forbidden", *Forbidden);
      W.s("first_rejection_reason", Reason ? Reason : "unlabelled-return");
      W.v("first_rejected_value", Rejected);
      W.n("first_rejection_depth", Depth);
      W.n("proof_nodes_left", ProofNodesLeft);
      W.n("frame_root_nodes_left", FrameNodesLeft);
      W.n("frame_reach_nodes_left", ReachNodesLeft);
      W.end();
    }
    if (Data.complete())
      (void)writeFile(Dir / "query.jsonl", Data.str());
  } catch (...) {
    // Observer failure never replaces or retries the original query.
  }
#else
  (void)Query;
  (void)Forbidden;
  (void)Reason;
  (void)Rejected;
  (void)Depth;
  (void)ProofNodesLeft;
  (void)FrameNodesLeft;
  (void)ReachNodesLeft;
#endif
}

// Called only inside an already-taken first-fatal branch. All arguments are
// existing values; this function must never call a provenance/ABI/CFG helper.
inline void capture(const BinaryImage *Img, const MedFunc *Func,
                    const char *Branch, const MedVar &Address,
                    bool HasEarlierCodeFatal, const MedVar *Term = nullptr,
                    std::initializer_list<std::pair<const char *, uint64_t>>
                        Facts = {}) noexcept {
  const char *Root = std::getenv("NEVERD_CI_FAILURE_SNAPSHOT_DIR");
  const char *Selected = std::getenv("NEVERD_CI_FAILURE_SNAPSHOT_FUNCTION");
  if (HasEarlierCodeFatal || !Root || !*Root || !Selected || !Img || !Func ||
      Func->Name != Selected || std::strlen(Root) > 4096)
    return;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
    namespace fs = std::filesystem;
    std::error_code EC;
    const fs::path Parent(Root);
    if (!fs::is_directory(Parent, EC) || EC)
      return;
    fs::path Dir;
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    // Directory creation arbitrates concurrent shard/process captures. No
    // shared counter, process-global observer, or thread-local state is used.
    for (unsigned Attempt = 0; Attempt < 64; ++Attempt) {
      fs::path Candidate = Parent / ("snapshot-" + std::to_string(Stamp) + "-" +
                                     std::to_string(Attempt));
      EC.clear();
      if (fs::create_directory(Candidate, EC)) {
        Dir = std::move(Candidate);
        break;
      }
      if (EC && EC != std::errc::file_exists)
        return;
    }
    if (Dir.empty())
      return;

    Budget O;
    Buffer Data(4 * 1024 * 1024);
    SnapshotWriter W{O, Data};
    if (W.row("first-fatal")) {
      W.s("branch", Branch);
      W.s("function", Func->Name);
      W.u("entry", Func->Entry);
      W.v("address", Address);
      if (Term)
        W.v("term", *Term);
      W.n("image_arch", Img->Arch);
      W.n("image_format", Img->Format);
      W.n("image_bits", Img->Bits);
      W.n("image_relocatable", Img->IsRelocatable);
      W.u("image_base", Img->Base);
      W.u("raw_size", Img->Raw.size());
      for (const auto &[Name, Value] : Facts)
        W.u(Name, Value);
      W.end();
    }
    writeGraph(O, Data, *Img, *Func);
    const bool GraphComplete = O.allows() && Data.complete();
    const bool GraphWritten =
        writeFile(Dir / "post-abi-graph.jsonl", Data.str());
    constexpr size_t RawLimit = 16 * 1024 * 1024;
    bool RawWritten = false;
    if (!Img->Raw.empty() && Img->Raw.size() <= RawLimit)
      RawWritten = writeFile(
          Dir / "loaded-image.raw",
          {reinterpret_cast<const char *>(Img->Raw.data()), Img->Raw.size()});

    Buffer Manifest(8192);
    Manifest.append("{\"schema\":1,\"scope\":\"first-fatal-post-abi-graph\"");
    Manifest.append(",\"branch\":");
    Manifest.json(Branch);
    Manifest.append(",\"function\":");
    Manifest.json(Func->Name);
    Manifest.append(",\"entry\":");
    Manifest.number(Func->Entry);
    Manifest.append(",\"record_budget_used\":");
    Manifest.number(O.Records);
    Manifest.append(",\"graph_bytes\":");
    Manifest.number(Data.size());
    Manifest.append(",\"graph_complete\":");
    Manifest.append(GraphComplete ? "true" : "false");
    Manifest.append(",\"graph_written\":");
    Manifest.append(GraphWritten ? "true" : "false");
    Manifest.append(",\"raw_bytes\":");
    Manifest.number(Img->Raw.size());
    Manifest.append(",\"raw_written\":");
    Manifest.append(RawWritten ? "true" : "false");
    Manifest.append(",\"raw_limit_bytes\":");
    Manifest.number(RawLimit);
    Manifest.append(",\"complete_ir_model\":false,\"proof_history\":false");
    Manifest.append(",\"low_ir\":false,\"operation_locator\":false,\"original_"
                    "object_equality\":\"unchecked\"}\n");
    if (Manifest.complete())
      (void)writeFile(Dir / "capture.json", Manifest.str());
  } catch (...) {
    // A missing/partial receipt is unavailable evidence. It must not replace
    // the original native failure with an exception or change its result.
  }
#else
  // No allocation/file capture is attempted without exception containment.
  // The collector reports the absent receipt; it does not infer success.
  (void)Branch;
  (void)Address;
  (void)Term;
  (void)Facts;
#endif
}
} // namespace neverd::detail::failure_snapshot
#endif // NEVERD_PRIVATE_MEDLLVMFAILURESNAPSHOT_H
