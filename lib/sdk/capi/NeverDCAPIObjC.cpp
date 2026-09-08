//===- NeverDCAPIObjC.cpp - Objective-C source projection JSON -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "ObjCSourceProjection.h"
#include "SessionImpl.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/loader/ObjC/ObjCMethods.h"

#include "llvm/ADT/StringExtras.h"

#include <exception>
#include <map>

using namespace neverd;
using namespace neverd::sdk;

namespace {

std::string addressText(va_t Address) {
  return "0x" + llvm::utohexstr(Address, /*LowerCase=*/true);
}

llvm::json::Object methodIdentity(const ObjCMethod &Method) {
  return llvm::json::Object{
      {"class_name", jsonSafeText(Method.ClassName)},
      {"selector", jsonSafeText(Method.Selector)},
      {"class_method", Method.IsClassMethod},
      {"implementation", addressText(Method.Implementation)},
      {"type_encoding", jsonSafeText(Method.TypeEncoding)}};
}

llvm::json::Object metadataJSON(const BinaryImage &Image) {
  llvm::json::Array Classes;
  for (const ObjCClass &Class : Image.ObjCClasses) {
    llvm::json::Array Methods;
    for (const ObjCMethod &Method : Image.ObjCMethods) {
      if (Method.ClassAddress != Class.Address)
        continue;
      Methods.push_back(llvm::json::Object{
          {"selector", jsonSafeText(Method.Selector)},
          {"type_encoding", jsonSafeText(Method.TypeEncoding)},
          {"implementation", addressText(Method.Implementation)},
          {"class_method", Method.IsClassMethod}});
    }
    Classes.push_back(llvm::json::Object{
        {"name", jsonSafeText(Class.Name)},
        {"address", addressText(Class.Address)},
        {"superclass_address", addressText(Class.SuperclassAddress)},
        {"superclass",
         Class.SuperclassName.empty()
             ? llvm::json::Value(nullptr)
             : llvm::json::Value(jsonSafeText(Class.SuperclassName))},
        {"root_class", Class.RootClass},
        {"inheritance_status", jsonSafeText(Class.InheritanceStatus)},
        {"methods", std::move(Methods)}});
  }
  llvm::json::Array Limitations;
  Limitations.push_back(
      "Runtime metadata does not recover properties, protocols, categories, or "
      "dynamically registered classes.");
  for (const std::string &Diagnostic : Image.ObjCMetadataDiagnostics)
    Limitations.push_back(jsonSafeText(Diagnostic));
  const char *Status = !Image.ObjCMetadataDiagnostics.empty() ? "partial"
                       : Image.ObjCClasses.empty()            ? "section-absent"
                                                              : "recovered";
  return llvm::json::Object{{"status", Status},
                            {"classes", std::move(Classes)},
                            {"limitations", std::move(Limitations)}};
}

} // namespace

const char *neverd_objc_methods_json(neverd_session_t Sess,
                                     size_t MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (!S->Loaded || S->Img.Format != BinaryFormat::MachO) {
      S->setError("Objective-C source export requires a loaded Mach-O image");
      return nullptr;
    }
    llvm::LLVMContext Context;
    PipelineOptions Options;
    S->applyAnalysisOptions(Options);
    Options.MaxFunctions = MaxFunctions;
    Options.EmitDumpOutput = false;
    Pipeline Engine;
    auto Result = Engine.run(S->Img, Context, Options, S->Dbg.get());
    if (!Result.Success) {
      S->setError(Result.Error.empty() ? "native source pipeline failed"
                                       : Result.Error);
      return nullptr;
    }

    CEmitterOptions COptions;
    COptions.TheArch = S->Img.Arch;
    COptions.Format = S->Img.Format;
    COptions.EmitComments = false;
    COptions.UseDebugNames = false;
    HighCEmitter Emitter;
    std::string NativeSource;
    llvm::raw_string_ostream NativeOS(NativeSource);
    if (!Emitter.emit(Result.HighFuncs, NativeOS, COptions)) {
      S->setError("native C source projection failed");
      return nullptr;
    }
    std::map<va_t, const HighFunc *> Functions;
    size_t NativeFunctionCount = 0;
    for (const HighFunc &Func : Result.HighFuncs) {
      if (Func.Name.empty())
        continue;
      Functions.emplace(Func.Entry, &Func);
      ++NativeFunctionCount;
    }
    std::map<va_t, const PipelineFunctionAudit *> Audits;
    for (const PipelineFunctionAudit &Audit : Result.FunctionAudits)
      Audits.emplace(Audit.Entry, &Audit);

    llvm::json::Array Methods;
    size_t Recovered = 0;
    for (const ObjCMethod &Method : S->Img.ObjCMethods) {
      auto Row = methodIdentity(Method);
      Row["metadata_address"] = addressText(Method.MetadataAddress);
      llvm::json::Array Diagnostics;
      for (const std::string &Diagnostic : Method.Diagnostics)
        Diagnostics.push_back(jsonSafeText(Diagnostic));
      Row["diagnostics"] = std::move(Diagnostics);
      std::string Reason;
      const HighFunc *Func = nullptr;
      if (auto It = Functions.find(Method.Implementation);
          It != Functions.end())
        Func = It->second;
      if (Method.Status != "supported" || !Method.TypeHint)
        Reason = "runtime method signature is not supported: " + Method.Status;
      else if (!Func || !Func->SourceTypeHint)
        Reason = "method has no complete typed source body (possibly limited "
                 "by max-func)";
      else {
        auto It = Audits.find(Method.Implementation);
        Reason = objcSourceBodyLimitation(
            *Func, *Method.TypeHint, It == Audits.end() ? nullptr : It->second);
      }
      if (!Reason.empty()) {
        Row["status"] = "unrecovered";
        Row["reason"] = std::move(Reason);
        Methods.push_back(std::move(Row));
        continue;
      }

      HighFunc Projection = *Func;
      Projection.Name =
          "neverd_objc_imp_" +
          llvm::utohexstr(Method.Implementation, /*LowerCase=*/true);
      Projection.DebugName.clear();
      Projection.SourceFile.clear();
      std::string Source;
      llvm::raw_string_ostream SourceOS(Source);
      if (!Emitter.emit({Projection}, SourceOS, COptions)) {
        Row["status"] = "unrecovered";
        Row["reason"] = "method C source projection failed";
      } else if (auto Limitation = objcSourceTextLimitation(Source);
                 !Limitation.empty()) {
        Row["status"] = "unrecovered";
        Row["reason"] = std::move(Limitation);
      } else {
        llvm::json::Array Parameters;
        for (const HighParam &Parameter : Projection.Params)
          Parameters.push_back(llvm::json::Object{
              {"name", Parameter.Name}, {"type", typeToC(Parameter.Type)}});
        Row["status"] = "recovered";
        Row["return_type"] = typeToC(Projection.ReturnType);
        Row["parameters"] = std::move(Parameters);
        Row["function_name"] = Projection.Name;
        Row["source"] = jsonSafeText(Source);
        ++Recovered;
      }
      Methods.push_back(std::move(Row));
    }
    llvm::json::Array Limitations;
    Limitations.push_back(
        "Recovered bodies are source projections, not a proof of semantic "
        "equivalence or original source text.");
    Limitations.push_back("Only fixed scalar/pointer Objective-C signatures "
                          "with supported ABI bindings are projected; variadic "
                          "tails are not described by runtime encodings.");
    Limitations.push_back("Swift method source, native/dynamic call bindings, "
                          "and exception-dependent method bodies are not "
                          "reconstructed by this exporter.");
    llvm::json::Object Report{
        {"schema_version", 1},
        {"status", "success"},
        {"pointer_size", S->Img.Bits == Bitness::Bits64 ? 8 : 4},
        {"native_source", jsonSafeText(NativeSource)},
        {"native_function_count", static_cast<int64_t>(NativeFunctionCount)},
        {"objc_metadata", metadataJSON(S->Img)},
        {"method_count", static_cast<int64_t>(S->Img.ObjCMethods.size())},
        {"recovered_method_count", static_cast<int64_t>(Recovered)},
        {"methods", std::move(Methods)},
        {"limitations", std::move(Limitations)}};
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    OS << llvm::json::Value(std::move(Report));
    char *Output = dupStr(Text);
    if (!Output)
      S->setError("unable to allocate Objective-C source report");
    return Output;
  } catch (const std::exception &Error) {
    S->setError(std::string("Objective-C source export failed: ") +
                Error.what());
  } catch (...) {
    S->setError("Objective-C source export failed with an unknown exception");
  }
  return nullptr;
}
