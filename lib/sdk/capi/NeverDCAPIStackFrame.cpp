#include "SessionImpl.h"
#include "SourceEdits.h"

#include "neverd/ir/SourceStackFrame.h"
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <cctype>

using namespace neverd;
using namespace neverd::sdk;

std::string neverd::sdk::sourceStackSnapshot(const LowFunc &F) {
  std::string Text = sourceCallSnapshot(F);
  if (Text.empty())
    return {};
  llvm::SHA256 Hash;
  llvm::raw_string_ostream OS(Text);
  OS << ":stack-v1:";
  for (auto Root : F.ModuleAnalysisRoots)
    OS << 'R' << Root;
  for (auto Root : F.OrdinaryModuleAnalysisRoots)
    OS << 'O' << Root;
  for (const auto &T : F.FunctionTemporaries)
    OS << 'T' << T.Offset << ':' << T.Bytes;
  for (const auto &B : F.Blocks) {
    for (const auto &E : B.ExceptionalSuccs) {
      OS << 'E' << E.BlockId << ':' << E.TargetVA << ':' << unsigned(E.Kind)
         << ':' << E.SourceVA;
      if (const auto *Context = E.singleContext())
        OS << ':' << Context->RegionIndex << ':' << Context->State;
      else
        OS << ":contexts:" << E.contexts().size() << ':'
           << llvm::toHex(E.SharedContexts->digest(), true);
    }
    for (const auto &I : B.InstructionBoundaries)
      OS << 'I' << I.Address << ':' << I.Size << ':' << unsigned(I.Mode) << ':'
         << unsigned(I.Control) << ':' << I.FirstOp << ':' << I.OpCount;
    // Keep the sourceVariableSnapshot byte protocol, but do not materialize
    // every block's edge references in one unbounded temporary string.
    OS.flush();
    Hash.update(Text);
    Text.clear();
  }
  OS.flush();
  Hash.update(Text);
  return "source-variable-v1:" + llvm::toHex(Hash.final(), true);
}

SourceStackFrame
neverd::sdk::readSourceStackFrame(const llvm::json::Object &O) {
  SourceStackFrame F;
  for (const auto &[Key, Value] : O)
    if (Key != "snapshot" && Key != "input_sha256" && Key != "points" &&
        Key != "variables")
      throw std::invalid_argument("Unknown stack policy field: " + Key.str());
  if (O.get("snapshot") && !O.getString("snapshot"))
    throw std::invalid_argument("Stack snapshot must be a string");
  F.Snapshot = O.getString("snapshot").value_or("").str();
  if (const auto *Hash = O.get("input_sha256")) {
    const auto Text = Hash->getAsString();
    if (!Text || Text->size() != 64 ||
        !std::all_of(Text->begin(), Text->end(),
                     [](unsigned char C) { return llvm::isHexDigit(C); }))
      throw std::invalid_argument("Invalid stack policy input digest");
    F.InputSHA256 = Text->lower();
  }
  if (const auto *Points = O.getArray("points")) {
    if (Points->size() > kMaxStackFramePoints)
      throw std::invalid_argument("Too many SP corrections");
    for (const auto &P : *Points) {
      const auto *Row = P.getAsObject();
      const auto PC = Row ? Row->getString("address") : std::nullopt;
      const auto After = Row ? Row->getInteger("after") : std::nullopt;
      uint64_t Address;
      if (!PC || PC->getAsInteger(0, Address) || !After ||
          !F.Points.emplace(Address, *After).second)
        throw std::invalid_argument("Invalid or repeated SP correction");
    }
  } else if (O.get("points"))
    throw std::invalid_argument("points must be an array");
  if (const auto *Variables = O.getArray("variables")) {
    if (Variables->size() > kMaxStackFrameVariables)
      throw std::invalid_argument("Too many stack variables");
    for (const auto &V : *Variables) {
      const auto *Row = V.getAsObject();
      const auto At = Row ? Row->getInteger("offset") : std::nullopt;
      const auto Bytes = Row ? Row->getInteger("bytes") : std::nullopt;
      const auto Name = Row ? Row->getString("name") : std::nullopt;
      if (!At || !Bytes || *Bytes < 1 || *Bytes > 16 || !Name ||
          (Row->get("type") && !Row->getString("type")) || Name->empty() ||
          Name->size() > kMaxStackVariableNameBytes)
        throw std::invalid_argument("Invalid stack variable");
      F.Variables.push_back({*At, static_cast<uint16_t>(*Bytes), Name->str(),
                             Row->getString("type").value_or("").str()});
    }
  } else if (O.get("variables"))
    throw std::invalid_argument("variables must be an array");
  if (!F.empty() &&
      (F.Snapshot.empty() || F.Snapshot.size() > kMaxSourceSnapshotBytes))
    throw std::invalid_argument(
        "A stack edit requires an original graph snapshot");
  return F;
}

void neverd::sdk::validateSourceStackFrame(Session &S, const LowFunc &F,
                                           const SourceStackFrame &Policy) {
  if (!Policy.InputSHA256.empty() && Policy.InputSHA256 != inputSha256(S))
    throw std::invalid_argument(
        "The stack policy belongs to a different input image");
  if (!Policy.empty() && Policy.Snapshot != sourceStackSnapshot(F))
    throw std::invalid_argument(
        "The stack analysis changed; reset or reapply the correction");
  const auto Flow = analyzeSourceStackFrame(F, S.Img.Arch, Policy);
  if (!Flow.Error.empty())
    throw std::invalid_argument(Flow.Error);
  std::string Error;
  if (!projectSourceStackFrame(F, S.Img.Arch, Policy, Error))
    throw std::invalid_argument(Error);
  std::set<std::string> Names;
  std::vector<std::pair<int64_t, int64_t>> Ranges;
  for (const auto &V : Policy.Variables) {
    if (V.Offset < -(1 << 20) || V.Offset >= 0 || V.Offset + V.Bytes > 0 ||
        !(std::isalpha(static_cast<unsigned char>(V.Name.front())) ||
          V.Name.front() == '_') ||
        !std::all_of(
            V.Name.begin(), V.Name.end(),
            [](unsigned char C) { return std::isalnum(C) || C == '_'; }) ||
        !Names.insert(V.Name).second)
      throw std::invalid_argument(
          "Use distinct C identifiers for bounded local stack slots");
    // Let the C parser reject keywords, reserved declarations and invalid
    // types.
    const auto *Library = S.readLocalTypes();
    if (!Library)
      throw std::invalid_argument(S.LastError);
    auto Decl = types::parseFunctionDeclaration(
        "void __frame(" + (V.Type.empty() ? "uint64_t" : V.Type) + " " +
            V.Name + ")",
        *Library);
    if (!Decl.ok())
      throw std::invalid_argument("Invalid C stack variable name or type");
    bool Observed = false;
    for (const auto &Access : Flow.Accesses) {
      if (Access.Offset == V.Offset && Access.Bytes == V.Bytes)
        Observed = true;
      else if (Access.Offset < V.Offset + V.Bytes &&
               V.Offset < Access.Offset + Access.Bytes)
        throw std::invalid_argument("Overlapping stack accesses must retain "
                                    "their shared backing storage");
    }
    if (!Observed)
      throw std::invalid_argument("Name an observed stack access of the same "
                                  "width; repair unknown SP first");
    for (const auto &[Begin, End] : Ranges)
      if (Begin < V.Offset + V.Bytes && V.Offset < End)
        throw std::invalid_argument("Manual stack variables overlap");
    Ranges.emplace_back(V.Offset, V.Offset + V.Bytes);
    if (!V.Type.empty()) {
      auto Type = parseSourceVariableType(S, V.Type, V.Bytes, Error);
      if (!Type || Type->Kind == NdTypeKind::Float)
        throw std::invalid_argument(
            Error.empty()
                ? "Stack bindings require an exact-width integer or pointer"
                : Error);
    }
  }
}

void neverd::sdk::validateSourceStackBindings(const SourceStackFrame &Policy,
                                              const CSourceMap &Map, va_t Entry,
                                              llvm::StringRef Route) {
  for (const auto &Variable : Policy.Variables) {
    bool Found = false;
    for (const auto &[Key, Actual] : Map.Variables) {
      if (Actual.Function != Entry)
        continue;
      if (Actual.Name == Variable.Name && Actual.StackOffset != Variable.Offset)
        throw std::invalid_argument(
            "The stack name conflicts with another source variable");
      Found |= Actual.StackOffset == Variable.Offset &&
               Actual.Name == Variable.Name && Actual.Bytes == Variable.Bytes &&
               !Actual.Ambiguous &&
               SourceVariableTypes::compatible(Actual.Type, Actual.Bytes);
    }
    if (!Found)
      throw std::invalid_argument(
          "The slot has no independent named storage in " + Route.str() +
          "; retain its backing storage");
  }
}

const char *neverd_stack_frame_json(neverd_session_t Sess, neverd_va_t Entry,
                                    const char *OptionsJSON) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    llvm::StringRef Text = OptionsJSON ? OptionsJSON : "{}";
    if (Text.size() > kMaxStackFrameOptionsBytes)
      throw std::invalid_argument("Stack policy exceeds 128 KiB");
    auto Parsed = llvm::json::parse(Text);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      throw std::invalid_argument("Invalid stack policy JSON");
    }
    const auto *O = Parsed->getAsObject();
    if (!O)
      throw std::invalid_argument("Stack policy must be an object");
    const bool Prepared = neverd_prepare_function_view(Sess, Entry, "low") != 0;
    if (!Prepared && !S->findLowFunc(Entry))
      return nullptr;
    S->clearError();
    const auto *F = S->findLowFunc(Entry);
    if (!F)
      throw std::invalid_argument("Original LowIR is unavailable");
    const auto Policy = readSourceStackFrame(*O);
    if (Policy != SourceStackFrame{})
      validateSourceStackFrame(*S, *F, Policy);
    const auto Flow = analyzeSourceStackFrame(*F, S->Img.Arch, Policy);
    if (!Flow.Error.empty())
      throw std::invalid_argument(Flow.Error);
    llvm::json::Array Instructions, Accesses;
    for (const auto &[PC, I] : Flow.Instructions) {
      llvm::json::Object Row{{"address", vaHex(PC)},
                             {"correctable", I.Correctable}};
      Row["before"] =
          I.Before ? llvm::json::Value(*I.Before) : llvm::json::Value(nullptr);
      Row["after"] =
          I.After ? llvm::json::Value(*I.After) : llvm::json::Value(nullptr);
      Row["frame_before"] = I.FrameBefore ? llvm::json::Value(*I.FrameBefore)
                                          : llvm::json::Value(nullptr);
      Instructions.push_back(std::move(Row));
    }
    for (const auto &A : Flow.Accesses)
      Accesses.push_back(llvm::json::Object{{"address", vaHex(A.Instruction)},
                                            {"sequence", A.Sequence},
                                            {"offset", A.Offset},
                                            {"bytes", A.Bytes},
                                            {"write", A.Write}});
    const auto Snapshot = sourceStackSnapshot(*F);
    llvm::json::Object SavedPolicy{
        {"snapshot", Snapshot},
        {"input_sha256", inputSha256(*S)},
        {"points", O->getArray("points")
                       ? llvm::json::Array(*O->getArray("points"))
                       : llvm::json::Array()},
        {"variables", O->getArray("variables")
                          ? llvm::json::Array(*O->getArray("variables"))
                          : llvm::json::Array()}};
    llvm::json::Value Result =
        llvm::json::Object{{"snapshot", Snapshot},
                           {"policy", std::move(SavedPolicy)},
                           {"instructions", std::move(Instructions)},
                           {"accesses", std::move(Accesses)},
                           {"valid", true}};
    std::string Out;
    llvm::raw_string_ostream OS(Out);
    OS << Result;
    return dupStr(Out);
  } catch (const std::exception &E) {
    S->setError(E.what());
    return nullptr;
  }
}
