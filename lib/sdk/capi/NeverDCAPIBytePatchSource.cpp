//===- NeverDCAPIBytePatchSource.cpp - Source patch evidence --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recheck exact emitted anchors, canonical instructions and operands.
//===----------------------------------------------------------------------===//
#include "SessionImpl.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/NativeOperands.h"
#include "neverd/patch/PatchActions.h"
#include "neverd/sdk/NeverDCAPIBytePatch.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/IntegerLiterals.h"

#include "llvm/ADT/StringExtras.h"

using namespace neverd;
using namespace neverd::sdk;
namespace {
using OwnedText = std::unique_ptr<const char, decltype(&neverd_free_string)>;
Session::FunctionSource *source(Session &S, va_t Function,
                                const char *Representation) {
  if (!Representation || (llvm::StringRef(Representation) != "c" &&
                          llvm::StringRef(Representation) != "llvmc")) {
    S.setError(
        "byte patch proof requires the exact C or LLVM-to-C representation");
    return nullptr;
  }
  OwnedText Page(neverd_ir_view_json(&S, Function, Representation, 0, 1),
                 neverd_free_string);
  if (!Page)
    return nullptr;
  auto *Source =
      S.findFunctionSource(Function, llvm::StringRef(Representation) == "c"
                                         ? Session::SourceRoute::PlainC
                                         : Session::SourceRoute::LLVMC);
  if (!Source || !Source->Map) {
    S.setError("source emission has no exact mapping");
    return nullptr;
  }
  return Source;
}
std::string address(va_t At) { return "0x" + llvm::utohexstr(At, true); }
// A return anchor identifies the RET, not the instruction that supplied its
// folded literal. Prove the value through local copies/zero extensions only;
// arithmetic, calls, partial writes and predecessor merges remain unsupported.
std::optional<va_t>
returnLiteralDefinition(const LowBlock &Block, size_t ReturnIndex,
                        uint64_t Literal,
                        const std::set<std::pair<va_t, int>> &Origins) {
  const auto &Return = Block.Ops[ReturnIndex];
  if (Return.Opcode != NdOp::RETURN || Return.NumInputs != 1)
    return std::nullopt;
  NdVar Value = Return.Inputs[0];
  for (size_t I = ReturnIndex; I != 0;) {
    const auto &Op = Block.Ops[--I];
    if (Op.Opcode == NdOp::NOP)
      continue;
    if (Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT)
      return std::nullopt;
    if ((Value.Space != VnodeSpace::REG && Value.Space != VnodeSpace::TEMP) ||
        !Value.Size || Value.Size > sizeof(uint64_t))
      return std::nullopt;
    const auto &Output = Op.Output;
    if (Output.Space != Value.Space || !Output.Size ||
        (Output.Offset >= Value.Offset
             ? Output.Offset - Value.Offset >= Value.Size
             : Value.Offset - Output.Offset >= Output.Size))
      continue;
    if (Output.Offset != Value.Offset || Output.Size != Value.Size ||
        Op.NumInputs != 1 || !Origins.contains({Op.Addr, Op.Seq}))
      return std::nullopt;
    const auto &Input = Op.Inputs[0];
    if (!Input.Size || Input.Size > Output.Size ||
        (Op.Opcode == NdOp::COPY && Input.Size != Output.Size))
      return std::nullopt;
    if (Input.isConst()) {
      if (Op.Opcode == NdOp::COPY && !isAddressProvenance(Input.Provenance) &&
          Input.Offset == Literal)
        return Op.Addr;
      return std::nullopt;
    }
    Value = Input;
  }
  return std::nullopt;
}
} // namespace

const char *neverd_byte_patch_source_target_json(neverd_session_t Sess,
                                                 neverd_va_t Function,
                                                 const char *Representation,
                                                 size_t Line, int64_t Literal) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return nullptr;
  S->clearError();
  auto Fail = [&](llvm::StringRef Message) -> const char * {
    S->setError(Message.str());
    return nullptr;
  };
  auto *Source = source(*S, Function, Representation);
  if (!Source)
    return nullptr;
  llvm::StringRef Full(Source->Text);
  size_t Start = 0;
  bool InComment = false;
  std::vector<IntegerLiteral> Literals;
  for (size_t I = 0; I <= Line; ++I) {
    if (Start >= Full.size())
      return Fail("source line is outside the emitted function");
    auto End = Full.find('\n', Start);
    if (End == llvm::StringRef::npos)
      End = Full.size();
    Literals =
        integerLiterals(std::string_view(Full.data() + Start, End - Start),
                        LiteralSyntax::C, InComment);
    if (I == Line)
      break;
    Start = End + 1;
  }
  const auto End = Full.find('\n', Start);
  const auto LineEnd = End == llvm::StringRef::npos ? Full.size() : End;
  size_t Begin = Start, Finish = LineEnd;
  if (Literal < -1)
    return Fail("invalid source literal index");
  if (Literal >= 0) {
    if (Literals.size() != 1)
      return Fail(
          "source line has multiple literals; an operand edit is ambiguous");
    if (uint64_t(Literal) >= Literals.size() || !Literals[Literal].Exact)
      return Fail("source literal has no exact integer value");
    Begin += Literals[Literal].Begin;
    Finish = Start + Literals[Literal].End;
  }
  std::set<va_t> Addresses;
  std::set<std::pair<va_t, int>> SelectedOrigins;
  std::set<std::pair<va_t, int>> Origins;
  const auto *Low = S->findLowFunc(Function);
  if (!Low)
    return Fail("source proof has no canonical LowIR");
  for (const auto &B : Low->Blocks)
    for (const auto &Boundary : B.InstructionBoundaries) {
      if (!Boundary.Size || Boundary.FirstOp > B.Ops.size() ||
          Boundary.OpCount > B.Ops.size() - Boundary.FirstOp)
        continue;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I) {
        const auto &Op = B.Ops[I];
        if (Op.Seq >= 0 && Op.Addr == Boundary.Address)
          Origins.emplace(Op.Addr, Op.Seq);
      }
    }
  for (const auto &Anchor : Source->Map->Anchors) {
    if (Anchor.Function != Function || Anchor.Span.Begin >= Finish ||
        Begin >= Anchor.Span.End)
      continue;
    if (Literal >= 0 && (Anchor.Span.Begin > Begin || Anchor.Span.End < Finish))
      continue;
    for (const auto &O : Anchor.Occurrences) {
      if (!Origins.contains({O.Address, O.Sequence}))
        return Fail("source occurrence is not a canonical instruction");
      Addresses.insert(O.Address);
      SelectedOrigins.emplace(O.Address, O.Sequence);
    }
  }
  if (Literal >= 0) {
    // Navigation events deliberately do not promise expression provenance.
    // Admit only an unchanged scalar COPY and a literal-only source RHS;
    // folded arithmetic, casts, addresses and compound expressions fail.
    const auto Prefix = Full.slice(Start, Begin).trim();
    const auto Suffix = Full.slice(Finish, LineEnd).trim();
    const auto LHS =
        Prefix.ends_with("=") ? Prefix.drop_back().rtrim() : llvm::StringRef();
    const bool Assignment =
        !LHS.empty() && !llvm::StringRef("+-*/%&|^!<>=?").contains(LHS.back());
    if (Suffix != ";" || (Prefix != "return" && !Assignment))
      return Fail("source literal is not a direct scalar assignment or return");
    std::set<va_t> Copies;
    for (const auto &Block : Low->Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const auto &Op = Block.Ops[I];
        if (!SelectedOrigins.contains({Op.Addr, Op.Seq}))
          continue;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].isConst() &&
            !isAddressProvenance(Op.Inputs[0].Provenance) &&
            Op.Inputs[0].Offset == Literals[Literal].Value)
          Copies.insert(Op.Addr);
        if (Prefix == "return")
          if (auto Definition = returnLiteralDefinition(
                  Block, I, Literals[Literal].Value, Origins))
            Copies.insert(*Definition);
      }
    Addresses = std::move(Copies);
  }
  if (Addresses.size() != 1)
    return Fail("source target needs exactly one proven instruction; mapping "
                "is missing or ambiguous");
  const auto PC = *Addresses.begin();
  const auto *Seg = S->Img.getSegmentFor(PC);
  Decoder Dec;
  DecodedInsn Insn{};
  if (!Seg || PC - Seg->VA >= Seg->Data.size() || !Dec.init(S->Img) ||
      !Dec.selectMode(S->Img, PC) ||
      !Dec.decodeOne(Seg->Data.data() + (PC - Seg->VA),
                     Seg->Data.size() - (PC - Seg->VA), PC, Insn))
    return Fail("source target does not decode in the image mode");
  OwnedText Context(neverd_byte_patch_context_json(Sess, PC),
                    neverd_free_string);
  if (!Context)
    return nullptr;
  auto Parsed = llvm::json::parse(Context.get());
  if (!Parsed) {
    S->setError(llvm::toString(Parsed.takeError()));
    return nullptr;
  }
  auto Result = std::move(*Parsed->getAsObject());
  Result["function"] = address(Function);
  Result["line"] = Line;
  Result["source_anchor"] = Full.slice(Start, LineEnd);
  llvm::json::Array Actions;
  if (Literal >= 0) {
    if ((S->Img.Arch != Arch::X86 && S->Img.Arch != Arch::X64) ||
        (Insn.Id != X86_INS_MOV && Insn.Id != X86_INS_MOVABS))
      return Fail(
          "source constant proof currently requires a direct MOV immediate");
    const auto Operands = describeNativeOperands(Dec, Insn, S->Img.Arch, false);
    if (Operands.Immediates.size() != 1 ||
        Operands.Immediates.front().Value != Literals[Literal].Value)
      return Fail(
          "source literal is not the unique matching immediate operand");
    patch::PatchRequest Request;
    Request.Address = PC;
    Request.Kind = patch::PatchKind::Constant;
    Request.Value = Literals[Literal].Value;
    auto Plan =
        patch::planPatch(S->Img, *S->PatchInput, S->BytePatches, Request);
    if (!Plan) {
      S->setError(llvm::toString(Plan.takeError()));
      return nullptr;
    }
    Result["operand"] = Operands.Immediates.front().Operand;
    Result["bits"] = Operands.Immediates.front().Bits;
    Result["value"] = address(Literals[Literal].Value);
    Actions.push_back("constant");
  } else {
    // Navigation alone is insufficient for removing a whole expression: a
    // folded operand may depend on other instructions. Branch rewrites affect
    // only the uniquely mapped machine condition; ambiguous conditions fail.
    for (const auto &Kind : *Result.getArray("actions"))
      if (auto Name = Kind.getAsString(); Name && Name->starts_with("branch_"))
        Actions.push_back(*Name);
    // A NOP is admitted only when this instruction emits one self-contained
    // statement and all its LowIR effects occur in that exact anchor.
    std::set<std::pair<va_t, int>> Covered;
    bool Isolated = true;
    for (const auto &Anchor : Source->Map->Anchors)
      for (const auto &O : Anchor.Occurrences)
        if (O.Address == PC) {
          Isolated &= Anchor.Span.Begin >= Start && Anchor.Span.End <= LineEnd;
          Covered.emplace(O.Address, O.Sequence);
        }
    for (const auto &O : Origins)
      if (O.first == PC && !Covered.contains(O))
        Isolated = false;
    if (Isolated)
      Actions.push_back("nop");
    if (Actions.empty())
      return Fail("source statement lacks complete instruction-effect "
                  "provenance for this edit");
  }
  Result["actions"] = std::move(Actions);
  return dupStr(jsonToString(std::move(Result)));
}

const char *neverd_source_patch_context_json(neverd_session_t Sess,
                                             neverd_va_t Function) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return nullptr;
  S->clearError();
  if (!S->BytePatches.empty()) {
    S->setError(
        "export and open the byte-patched binary before rewriting a function");
    return nullptr;
  }
  const auto *Source = source(*S, Function, "c");
  if (!Source)
    return nullptr;
  llvm::json::Array Bindings;
  std::string Entry;
  for (const auto &Name : Source->Map->Names) {
    if (!Name.Address || Name.TheKind == CSourceName::Kind::Type)
      continue;
    if (*Name.Address == Function &&
        Name.TheKind == CSourceName::Kind::Function) {
      Entry = Name.Identifier;
      continue;
    }
    const auto *Seg = S->Img.getSegmentFor(*Name.Address);
    if (!Seg)
      continue;
    Bindings.push_back(llvm::json::Object{
        {"symbol", Name.Identifier},
        {"address", address(*Name.Address)},
        {"kind", Name.TheKind == CSourceName::Kind::Function ? "function"
                 : Seg->isWritable()                         ? "data"
                                                             : "const"},
        {"size", 0}});
  }
  return dupStr(
      jsonToString(llvm::json::Object{{"source", Source->Text},
                                      {"symbol", Entry},
                                      {"address", address(Function)},
                                      {"bindings", std::move(Bindings)}}));
}

const char *neverd_byte_patch_source_preview_json(
    neverd_session_t Sess, neverd_va_t Function, const char *Representation,
    const neverd_byte_patch_options_v1 *Options) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return nullptr;
  const auto *Before = source(*S, Function, Representation);
  if (!Before)
    return nullptr;
  llvm::json::Object Result{{"before", Before->Text}};
  // Use an isolated image/pipeline; preview never mutates live byte caches or
  // invalidates a client cursor. The source options are the ordinary view's.
  Session Preview;
  Preview.RequestedLoad = S->LoadedChoice;
  Preview.DbgRequest = S->LoadedDbgRequest;
  Preview.ReadsStaticSymbols = S->LoadedStaticSymbols;
  Preview.OnlyFunctionEntries = S->OnlyFunctionEntries;
  Preview.ARMFunctionModes = S->ARMFunctionModes;
  if (!neverd_session_load(&Preview, pathToUTF8(S->FilePath).c_str())) {
    S->setError(Preview.LastError);
    return nullptr;
  }
  Preview.Img = S->Img;
  Preview.FilePath = S->FilePath;
  Preview.Loaded = true;
  Preview.BytePatches = S->BytePatches;
  Preview.PatchInput = S->PatchInput;
  Preview.FunctionEdits = S->FunctionEdits;
  Preview.FunctionAttributeEdits = S->FunctionAttributeEdits;
  Preview.Renames = S->Renames;
  Preview.Annotations = S->Annotations;
  Preview.TypeDeclarations = S->TypeDeclarations;
  Preview.TypeLibraryChoice = S->TypeLibraryChoice;
  Preview.SourceOptions = S->SourceOptions;
  Preview.SigDB = S->SigDB.snapshotForAnalysis();
  Preview.EVMFork = S->EVMFork;
  Preview.EVMStrict = S->EVMStrict;
  Preview.SBFVersion = S->SBFVersion;
  Preview.SBFStrict = S->SBFStrict;
  Preview.SBFProfile = S->SBFProfile;
  Preview.SBFIdl = S->SBFIdl;
  Preview.dropLocalTypes();
  OwnedText Applied(neverd_byte_patch_apply(&Preview, Options),
                    neverd_free_string);
  if (!Applied) {
    S->setError(Preview.LastError);
    return nullptr;
  }
  if (const auto *After = source(Preview, Function, Representation))
    Result["after"] = After->Text;
  else
    Result["after_error"] = Preview.LastError;
  return dupStr(jsonToString(std::move(Result)));
}
