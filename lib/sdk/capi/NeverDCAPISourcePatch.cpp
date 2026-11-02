//===- NeverDCAPISourcePatch.cpp - Source replacement C API ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../SourcePatchCompiler.h"
#include "SessionImpl.h"

#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/backend/codegen/SourcePatchBindings.h"
#include "neverd/backend/codegen/SourcePatchPreparation.h"
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstddef>
#include <cstring>
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
  if (!S.BytePatches.empty())
    return Fail(
        "export and open the byte-patched binary before source replacement");
  if (!S.Loaded)
    return Fail("no binary loaded");
  if (!Input || InputSize == 0 ||
      InputSize > ::neverd::sdk::kMaxSourcePatchInputBytes)
    return Fail("source input must contain between 1 byte and " +
                std::to_string(kMaxSourcePatchInputBytes / (1024 * 1024)) +
                " MiB");
  // Freeze the original ABI boundary before adding optional tail fields. An
  // older caller's allocation must never be read using the current sizeof.
  constexpr size_t V1Minimum =
      offsetof(neverd_source_patch_options_v1, binding_count) + sizeof(size_t);
  if (!Options || Options->struct_size < V1Minimum)
    return Fail("source patch options require the complete v1 structure");
  neverd_source_patch_options_v1 Normalized{};
  std::memcpy(&Normalized, Options, V1Minimum);
  // Read an optional array only when both the pointer and count are present.
  // A truncated tail is never interpreted as a complete field.
  constexpr size_t DependencyEnd =
      offsetof(neverd_source_patch_options_v1, dependency_count) +
      sizeof(size_t);
  if (Options->struct_size >= DependencyEnd) {
    Normalized.dependencies = Options->dependencies;
    Normalized.dependency_count = Options->dependency_count;
  }
  Normalized.struct_size = sizeof(Normalized);
  Options = &Normalized;
  if (Options->input_kind > NEVERD_SOURCE_PATCH_OBJECT ||
      Options->compiler > NEVERD_SOURCE_PATCH_GCC ||
      Options->language > NEVERD_SOURCE_PATCH_CXX ||
      Options->artifact > NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT ||
      Options->mode > NEVERD_SOURCE_PATCH_DYNCODE || Options->strategy > 1)
    return Fail("unknown source patch option value");
  if (Options->strategy != 0)
    return Fail("source replacement currently requires section installation");
  if (Options->mapping_count > ::neverd::sdk::kMaxSourcePatchMappings ||
      (Options->mapping_count && !Options->mappings) ||
      Options->compiler_arg_count >
          ::neverd::sdk::kMaxSourcePatchCompilerArguments ||
      (Options->compiler_arg_count && !Options->compiler_args) ||
      Options->binding_count > ::neverd::sdk::kMaxSourcePatchBindings ||
      (Options->binding_count && !Options->bindings) ||
      Options->dependency_count > ::neverd::sdk::kMaxSourcePatchDependencies ||
      (Options->dependency_count && !Options->dependencies) ||
      Options->timeout_seconds > ::neverd::sdk::kMaxSourcePatchTimeoutSeconds)
    return Fail("invalid or excessive source patch options");
  for (size_t I = 0; I < Options->compiler_arg_count; ++I)
    if (!Options->compiler_args[I])
      return Fail("compiler argument is null");
  for (size_t I = 0; I < Options->binding_count; ++I)
    if (!Options->bindings[I].symbol ||
        Options->bindings[I].kind > NEVERD_SOURCE_BIND_ORIGINAL_FUNCTION)
      return Fail("invalid original reference binding");
  for (size_t I = 0; I < Options->dependency_count; ++I)
    if (!Options->dependencies[I].library || !Options->dependencies[I].symbol ||
        Options->dependencies[I].kind > NEVERD_SOURCE_DEPENDENCY_DATA)
      return Fail("invalid new dependency request");
  if (Options->dependency_count && Options->mode == NEVERD_SOURCE_PATCH_DYNCODE)
    return Fail("hosted dyncode does not admit additional loader dependencies");
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
                      : Binding.kind == NEVERD_SOURCE_BIND_ORIGINAL_FUNCTION
                          ? SourcePatchBindingKind::OriginalFunction
                          : SourcePatchBindingKind::WritableData;
    Bindings.push_back(
        {Binding.symbol, Kind, Binding.original_va, Binding.size});
  }
  const std::filesystem::path Output =
      OutputPath && *OutputPath ? OutputPath : S.FilePath.string() + ".patched";
  std::optional<SourcePatchPreparation> Preparation;
  std::vector<SourcePatchDependency> Dependencies;
  for (size_t I = 0; I < Options->dependency_count; ++I) {
    const auto &D = Options->dependencies[I];
    Dependencies.push_back({D.library, D.symbol,
                            D.kind == NEVERD_SOURCE_DEPENDENCY_FUNCTION
                                ? SourcePatchDependencyKind::Function
                                : SourcePatchDependencyKind::Data});
    if (llvm::any_of(Bindings,
                     [&](const auto &B) { return B.Symbol == D.symbol; }))
      return Fail("a dependency conflicts with an original binding");
  }
  auto Prepare = [&](llvm::ArrayRef<SourcePatchMapping> Mappings,
                     llvm::StringRef LayoutObject = {}) -> llvm::Error {
    const bool Callbacks = llvm::any_of(Bindings, [](const auto &Binding) {
      return Binding.Kind == SourcePatchBindingKind::OriginalFunction;
    });
    if (!Callbacks && Dependencies.empty() && !S.Img.isMachO())
      return llvm::Error::success();
    std::optional<BinaryImage> Reserved;
    if (S.Img.isMachO()) {
      auto Raw = S.Img.Raw;
      uint64_t CommandBytes = 512;
      for (const auto &D : Dependencies)
        CommandBytes += 32 + D.Library.size();
      auto H = reserveMachOHeaderStorage(Raw, S.Img.Arch, CommandBytes);
      if (!H)
        return H.takeError();
      if (!LayoutObject.empty()) {
        auto R = reserveMachONativePatchStorage(
            Raw, S.Img.Arch, LayoutObject,
            llvm::count_if(Bindings,
                           [](const auto &B) {
                             return B.Kind ==
                                    SourcePatchBindingKind::OriginalFunction;
                           }),
            llvm::count_if(Dependencies, [](const auto &D) {
              return D.Kind == SourcePatchDependencyKind::Function;
            }));
        if (!R)
          return R.takeError();
      }
      auto L = Loader::create(BinaryFormat::MachO);
      auto Image = L->loadBuffer(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Raw.data()),
                          Raw.size()),
          "source-layout"));
      if (!Image)
        return Image.takeError();
      Reserved = std::move(*Image);
    }
    std::optional<SourcePatchDependencies> Added;
    if (Callbacks && S.Img.isMachO()) {
      auto P = prepareSourcePatchCallbacks(Reserved ? *Reserved : S.Img,
                                           Mappings, Bindings);
      if (!P)
        return P.takeError();
      Preparation = std::move(*P);
    }
    if (!Dependencies.empty()) {
      auto New = prepareSourcePatchDependencies(Preparation ? Preparation->Image
                                                : Reserved  ? *Reserved
                                                            : S.Img,
                                                Dependencies);
      if (!New)
        return New.takeError();
      Added = std::move(*New);
    }
    if (Callbacks && !Preparation) {
      auto Prepared = prepareSourcePatchCallbacks(Added ? Added->Image : S.Img,
                                                  Mappings, Bindings);
      if (!Prepared)
        return Prepared.takeError();
      Preparation = std::move(*Prepared);
    } else if (!Preparation) {
      Preparation.emplace();
      Preparation->Image = std::move(Added ? Added->Image : *Reserved);
      Preparation->Bindings = Bindings;
    } else if (Added) {
      Preparation->Image = std::move(Added->Image);
    }
    Preparation->OriginalSHA256 = llvm::SHA256::hash(S.Img.Raw);
    if (Added)
      Preparation->Imports = std::move(Added->Imports);
    return llvm::Error::success();
  };
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
    llvm::json::Array Callbacks;
    for (const auto &Callback : Result.OriginalCallbacks)
      Callbacks.push_back(
          llvm::json::Object{{"symbol", Callback.Symbol},
                             {"original_va", Callback.OriginalVA},
                             {"callback_va", Callback.CallbackVA},
                             {"copied_bytes", Callback.CopiedSize}});
    SuccessReport["original_callbacks"] = std::move(Callbacks);
    llvm::json::Array Imports;
    if (Preparation)
      for (const auto &Import : Preparation->Imports)
        Imports.push_back(llvm::json::Object{
            {"library", Import.Request.Library},
            {"symbol", Import.Request.Symbol},
            {"kind", static_cast<unsigned>(Import.Request.Kind)},
            {"slot_va", Import.SlotVA}});
    SuccessReport["added_imports"] = std::move(Imports);
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
    if (auto E = Prepare(Requested, Artifact))
      return Error(std::move(E));
    auto Installed = installObjectPatch(
        Artifact, S.Img, Requested,
        Preparation ? Preparation->Bindings : Bindings, Output, PrepareReceipt,
        DynCode ? &*DynCode : nullptr, Preparation ? &*Preparation : nullptr);
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

  std::string LayoutObject;
  if (S.Img.isMachO()) {
    auto Copy = llvm::CloneModule(*Module);
    Codegen Generator;
    auto Object = Generator.compile(*Copy, S.Img.Arch, S.Img.Format,
                                    Module->getTargetTriple().str());
    if (!Object.Success)
      return Fail("cannot emit native object for layout planning");
    LayoutObject.assign(
        reinterpret_cast<const char *>(Object.ObjectData.data()),
        Object.ObjectData.size());
  }
  if (auto E = Prepare(*Bound, LayoutObject))
    return Error(std::move(E));
  const auto &EffectiveImage = Preparation ? Preparation->Image : S.Img;
  const bool NativeImports =
      EffectiveImage.isMachO() && Preparation && !Preparation->Imports.empty();
  if (Preparation)
    if (auto E = bindSourcePatchDependencies(*Module, Preparation->Imports,
                                             !NativeImports))
      return Error(std::move(E));
  auto References = bindSourcePatchReferences(
      *Module, EffectiveImage, *Bound,
      Preparation ? Preparation->Bindings : Bindings, !NativeImports);
  if (!References)
    return Error(References.takeError());
  if (NativeImports)
    Preparation->Bindings = *References;
  llvm::json::Array BindingReport;
  for (const auto &Binding : *References) {
    const auto Requested = llvm::find_if(
        Bindings, [&](const auto &B) { return B.Symbol == Binding.Symbol; });
    BindingReport.push_back(
        llvm::json::Object{{"symbol", Binding.Symbol},
                           {"kind", static_cast<unsigned>(Requested->Kind)},
                           {"original_va", Requested->OriginalVA},
                           {"resolved_va", Binding.OriginalVA},
                           {"size", Binding.Size}});
  }
  Report["bindings"] = std::move(BindingReport);
  Report["target"] = Module->getTargetTriple().str();
  Report["data_layout"] = Module->getDataLayoutStr();
  Report["language"] = Options->language;
  Report["source_name"] = Options->source_name ? Options->source_name : "";

  if (!NativeImports)
    symbolizeImageAbsolutePointers(*Module, EffectiveImage);
  else
    Report["emission"] = "native-object-with-indirect-symbols";
  const auto ObfuscationCounts = S.runSessionObfuscation(*Module);
  auto Installed =
      installSourcePatch(*Module, S.Img, *Bound, Output, S.TextSectionOverride,
                         PrepareReceipt, Preparation ? &*Preparation : nullptr);
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
