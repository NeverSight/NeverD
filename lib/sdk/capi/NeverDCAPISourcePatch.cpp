//===- NeverDCAPISourcePatch.cpp - Source replacement C API ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../SourcePatchCompiler.h"
#include "SessionImpl.h"

#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/backend/codegen/SourcePatchBindings.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"

#include <exception>

using namespace neverd;
using namespace neverd::sdk;

namespace {

void saveReport(Session &S, llvm::json::Object Report) {
  S.LastSourcePatchReport.clear();
  llvm::raw_string_ostream OS(S.LastSourcePatchReport);
  OS << llvm::json::Value(std::move(Report));
}

int patchSource(Session &S, const void *Input, size_t InputSize,
                const neverd_source_patch_options_v1 *Options,
                const char *OutputPath) {
  llvm::json::Object Report{{"schema", "neverd.source-patch.v1"},
                            {"success", false}};
  auto Fail = [&](llvm::StringRef Message) {
    const std::string Detail = Message.str();
    S.setError(Detail);
    Report["error"] = Detail;
    saveReport(S, std::move(Report));
    return 0;
  };
  auto Error = [&](llvm::Error E) {
    return Fail(llvm::toString(std::move(E)));
  };
  if (!S.requireFileBacked())
    return Fail(S.LastError);
  if (!S.Loaded)
    return Fail("no binary loaded");
  if (!Input || InputSize == 0 || InputSize > 64 * 1024 * 1024)
    return Fail("source input must contain between 1 byte and 64 MiB");
  if (!Options || Options->struct_size < sizeof(*Options))
    return Fail("source patch options require the complete v1 structure");
  if (Options->input_kind > NEVERD_SOURCE_PATCH_OBJECT ||
      Options->compiler > NEVERD_SOURCE_PATCH_GCC ||
      Options->language > NEVERD_SOURCE_PATCH_CXX ||
      Options->artifact > NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT ||
      Options->mode > NEVERD_SOURCE_PATCH_DYNCODE || Options->strategy > 1)
    return Fail("unknown source patch option value");
  if (Options->strategy != 0)
    return Fail("source replacement currently requires section installation");
  if (Options->mapping_count > 65536 ||
      (Options->mapping_count && !Options->mappings) ||
      Options->compiler_arg_count > 256 ||
      (Options->compiler_arg_count && !Options->compiler_args) ||
      Options->binding_count > 65536 ||
      (Options->binding_count && !Options->bindings) ||
      Options->timeout_seconds > 3600)
    return Fail("invalid or excessive source patch options");
  for (size_t I = 0; I < Options->compiler_arg_count; ++I)
    if (!Options->compiler_args[I])
      return Fail("compiler argument is null");
  for (size_t I = 0; I < Options->binding_count; ++I)
    if (!Options->bindings[I].symbol ||
        Options->bindings[I].kind > NEVERD_SOURCE_BIND_WRITABLE_DATA)
      return Fail("invalid original reference binding");
  if (Options->mode == NEVERD_SOURCE_PATCH_DYNCODE &&
      Options->input_kind != NEVERD_SOURCE_PATCH_SOURCE)
    return Fail(
        "hosted dyncode requires source and a verified producer sidecar");
  if (S.Img.Arch == Arch::EVM || S.Img.Arch == Arch::SBF)
    return Fail("binary patching is not supported for virtual-machine inputs");
  llvm::StringRef Text(static_cast<const char *>(Input), InputSize);
  if (Options->input_kind != NEVERD_SOURCE_PATCH_OBJECT && Text.contains('\0'))
    return Fail("text source/LLVM IR contains an embedded NUL byte");
  Report["input_kind"] = Options->input_kind;
  Report["compiler"] = Options->compiler;
  Report["artifact"] = Options->artifact;
  Report["mode"] = Options->mode;
  Report["language"] = Options->language;

  std::string Artifact;
  std::optional<DynCodePatchEvidence> DynCode;
  if (Options->input_kind == NEVERD_SOURCE_PATCH_SOURCE) {
    auto Compiled = compileSourcePatch(Text, *Options, S.Img);
    if (!Compiled)
      return Error(Compiled.takeError());
    Artifact = std::move(Compiled->Bytes);
    Report["compiler_path"] = std::move(Compiled->CompilerPath);
    Report["compiler_version"] = std::move(Compiled->CompilerVersion);
    Report["compiler_diagnostics"] = std::move(Compiled->Diagnostics);
    llvm::json::Array Arguments;
    for (std::string &Argument : Compiled->Arguments)
      Arguments.push_back(std::move(Argument));
    Report["compiler_arguments"] = std::move(Arguments);
    Report["source_sha256"] = llvm::toHex(
        llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
            reinterpret_cast<const uint8_t *>(Text.data()), Text.size())),
        true);
    DynCode = std::move(Compiled->DynCode);
    if (DynCode) {
      auto Audit = llvm::json::parse(DynCode->Audit);
      if (!Audit)
        return Error(Audit.takeError());
      Report["dyncode"] = std::move(*Audit);
    }
  } else {
    const bool Object = Options->input_kind == NEVERD_SOURCE_PATCH_OBJECT;
    if ((Options->artifact == NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT) != Object)
      return Fail("input kind and artifact profile disagree");
    Artifact = Text.str();
    Report["compiler"] = nullptr;
    Report["producer"] = "external";
  }

  std::vector<SourcePatchMapping> Requested;
  for (size_t I = 0; I < Options->mapping_count; ++I) {
    const auto &Mapping = Options->mappings[I];
    Requested.push_back(
        {Mapping.symbol ? Mapping.symbol : "", Mapping.original_va});
  }
  std::vector<SourcePatchBinding> Bindings;
  for (size_t I = 0; I < Options->binding_count; ++I) {
    const auto &Binding = Options->bindings[I];
    const auto Kind = Binding.kind == NEVERD_SOURCE_BIND_FUNCTION
                          ? SourcePatchBindingKind::Function
                      : Binding.kind == NEVERD_SOURCE_BIND_READ_ONLY_DATA
                          ? SourcePatchBindingKind::ReadOnlyData
                          : SourcePatchBindingKind::WritableData;
    Bindings.push_back(
        {Binding.symbol, Kind, Binding.original_va, Binding.size});
  }
  const std::filesystem::path Output =
      OutputPath && *OutputPath ? OutputPath : S.FilePath.string() + ".patched";
  std::string SuccessText;
  auto PrepareReceipt = [&](const PatchResult &Result) {
    llvm::json::Object SuccessReport = Report;
    llvm::json::Array Mappings;
    for (const auto &Mapping : Result.PatchedFunctions)
      Mappings.push_back(
          llvm::json::Object{{"symbol", Mapping.SourceFunction},
                             {"owner_symbol", Mapping.OwnerSymbol},
                             {"original_va", Mapping.OriginalVA},
                             {"replacement_va", Mapping.OwnerVA}});
    SuccessReport["installed"] = std::move(Mappings);
    SuccessReport["success"] = true;
    SuccessReport["output_path"] = Result.OutputPath;
    llvm::raw_string_ostream OS(SuccessText);
    OS << llvm::json::Value(std::move(SuccessReport));
    return llvm::Error::success();
  };
  if (Options->artifact == NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT) {
    if (S.InstSubstitution || S.ConstantEncryption || S.OpaquePredicate ||
        S.BogusControlFlow || S.ControlFlowFlattening || S.IndirectBranch ||
        S.IndirectCall || S.MBA || S.IndirectGlobal || S.ValueLaunder ||
        S.ConstantPooling || S.BitMasking)
      return Fail("session LLVM IR transforms do not apply to native objects");
    auto Target = sourcePatchTargetTriple(S.Img);
    if (!Target)
      return Error(Target.takeError());
    Report["target"] = std::move(*Target);
    Report["language"] = Options->language;
    Report["object_bytes"] = Artifact.size();
    Report["object_sha256"] =
        llvm::toHex(llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
                        reinterpret_cast<const uint8_t *>(Artifact.data()),
                        Artifact.size())),
                    true);
    llvm::json::Array RequestedReport, BindingReport;
    for (const auto &Mapping : Requested)
      RequestedReport.push_back(llvm::json::Object{
          {"symbol", Mapping.Symbol}, {"original_va", Mapping.OriginalVA}});
    for (const auto &Binding : Bindings)
      BindingReport.push_back(
          llvm::json::Object{{"symbol", Binding.Symbol},
                             {"kind", static_cast<unsigned>(Binding.Kind)},
                             {"original_va", Binding.OriginalVA},
                             {"size", Binding.Size}});
    Report["requested"] = std::move(RequestedReport);
    Report["bindings"] = std::move(BindingReport);
    auto Installed =
        installObjectPatch(Artifact, S.Img, Requested, Bindings, Output,
                           PrepareReceipt, DynCode ? &*DynCode : nullptr);
    if (!Installed)
      return Error(Installed.takeError());
    S.commitObfuscationCounts({});
    S.LastPatch = std::move(*Installed);
    S.LastSourcePatchReport.swap(SuccessText);
    return 1;
  }

  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Buffer = llvm::MemoryBuffer::getMemBuffer(Artifact, "source-patch");
  auto Module = llvm::parseIR(*Buffer, Diagnostic, Context);
  if (!Module) {
    std::string Detail;
    llvm::raw_string_ostream OS(Detail);
    Diagnostic.print("source-patch", OS);
    return Fail(Detail);
  }
  auto Bound = bindSourcePatch(*Module, S.Img, Requested);
  if (!Bound)
    return Error(Bound.takeError());
  llvm::json::Array RequestedReport;
  for (const auto &Mapping : *Bound)
    RequestedReport.push_back(llvm::json::Object{
        {"symbol", Mapping.Symbol}, {"original_va", Mapping.OriginalVA}});
  Report["requested"] = std::move(RequestedReport);

  auto References = bindSourcePatchReferences(*Module, S.Img, *Bound, Bindings);
  if (!References)
    return Error(References.takeError());
  llvm::json::Array BindingReport;
  for (const auto &Binding : *References)
    BindingReport.push_back(
        llvm::json::Object{{"symbol", Binding.Symbol},
                           {"kind", static_cast<unsigned>(Binding.Kind)},
                           {"original_va", Binding.OriginalVA},
                           {"size", Binding.Size}});
  Report["bindings"] = std::move(BindingReport);
  Report["target"] = Module->getTargetTriple().str();
  Report["data_layout"] = Module->getDataLayoutStr();
  Report["language"] = Options->language;
  Report["source_name"] = Options->source_name ? Options->source_name : "";

  symbolizeImageAbsolutePointers(*Module, S.Img);
  const auto ObfuscationCounts = S.runSessionObfuscation(*Module);
  auto Installed = installSourcePatch(*Module, S.Img, *Bound, Output,
                                      S.TextSectionOverride, PrepareReceipt);
  if (!Installed)
    return Error(Installed.takeError());
  S.commitObfuscationCounts(ObfuscationCounts);
  S.LastPatch = std::move(*Installed);
  S.LastSourcePatchReport.swap(SuccessText);
  return 1;
}

} // namespace

int neverd_patch_source(neverd_session_t Sess, const void *Input,
                        size_t InputSize,
                        const neverd_source_patch_options_v1 *Options,
                        const char *OutputPath) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  S->LastPatch = {};
  S->LastSourcePatchReport.clear();
  try {
    return patchSource(*S, Input, InputSize, Options, OutputPath);
  } catch (const std::exception &Error) {
    S->setError(std::string("source patch failed: ") + Error.what());
  } catch (...) {
    S->setError("source patch failed with an unknown exception");
  }
  return 0;
}

const char *neverd_patch_source_report(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S ? dupStr(S->LastSourcePatchReport) : nullptr;
}
