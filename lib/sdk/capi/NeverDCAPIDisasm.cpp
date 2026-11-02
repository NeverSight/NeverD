//===- NeverDCAPIDisasm.cpp - C API: disassembly --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Disassembly functions: JSON array output and annotated text output.
///
//===----------------------------------------------------------------------===//

#include "BytecodeOperands.h"
#include "InstructionFlow.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/Limits.h"
#include "neverd/decode/NativeOperands.h"
#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/analysis/SBFFunctionBody.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <optional>

using namespace neverd;
using namespace neverd::sdk;

namespace {
// Keep raw decoder operands available to clients such as the GUI's listing.
// Only the display field/text consumes the shared enum formatter.
/// \p Raw, the operands of the instruction at \p Addr, with the enumerators
/// the user chose naming the immediates \p Immediate types.
std::string enumOperands(
    Session &S, va_t Addr, const std::string &Raw,
    llvm::function_ref<std::optional<NativeImmediate>(unsigned)> Immediate) {
  // An instruction without an enumerator keeps the decoder's text as is.
  const auto At = S.OperandFormats.find(Addr);
  if (At == S.OperandFormats.end() ||
      llvm::none_of(At->second, [](const auto &Entry) {
        return Entry.second.Base == "enum";
      }))
    return Raw;
  // Operands as clients count them: commas outside brackets, braces and
  // parentheses separate them.
  std::vector<std::string> Parts;
  size_t Start = 0;
  int Depth = 0;
  for (size_t N = 0; N <= Raw.size(); ++N) {
    const char C = N < Raw.size() ? Raw[N] : '\0';
    if (C == '[' || C == '{' || C == '(')
      ++Depth;
    else if ((C == ']' || C == '}' || C == ')') && Depth > 0)
      --Depth;
    if (N == Raw.size() || (C == ',' && Depth == 0)) {
      Parts.push_back(llvm::StringRef(Raw).slice(Start, N).trim().str());
      Start = N + 1;
    }
  }
  bool Changed = false;
  for (const auto &[Index, Format] : At->second) {
    if (Format.Base != "enum" || Index >= Parts.size())
      continue;
    const auto O = Immediate(Index);
    if (!O)
      continue;
    const auto Previous = S.LastError;
    const auto Value = "0x" + llvm::utohexstr(O->Value);
    const char *Text = neverd_enum_value_json(
        &S, Format.Type.c_str(), Value.c_str(), O->Bits, Format.Delta != 0);
    S.LastError = Previous;
    if (!Text)
      continue;
    auto Parsed = llvm::json::parse(Text);
    neverd_free_string(Text);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      continue;
    }
    const auto *Object = Parsed->getAsObject();
    if (const auto Name = Object ? Object->getString("text") : std::nullopt) {
      // An ARM-family immediate keeps its `#`.
      Parts[Index] =
          (llvm::StringRef(Parts[Index]).starts_with("#") ? "#" : "") +
          Name->str();
      Changed = true;
    }
  }
  if (!Changed)
    return Raw;
  std::string Result;
  for (const auto &Part : Parts) {
    if (!Result.empty())
      Result += ", ";
    Result += Part;
  }
  return Result;
}

std::string displayOperands(Session &S, const Decoder &D,
                            const DecodedInsn &I) {
  std::optional<NativeOperands> Facts;
  return enumOperands(
      S, I.Addr, I.Raw ? I.Raw->op_str : "", [&](unsigned Index) {
        if (!Facts)
          Facts = describeNativeOperands(D, I, S.Img.Arch, false);
        return nativeImmediateAt(*Facts, I, S.Img.Arch, Index);
      });
}

std::string evmBytes(const evm::LowInstruction &Instruction) {
  std::string Bytes;
  for (uint8_t Byte : Instruction.Encoding)
    Bytes += llvm::utohexstr(Byte, /*LowerCase=*/true, evm::kHexDigitsPerByte);
  return Bytes;
}

std::string sbfBytes(const sbf::LowInstruction &Instruction) {
  std::string Bytes;
  for (uint8_t Byte : Instruction.Encoding)
    Bytes += llvm::utohexstr(Byte, /*LowerCase=*/true, evm::kHexDigitsPerByte);
  return Bytes;
}

llvm::BitVector functionSlots(const sbf::SBFProgram &Program,
                              const sbf::Function &Function) {
  llvm::BitVector Slots(Program.Low.Instructions.size());
  const sbf::FunctionBodyIndex FunctionBodies(Program);
  for (size_t BlockID : FunctionBodies.blocks(Function)) {
    if (BlockID >= Program.Low.Blocks.size())
      continue;
    const sbf::BasicBlock &Block = Program.Low.Blocks[BlockID];
    const size_t End =
        std::min(Block.EndSlot, static_cast<size_t>(Slots.size()));
    for (size_t Slot = Block.StartSlot; Slot < End; ++Slot)
      Slots.set(Slot);
  }
  return Slots;
}

template <typename Visitor>
void visitInstructionReferences(const BinaryImage &Img,
                                const InstructionFlow &Flow, Visitor Visit) {
  for (const InstructionRef &Ref : Flow.Refs)
    Visit(Ref);
  // A transfer through a slot the loader relocated to code reaches the
  // slot's pointer as loaded. Raw and recovered views share this authority.
  if (Flow.Kind == "icall" || Flow.Kind == "ijump")
    for (const InstructionRef &Ref : Flow.Refs)
      if (Ref.Kind == "read" && Img.CodePtrRelocSlots.count(Ref.To))
        if (const auto Target = relocatedPointer(Img, Ref.To))
          Visit(InstructionRef{*Target, Flow.Kind, 0});
}

llvm::json::Object nativeInstructionRow(Session *S, const DecodedInsn &DI,
                                        const uint8_t *Bytes, int Sz,
                                        unsigned Options,
                                        InstructionFlowScratch &Scratch) {
  const TargetRegInfo &Registers = getTargetRegInfo(S->Img.Arch);
  std::string BytesHex;
  for (int J = 0; J < Sz; ++J) {
    char Buf[4];
    snprintf(Buf, sizeof(Buf), "%02x", Bytes[J]);
    BytesHex += Buf;
  }

  llvm::json::Object Obj;
  Obj["addr"] = vaHex(DI.Addr);
  Obj["size"] = Sz;
  Obj["mnemonic"] = std::string(DI.Raw ? DI.Raw->mnemonic : "");
  Obj["op_str"] = std::string(DI.Raw ? DI.Raw->op_str : "");
  const auto Display = displayOperands(*S, S->Dec, DI);
  if (DI.Raw && Display != DI.Raw->op_str)
    Obj["display_op_str"] = Display;
  Obj["bytes"] = BytesHex;
  if ((Options & NEVERD_DISASM_OPERANDS) &&
      (S->Img.Arch == Arch::X86 || S->Img.Arch == Arch::X64) && DI.Raw &&
      DI.Raw->detail) {
    llvm::json::Array Bits;
    const auto &Operands = DI.Raw->detail->x86;
    for (unsigned I = 0; I < Operands.op_count; ++I)
      Bits.push_back(Operands.operands[I].size * 8);
    Obj["operand_bits"] = std::move(Bits);
  }
  if (Options & (NEVERD_DISASM_FLOW | NEVERD_DISASM_STACK)) {
    const InstructionFlow Flow =
        Scratch.summarize(S->Img, S->Dec, DI,
                          Options & NEVERD_DISASM_STACK ? &Registers : nullptr);
    if (Options & NEVERD_DISASM_FLOW) {
      if (!Flow.Kind.empty())
        Obj["flow"] = Flow.Kind.str();
      if (Flow.Target != InvalidVA)
        Obj["target"] = vaHex(Flow.Target);
      if (!Flow.Refs.empty()) {
        llvm::json::Array Refs;
        visitInstructionReferences(
            S->Img, Flow, [&](const InstructionRef &Ref) {
              llvm::json::Object Row{{"to", vaHex(Ref.To)},
                                     {"kind", Ref.Kind.str()}};
              if (Ref.Bytes)
                Row["size"] = static_cast<int64_t>(Ref.Bytes);
              Refs.push_back(std::move(Row));
            });
        Obj["refs"] = std::move(Refs);
      }
    }
    if (Options & NEVERD_DISASM_STACK) {
      const char *Base =
          Flow.Stack.Base && Registers.GetRegName
              ? Registers.GetRegName(*Flow.Stack.Base, Registers.PointerSize)
              : nullptr;
      if (Flow.Stack.Unknown || (Flow.Stack.Base && !Base)) {
        Obj["sp"] = nullptr;
      } else {
        Obj["sp"] = Flow.Stack.Delta;
        // Spelled like the operand text.
        if (Base)
          Obj["sp_base"] = llvm::StringRef(Base).lower();
      }
    }
  }
  return Obj;
}

} // namespace

// ===--------------------------------------------------------------------===//
// Disassembly (JSON array)
// ===--------------------------------------------------------------------===//

const char *neverd_disasm_json(neverd_session_t Sess, neverd_va_t Addr,
                               int MaxInsns) {
  return neverd_disasm_json_ex(Sess, Addr, MaxInsns, 0);
}

const char *neverd_disasm_json_ex(neverd_session_t Sess, neverd_va_t Addr,
                                  int MaxInsns, unsigned Options) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return dupStr(std::string("[]"));
  }

  if (S->Img.Arch == Arch::EVM) {
    if (!S->ensurePipeline() || !S->PipeResult.EVM)
      return dupStr(std::string("[]"));
    llvm::json::Array EVMInstructions;
    int Count = 0;
    for (const auto &Instruction : S->PipeResult.EVM->Low.Instructions) {
      if (Instruction.PC < Addr)
        continue;
      if (MaxInsns > 0 && Count >= MaxInsns)
        break;
      llvm::json::Object Object;
      Object["addr"] = vaHex(Instruction.PC);
      Object["size"] = static_cast<int64_t>(Instruction.Encoding.size());
      Object["mnemonic"] = std::string(Instruction.Info.Name);
      const std::string Operands = evm::formatImmediate(Instruction);
      Object["op_str"] = Operands;
      const std::string Display =
          enumOperands(*S, Instruction.PC, Operands, [&](unsigned Index) {
            return evmImmediate(Instruction, Index);
          });
      if (Display != Operands)
        Object["display_op_str"] = Display;
      Object["decode_status"] =
          evm::opcodeDecodeStatusName(Instruction.DecodeStatus);
      Object["immediate_status"] =
          evm::immediateDecodeStatusName(Instruction.ImmediateStatus);
      Object["bytes"] = evmBytes(Instruction);
      EVMInstructions.push_back(std::move(Object));
      ++Count;
    }
    return dupStr(jsonToString(llvm::json::Value(std::move(EVMInstructions))));
  }

  if (S->Img.Arch == Arch::SBF) {
    if (!S->ensurePipeline() || !S->PipeResult.SBF)
      return dupStr(std::string("[]"));
    llvm::json::Array Instructions;
    int Count = 0;
    llvm::BitVector SelectedSlots;
    if (MaxInsns <= 0)
      for (const sbf::Function &Function : S->PipeResult.SBF->High.Functions)
        if (Function.Address == Addr) {
          SelectedSlots = functionSlots(*S->PipeResult.SBF, Function);
          break;
        }
    for (const auto &Instruction : S->PipeResult.SBF->Low.Instructions) {
      if (Instruction.Address < Addr || Instruction.IsContinuation)
        continue;
      if (!SelectedSlots.empty() && !SelectedSlots.test(Instruction.Slot))
        continue;
      if (MaxInsns > 0 && Count >= MaxInsns)
        break;
      llvm::json::Object Object;
      Object["addr"] = vaHex(Instruction.Address);
      Object["size"] =
          static_cast<int64_t>(Instruction.SlotWidth * sbf::kInstructionSize);
      Object["mnemonic"] =
          Instruction.Info ? Instruction.Info->Mnemonic.str() : ".byte";
      // The operands alone: the mnemonic has its own field. An invalid slot
      // keeps the rule it breaks.
      const std::string Operands =
          Instruction.isInvalid() ? sbf::formatInstruction(Instruction)
          : Instruction.Info ? sbf::formatOperands(Instruction)
                             : "0x" + llvm::utohexstr(Instruction.RawOpcode);
      Object["op_str"] = jsonSafeText(Operands);
      const std::string Display =
          enumOperands(*S, Instruction.Address, Operands, [&](unsigned Index) {
            return sbfImmediate(Instruction, Index);
          });
      if (Display != Operands)
        Object["display_op_str"] = jsonSafeText(Display);
      Object["bytes"] = sbfBytes(Instruction);
      Instructions.push_back(std::move(Object));
      ++Count;
    }
    return dupStr(jsonToString(llvm::json::Value(std::move(Instructions))));
  }

  if (!S->synchronizeFunctions())
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  uint64_t Span = 0;
  for (const auto &F : S->Functions) {
    if (F.Entry == Addr) {
      Span = F.Size;
      break;
    }
  }
  uint64_t Limit =
      MaxInsns > 0
          ? static_cast<uint64_t>(MaxInsns)
          : (Span > 0 ? Span
                      : ::neverd::sdk::
                            kNeverDCAPIDisasmNeverdDisasmJsonExLimitDefault);

  InstructionFlowScratch Scratch;
  const bool ModeKnown = decodeNativeRange(
      S->Img, S->Dec, Addr, Span, Limit,
      [&](const DecodedInsn &DI, const uint8_t *Bytes, int Sz) {
        auto Obj = nativeInstructionRow(S, DI, Bytes, Sz, Options, Scratch);
        Arr.push_back(std::move(Obj));
        return true;
      });
  if (!ModeKnown)
    S->setError("unknown or conflicting instruction mode at address");

  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

// ===--------------------------------------------------------------------===//
// Direct references (JSON object)
// ===--------------------------------------------------------------------===//

const char *neverd_function_disasm_json(neverd_session_t Sess,
                                        neverd_va_t Entry,
                                        neverd_va_t FirstAddress, int MaxInsns,
                                        unsigned Options) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded || S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("function disassembly requires a loaded native image");
    return nullptr;
  }
  if (!neverd_prepare_function_view(Sess, Entry, "low") &&
      !S->findLowFunc(Entry))
    return nullptr;
  const LowFunc *Function = S->findLowFunc(Entry);
  if (!Function) {
    S->setError("no recovered native function at " + vaHex(Entry));
    return nullptr;
  }
  std::map<va_t, const LowInstructionBoundary *> Boundaries;
  for (const LowBlock &Block : Function->Blocks)
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
      Boundaries.emplace(Boundary.Address, &Boundary);
  const size_t Limit = std::clamp(MaxInsns, 1, kMaxFunctionDisasmPage);
  llvm::json::Array Rows;
  InstructionFlowScratch Scratch;
  auto At = Boundaries.lower_bound(FirstAddress);
  for (; At != Boundaries.end() && Rows.size() < Limit; ++At) {
    const auto &Boundary = *At->second;
    const auto *Bytes = S->Img.readVA(Boundary.Address, Boundary.Size);
    if (!Bytes || !Boundary.Size ||
        !S->Dec.selectMode(S->Img, Boundary.Address, Boundary.Mode)) {
      S->setError(
          "recovered instruction has no matching readable bytes or mode");
      return nullptr;
    }
    DecodedInsn Instruction;
    if (S->Dec.decodeOne(Bytes, Boundary.Size, Boundary.Address, Instruction) !=
        Boundary.Size) {
      S->setError("recovered instruction boundary disagrees with the decoder");
      return nullptr;
    }
    auto Row = nativeInstructionRow(S, Instruction, Bytes, Boundary.Size,
                                    Options, Scratch);
    if ((Options & NEVERD_DISASM_FLOW) &&
        Boundary.Control == LowInstructionControl::Branch &&
        Boundary.Immediate &&
        !hasLowInstructionControlFlag(Boundary.ControlFlags,
                                      LowInstructionControlFlag::Indirect)) {
      Row["flow"] =
          hasLowInstructionControlFlag(Boundary.ControlFlags,
                                       LowInstructionControlFlag::Conditional)
              ? "cjump"
              : "jump";
      Row["target"] = vaHex(*Boundary.Immediate);
    }
    Rows.push_back(std::move(Row));
  }
  S->clearError();
  return dupStr(jsonToString(llvm::json::Object{
      {"instructions", std::move(Rows)},
      {"next_address", At == Boundaries.end()
                           ? llvm::json::Value(nullptr)
                           : llvm::json::Value(vaHex(At->first))},
      {"lift_complete", Function->hasCompleteInstructionLift()}}));
}

const char *neverd_code_refs_json(neverd_session_t Sess, neverd_va_t FirstEntry,
                                  int MaxFunctions) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("direct references are only published for native images");
    return nullptr;
  }
  if (!S->synchronizeFunctions())
    return nullptr;
  const FunctionPage Page = functionPage(*S, FirstEntry, MaxFunctions);
  struct Ref {
    va_t From, To;
    llvm::StringRef Kind;
    unsigned Bytes = 0;
  };
  // Each function's references, merged in entry order.
  std::vector<std::vector<Ref>> Slots(Page.Functions.size());
  const bool Decoded = decodeFunctions(
      S->Img, Page,
      [&](size_t Index, Decoder &Dec, const DecodedInsn &DI,
          InstructionFlowScratch &Scratch) {
        auto &Out = Slots[Index];
        const InstructionFlow Flow = Scratch.summarize(S->Img, Dec, DI);
        if (Flow.Target != InvalidVA && !Flow.Kind.empty() &&
            Flow.Kind != UnliftedFlow)
          Out.push_back({DI.Addr, Flow.Target, Flow.Kind});
        visitInstructionReferences(S->Img, Flow, [&](const InstructionRef &R) {
          Out.push_back({DI.Addr, R.To, R.Kind, R.Bytes});
        });
      });
  if (!Decoded) {
    S->setError("failed to initialize a decoder for the image");
    return nullptr;
  }
  llvm::json::Array Refs;
  for (const auto &Slot : Slots)
    for (const Ref &R : Slot) {
      llvm::json::Array Row{vaHex(R.From), vaHex(R.To), R.Kind.str()};
      if (R.Bytes)
        Row.push_back(static_cast<int64_t>(R.Bytes));
      Refs.push_back(std::move(Row));
    }
  llvm::json::Object Result;
  Result["refs"] = std::move(Refs);
  Result["next_entry"] =
      Page.NextEntry ? llvm::json::Value(vaHex(*Page.NextEntry)) : nullptr;
  Result["function_count"] = static_cast<int64_t>(S->Functions.size());
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_pointer_refs_json(neverd_session_t Sess,
                                     neverd_va_t FirstSlot, int MaxSlots) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  const size_t Limit =
      static_cast<size_t>(std::clamp(MaxSlots, 1, kMaxDataSlotsPerQuery));
  // Both slot sets are ordered; walk them as one ascending sequence.
  const auto &Code = S->Img.CodePtrRelocSlots;
  const auto &Data = S->Img.DataPtrRelocSlots;
  auto CodeIt = Code.lower_bound(FirstSlot);
  auto DataIt = Data.lower_bound(FirstSlot);
  llvm::json::Array Refs;
  size_t Visited = 0;
  std::optional<va_t> Next;
  while (CodeIt != Code.end() || DataIt != Data.end()) {
    va_t Slot = 0;
    if (DataIt == Data.end() || (CodeIt != Code.end() && *CodeIt <= *DataIt)) {
      Slot = *CodeIt++;
      if (DataIt != Data.end() && *DataIt == Slot)
        ++DataIt;
    } else {
      Slot = *DataIt++;
    }
    if (Visited == Limit) {
      Next = Slot;
      break;
    }
    ++Visited;
    if (const auto Target = relocatedPointer(S->Img, Slot))
      Refs.push_back(llvm::json::Array{vaHex(Slot), vaHex(*Target), "offset"});
  }
  llvm::json::Object Result;
  Result["refs"] = std::move(Refs);
  Result["next_slot"] = Next ? llvm::json::Value(vaHex(*Next)) : nullptr;
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

int neverd_pointer_at(neverd_session_t Sess, neverd_va_t Address,
                      neverd_va_t *Slot, neverd_va_t *Target) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return 0;
  const va_t Size = S->Img.getPointerSize();
  // The nearest slot that starts at or before Address in either set.
  std::optional<va_t> Found;
  for (const auto *Slots :
       {&S->Img.CodePtrRelocSlots, &S->Img.DataPtrRelocSlots}) {
    auto It = Slots->upper_bound(Address);
    if (It != Slots->begin() && Address - *std::prev(It) < Size &&
        (!Found || *std::prev(It) > *Found))
      Found = *std::prev(It);
  }
  if (!Found)
    return 0;
  const auto Pointer = relocatedPointer(S->Img, *Found);
  if (!Pointer)
    return 0;
  if (Slot)
    *Slot = *Found;
  if (Target)
    *Target = *Pointer;
  return 1;
}

// ===--------------------------------------------------------------------===//
// Disassembly (annotated text)
// ===--------------------------------------------------------------------===//

const char *neverd_disasm_text(neverd_session_t Sess,
                               const char *FuncNameOrAddr, int Annotate) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded)
    return nullptr;

  if (S->Img.Arch == Arch::EVM) {
    if (!S->ensurePipeline() || !S->PipeResult.EVM)
      return nullptr;
    std::string Buffer;
    llvm::raw_string_ostream OS(Buffer);
    OS << "; " << kEVMEntrySymbolName << " (0x"
       << llvm::utohexstr(evm::kEntryPC) << ", " << S->Img.Raw.size()
       << " bytes)\n";
    for (const auto &Instruction : S->PipeResult.EVM->Low.Instructions) {
      OS << "  0x" << llvm::utohexstr(Instruction.PC) << "  ";
      const std::string Bytes = evmBytes(Instruction);
      OS << Bytes;
      if (Bytes.size() < kShortInstructionByteColumnWidth)
        OS.indent(kShortInstructionByteColumnWidth - Bytes.size());
      OS << " " << Instruction.Info.Name;
      const std::string Immediate = evm::formatImmediate(Instruction);
      if (!Immediate.empty())
        OS << " " << Immediate;
      const std::string Annotation = evm::formatDecodeAnnotation(Instruction);
      if (!Annotation.empty())
        OS << " ; " << Annotation;
      OS << "\n";
    }
    return dupStr(Buffer);
  }

  if (S->Img.Arch == Arch::SBF) {
    if (!S->ensurePipeline() || !S->PipeResult.SBF)
      return nullptr;
    std::string Buffer;
    llvm::raw_string_ostream OS(Buffer);
    const auto &Program = *S->PipeResult.SBF;
    const llvm::StringRef Identifier = FuncNameOrAddr ? FuncNameOrAddr : "";
    const sbf::Function *Function = sbf::findFunction(Program, Identifier);
    if (!Function) {
      S->setError("SBF function not found: " + Identifier.str());
      return nullptr;
    }
    const llvm::BitVector SelectedSlots = functionSlots(Program, *Function);
    OS << "; " << Function->Name << " (0x" << llvm::utohexstr(Function->Address)
       << ", " << SelectedSlots.count() * sbf::kInstructionSize << " bytes, "
       << sbf::versionDisplayName(Program.Low.TheVersion) << ")\n";
    for (const auto &Instruction : Program.Low.Instructions) {
      if (Instruction.IsContinuation || !SelectedSlots.test(Instruction.Slot))
        continue;
      OS << "  0x" << llvm::utohexstr(Instruction.Address) << "  ";
      const std::string Bytes = sbfBytes(Instruction);
      OS << Bytes;
      if (Bytes.size() < kShortInstructionByteColumnWidth)
        OS.indent(kShortInstructionByteColumnWidth - Bytes.size());
      OS << " " << sbf::formatInstruction(Instruction) << "\n";
    }
    return dupStr(Buffer);
  }

  if (!S->synchronizeFunctions())
    return nullptr;

  std::optional<FuncInfo> Function;
  const Symbol *Sym = nullptr;
  std::string FN(FuncNameOrAddr ? FuncNameOrAddr : "");
  if (FN.size() > 2 && (FN.substr(0, 2) == "0x" || FN.substr(0, 2) == "0X")) {
    llvm::StringRef Ref(FN);
    Ref = Ref.drop_front(2);
    va_t Addr = 0;
    if (!Ref.getAsInteger(16, Addr)) {
      for (const auto &F : S->Functions) {
        if (F.Entry == Addr) {
          Function = F;
          break;
        }
      }
      if (!Function)
        Sym = S->Img.findSymbolAt(Addr);
    }
  } else {
    const auto Index = S->functionIndexNamed(FN);
    if (Index && S->Functions[*Index].Name == FN)
      Function = S->Functions[*Index];
    // A loader symbol still shows its original text after a user rename.
    // With neither exact spelling, share export and C-name alias lookup.
    else if (!(Sym = S->Img.findSymbol(FN)) && Index)
      Function = S->Functions[*Index];
  }
  if (!Function) {
    if (!Sym)
      return nullptr;
    Function = FuncInfo{Sym->Addr, Sym->Size, Sym->Name};
  }

  const Segment *Seg = nullptr;
  for (const auto &Sg : S->Img.Segments)
    if (Sg.contains(Function->Entry)) {
      Seg = &Sg;
      break;
    }
  if (!Seg)
    return nullptr;

  if (!S->Dec.init(S->Img))
    return nullptr;

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << "; " << Function->Name << " (0x" << llvm::utohexstr(Function->Entry)
     << ", " << Function->Size << " bytes)\n";

  va_t Addr = Function->Entry;
  std::optional<InstructionMode> PathMode = S->Img.instructionModeAt(Addr);
  uint64_t Span = Function->Size > 0 ? Function->Size : 0x100;
  while (Addr >= Function->Entry && Addr - Function->Entry < Span &&
         Seg->contains(Addr)) {
    uint64_t Off64 = Addr - Seg->VA;
    // contains() only checks the VA span (Seg->Size), which can exceed the
    // materialized bytes; guard so Remain does not underflow into a huge
    // length.
    if (Off64 >= Seg->Data.size())
      break;
    size_t Off = static_cast<size_t>(Off64);
    const uint8_t *Bytes = Seg->Data.data() + Off;
    size_t Remain = static_cast<size_t>(std::min<uint64_t>(
        Seg->Size - Off64,
        std::min<uint64_t>(Seg->Data.size() - Off,
                           Span - (Addr - Function->Entry))));
    if (!S->Dec.selectMode(S->Img, Addr, PathMode)) {
      S->setError("unknown or conflicting instruction mode at address");
      break;
    }
    PathMode = S->Dec.currentMode();
    DecodedInsn Insn;
    int Sz = S->Dec.decodeOne(Bytes, Remain, Addr, Insn);
    if (Sz <= 0)
      break;
    if (S->Img.Arch == Arch::ARM &&
        S->Img.instructionModeAt(Addr + Sz - 1, S->Dec.currentMode()) !=
            S->Dec.currentMode())
      break;
    OS << "  0x" << llvm::utohexstr(Addr) << "  ";
    for (int I = 0; I < Sz && I < 8; ++I)
      OS << llvm::format("%02x ", Bytes[I]);
    for (int I = Sz; I < 8; ++I)
      OS << "   ";
    OS << " " << (Insn.Raw ? Insn.Raw->mnemonic : "");
    if (Insn.Raw && Insn.Raw->op_str[0])
      OS << "\t" << displayOperands(*S, S->Dec, Insn);

    if (Annotate && Insn.Raw && Insn.Raw->op_str[0]) {
      std::string OpStr(Insn.Raw->op_str);
      auto HexPos = OpStr.find("0x");
      if (HexPos != std::string::npos) {
        size_t HexBegin = HexPos + 2;
        size_t HexEnd = HexBegin;
        while (HexEnd < OpStr.size() &&
               std::isxdigit(static_cast<unsigned char>(OpStr[HexEnd])))
          ++HexEnd;
        va_t OpAddr = 0;
        llvm::StringRef HexDigits(OpStr.data() + HexBegin, HexEnd - HexBegin);
        if (HexDigits.empty() || HexDigits.getAsInteger(16, OpAddr))
          OpAddr = 0;
        if (OpAddr > 0 && OpAddr != Addr) {
          const char *Ref = neverd_resolve_addr(Sess, OpAddr);
          if (Ref) {
            auto Parsed = llvm::json::parse(Ref);
            if (Parsed) {
              if (auto *RObj = Parsed->getAsObject()) {
                auto Type = RObj->getString("type").value_or("");
                if (Type == "function")
                  OS << " ; " << RObj->getString("name").value_or("");
                else if (Type == "import")
                  OS << " ; [import] " << RObj->getString("name").value_or("");
                else if (Type == "export")
                  OS << " ; [export] " << RObj->getString("name").value_or("");
                else if (Type == "string")
                  OS << " ; \"" << RObj->getString("value").value_or("")
                     << "\"";
              }
            }
            neverd_free_string(Ref);
          }
        }
      }
    }
    OS << "\n";
    if (static_cast<uint64_t>(Sz) > InvalidVA - Addr)
      break;
    Addr += Sz;
  }
  return dupStr(Buf);
}
