//===- NeverDCAPIObjC.cpp - Objective-C source projection JSON -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "NativePhaseTrace.h"
#include "ObjCBlockSources.h"
#include "ObjCNativeDependencies.h"
#include "ObjCSourceBindings.h"
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
      {"category_name", jsonSafeText(Method.CategoryName)},
      {"category_address", addressText(Method.CategoryAddress)},
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
          {"class_method", Method.IsClassMethod},
          {"category_name", jsonSafeText(Method.CategoryName)},
          {"category_address", addressText(Method.CategoryAddress)}});
    }
    llvm::json::Array Ivars;
    for (const auto &Ivar : Class.Ivars)
      Ivars.push_back(llvm::json::Object{
          {"name", jsonSafeText(Ivar.Name)},
          {"type_encoding", jsonSafeText(Ivar.TypeEncoding)},
          {"offset", static_cast<int64_t>(Ivar.Offset)},
          {"size", static_cast<int64_t>(Ivar.Size)},
          {"alignment", static_cast<int64_t>(Ivar.Alignment)}});
    Classes.push_back(llvm::json::Object{
        {"name", jsonSafeText(Class.Name)},
        {"address", addressText(Class.Address)},
        {"superclass_address", addressText(Class.SuperclassAddress)},
        {"superclass",
         Class.SuperclassName.empty()
             ? llvm::json::Value(nullptr)
             : llvm::json::Value(jsonSafeText(Class.SuperclassName))},
        {"root_class", Class.RootClass},
        {"instance_start", static_cast<int64_t>(Class.InstanceStart)},
        {"instance_size", static_cast<int64_t>(Class.InstanceSize)},
        {"ivar_status", Class.IvarStatus},
        {"ivars", std::move(Ivars)},
        {"inheritance_status", jsonSafeText(Class.InheritanceStatus)},
        {"methods", std::move(Methods)}});
  }
  llvm::json::Array Categories;
  std::map<va_t, std::vector<const ObjCMethod *>> CategoryMethods;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.CategoryAddress && !Method.CategoryName.empty())
      CategoryMethods[Method.CategoryAddress].push_back(&Method);
  for (const auto &[Address, Methods] : CategoryMethods) {
    llvm::json::Array Members;
    for (const auto *Method : Methods)
      Members.push_back(llvm::json::Object{
          {"selector", jsonSafeText(Method->Selector)},
          {"type_encoding", jsonSafeText(Method->TypeEncoding)},
          {"implementation", addressText(Method->Implementation)},
          {"class_method", Method->IsClassMethod},
          {"category_name", jsonSafeText(Method->CategoryName)},
          {"category_address", addressText(Address)}});
    Categories.push_back(llvm::json::Object{
        {"name", jsonSafeText(Methods.front()->CategoryName)},
        {"class_name", jsonSafeText(Methods.front()->ClassName)},
        {"address", addressText(Address)},
        {"methods", std::move(Members)}});
  }
  llvm::json::Array Limitations;
  Limitations.push_back(
      "Runtime metadata does not recover properties, protocols, or "
      "dynamically registered classes.");
  for (const std::string &Diagnostic : Image.ObjCMetadataDiagnostics)
    Limitations.push_back(jsonSafeText(Diagnostic));
  const char *Status = !Image.ObjCMetadataDiagnostics.empty() ? "partial"
                       : Image.ObjCClasses.empty()            ? "section-absent"
                                                              : "recovered";
  return llvm::json::Object{{"status", Status},
                            {"classes", std::move(Classes)},
                            {"categories", std::move(Categories)},
                            {"limitations", std::move(Limitations)}};
}

} // namespace

const char *neverd_objc_methods_json(neverd_session_t Sess,
                                     size_t MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  NativePhaseTrace Trace(NativePhaseTrace::Phase::ObjCExport);
  try {
    if (!S->Loaded || S->Img.Format != BinaryFormat::MachO) {
      S->setError("Objective-C source export requires a loaded Mach-O image");
      Trace.finish(false);
      return nullptr;
    }
    llvm::LLVMContext Context;
    PipelineOptions Options;
    S->applyAnalysisOptions(Options);
    Options.MaxFunctions = MaxFunctions;
    Options.EmitDumpOutput = false;
    Pipeline Engine;
    auto RunPipeline = [&](unsigned Iteration) {
      NativePhaseTrace PipelineTrace(NativePhaseTrace::Phase::Pipeline,
                                     Iteration);
      auto Result = Engine.run(S->Img, Context, Options, S->Dbg.get());
      PipelineTrace.finish(Result.Success);
      return Result;
    };
    auto Result = RunPipeline(0);
    if (!Result.Success) {
      S->setError(Result.Error.empty() ? "native source pipeline failed"
                                       : Result.Error);
      Trace.finish(false);
      return nullptr;
    }

    std::map<va_t, std::string> NativeDependencies;
    ObjCBlockSourcePlan BlockPlan;
    for (unsigned Depth = 0; Depth < 16; ++Depth) {
      const bool NativeChanged = inferObjCNativeDependencies(
          S->Img, Result, Options, NativeDependencies);
      BlockPlan = discoverObjCBlockSources(S->Img, Result);
      const bool BlocksChanged =
          applyObjCBlockInvokeHints(BlockPlan, Options) != 0;
      if (!NativeChanged && !BlocksChanged)
        break;
      Result = RunPipeline(Depth + 1);
      if (!Result.Success) {
        S->setError(Result.Error.empty()
                        ? "native dependency source pipeline failed"
                        : Result.Error);
        Trace.finish(false);
        return nullptr;
      }
    }

    // Stack expression identities belong to the final pipeline result.
    BlockPlan = discoverObjCBlockSources(S->Img, Result);

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
      Trace.finish(false);
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

    std::map<va_t, ObjCSourceBindingResult> Projections;
    std::map<va_t, ObjCBlockSourceBindingResult> BlockProjections;
    std::map<va_t, std::string> ProjectionReasons;
    std::set<va_t> Closed;
    for (const auto &[Entry, Func] : Functions) {
      if (!Func->SourceTypeHint)
        continue;
      auto BlockBinding =
          bindObjCBlockSourceReferences(*Func, S->Img, BlockPlan, Functions);
      auto Binding = bindObjCSourceReferences(BlockBinding.Function, S->Img);
      Binding.Dependencies.insert(BlockBinding.Dependencies.begin(),
                                  BlockBinding.Dependencies.end());
      std::string Reason = BlockBinding.Limitation.empty()
                               ? Binding.Limitation
                               : BlockBinding.Limitation;
      if (Reason.empty()) {
        const auto Audit = Audits.find(Entry);
        Reason = sourceBodyLimitation(
            Binding.Function, *Func->SourceTypeHint,
            Audit == Audits.end() ? nullptr : Audit->second,
            [&](const HighExpr &Expression) {
              return objcSourceCallBound(Expression, S->Img, Functions) ||
                     objcBlockSourceCallBound(Expression, S->Img, BlockPlan,
                                              Functions);
            });
      }
      if (Reason.empty())
        Closed.insert(Entry);
      ProjectionReasons.emplace(Entry, std::move(Reason));
      Projections.emplace(Entry, std::move(Binding));
      BlockProjections.emplace(Entry, std::move(BlockBinding));
    }
    bool Changed;
    do {
      Changed = false;
      for (const auto &[Entry, Projection] : Projections) {
        if (!Closed.count(Entry))
          continue;
        for (va_t Dependency : Projection.Dependencies)
          if (!Closed.count(Dependency)) {
            Closed.erase(Entry);
            ProjectionReasons[Entry] = "method depends on native source that "
                                       "was not completely recovered";
            Changed = true;
            break;
          }
      }
    } while (Changed);

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
            Projections.at(Method.Implementation).Function, *Method.TypeHint,
            It == Audits.end() ? nullptr : It->second,
            [&](const HighExpr &Expression) {
              return objcSourceCallBound(Expression, S->Img, Functions) ||
                     objcBlockSourceCallBound(Expression, S->Img, BlockPlan,
                                              Functions);
            });
        if (Reason.empty()) {
          if (auto Projection = ProjectionReasons.find(Method.Implementation);
              Projection != ProjectionReasons.end())
            Reason = Projection->second;
          else
            Reason = "method has no complete relocatable source projection";
        }
      }
      if (!Reason.empty()) {
        Row["status"] = "unrecovered";
        Row["reason"] = std::move(Reason);
        Methods.push_back(std::move(Row));
        continue;
      }

      HighFunc Projection = Projections.at(Method.Implementation).Function;
      Projection.Name =
          "neverd_objc_imp_" +
          llvm::utohexstr(Method.Implementation, /*LowerCase=*/true);
      Projection.DebugName.clear();
      Projection.SourceFile.clear();
      std::string Source;
      llvm::raw_string_ostream SourceOS(Source);
      std::vector<HighFunc> Unit{Projection};
      std::set<va_t> Included{Projection.Entry};
      std::vector<va_t> Pending(
          Projections.at(Projection.Entry).Dependencies.begin(),
          Projections.at(Projection.Entry).Dependencies.end());
      while (!Pending.empty()) {
        const va_t Entry = Pending.back();
        Pending.pop_back();
        if (!Included.insert(Entry).second)
          continue;
        const auto &Dependency = Projections.at(Entry);
        Unit.push_back(Dependency.Function);
        Pending.insert(Pending.end(), Dependency.Dependencies.begin(),
                       Dependency.Dependencies.end());
      }
      std::set<va_t> BlockDescriptors;
      for (va_t Entry : Included) {
        const auto &Descriptors = BlockProjections.at(Entry).Descriptors;
        BlockDescriptors.insert(Descriptors.begin(), Descriptors.end());
      }
      std::set<std::string> SharedBlockFunctions;
      const std::string BlockHelpers = renderObjCBlockSourceHelpers(
          BlockPlan, BlockDescriptors, SharedBlockFunctions);
      const bool Emitted = Emitter.emit(Unit, SourceOS, COptions);
      SourceOS << BlockHelpers;
      if (!Emitted) {
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
        std::set<std::string> LayoutClasses;
        for (va_t Entry : Included) {
          const auto &Classes = Projections.at(Entry).InstanceLayoutClasses;
          LayoutClasses.insert(Classes.begin(), Classes.end());
        }
        if (!Method.IsClassMethod && Method.ClassAddress)
          LayoutClasses.insert(Method.ClassName);
        llvm::json::Array Layouts;
        for (const auto &ClassName : LayoutClasses)
          Layouts.push_back(ClassName);
        Row["instance_layout_classes"] = std::move(Layouts);
        Row["return_type"] = typeToC(Projection.ReturnType);
        Row["parameters"] = std::move(Parameters);
        Row["function_name"] = Projection.Name;
        Row["source"] = jsonSafeText(Source);
        llvm::json::Array SharedFunctions;
        for (const auto &Name : SharedBlockFunctions)
          SharedFunctions.push_back(Name);
        Row["shared_block_functions"] = std::move(SharedFunctions);
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
    Limitations.push_back(
        "Unresolved native dependencies and exception-dependent method bodies "
        "remain individually unrecovered.");
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
    Trace.finish(Output != nullptr);
    return Output;
  } catch (const std::exception &Error) {
    S->setError(std::string("Objective-C source export failed: ") +
                Error.what());
  } catch (...) {
    S->setError("Objective-C source export failed with an unknown exception");
  }
  Trace.finish(false);
  return nullptr;
}
