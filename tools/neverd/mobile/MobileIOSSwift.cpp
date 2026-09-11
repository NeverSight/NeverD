#include "MobileIOSInternal.h"

#include <algorithm>

namespace neverd::mobile::ios {
namespace {
using Identity = std::pair<std::string, std::string>;
Identity identity(const Object &o) {
  auto entry = requiredString(o, "entry"),
       symbol = requiredString(o, "mangled_symbol");
  if (!hexAddress(entry) || symbol.empty())
    throw Error("invalid Swift method identity");
  return {entry, symbol};
}
bool sourceText(const std::string &s) {
  return !llvm::StringRef(s).trim().empty() && s.find('\0') == s.npos;
}
bool emptyInitializer(const Object &s, const Array &types) {
  const auto *parameters = s.getArray("parameters");
  const auto *labels = s.getArray("labels");
  const auto *returned = s.getObject("return_type");
  if (str(s, "declaration_kind") != "initializer" ||
      str(s, "node_kind") != "Allocator" || str(s, "name") != "init" ||
      str(s, "context_kind") != "struct" ||
      !flag(s, "requires_storage_abi_proof") ||
      s.getBoolean("is_static") != false ||
      s.getBoolean("is_mutating") != false ||
      s.getBoolean("is_mutating_known") != true || !parameters ||
      !parameters->empty() || !labels || !labels->empty() || !returned ||
      str(*returned, "kind") != "nominal" ||
      str(*returned, "context_kind") != "struct" ||
      str(*returned, "module") != str(s, "module") ||
      str(*returned, "name") != str(s, "context_name"))
    return false;
  auto symbol = str(s, "mangled_symbol");
  if (symbol.starts_with("_"))
    symbol.erase(0, 1);
  const auto module = str(s, "module"), name = str(s, "context_name");
  if (symbol != "$s" + std::to_string(module.size()) + module +
                    std::to_string(name.size()) + name + "VACycfC")
    return false;
  size_t matches = 0;
  for (const auto &value : types) {
    const auto &type = object(value, "Swift type");
    if (str(type, "module") != module || str(type, "name") != name ||
        str(type, "kind") != "struct")
      continue;
    const auto *fields = type.getArray("fields");
    if (++matches != 1 || str(type, "status") != "recovered" ||
        !str(type, "reason").empty() || type.getInteger("size") != 0 ||
        type.getInteger("alignment") != 1 || !fields || !fields->empty())
      return false;
  }
  return matches == 1;
}
std::string callableStatus(size_t count, size_t recovered) {
  return !count               ? "no-methods"
         : !recovered         ? "unrecovered"
         : count == recovered ? "recovered"
                              : "partial";
}
void exactCount(const Object &o, llvm::StringRef key, uint64_t n) {
  auto v = o.getInteger(key);
  if (!v || *v < 0 || uint64_t(*v) != n)
    throw Error("Swift report disagrees with count: " + key.str());
}
Object unclassified(const Array &symbols, const std::string &reason) {
  Array rows;
  for (const auto &v : symbols) {
    const auto &s = object(v, "Swift symbol");
    rows.push_back(Object{{"entry", requiredString(s, "address")},
                          {"mangled_symbol", requiredString(s, "name")},
                          {"classification", "unknown"},
                          {"status", "unsupported"},
                          {"reason", reason}});
  }
  return Object{{"schema_version", 1},
                {"methods", Array{}},
                {"symbols", std::move(rows)},
                {"method_count", 0},
                {"symbol_count", symbols.size()},
                {"unclassified_symbol_count", symbols.size()},
                {"supported_signature_count", 0},
                {"unsupported_signature_count", 0},
                {"demangler", swiftDemanglerInfo()},
                {"limitations", reason.empty() ? Array{} : Array{reason}}};
}
} // namespace
Object swiftCoverage(const Object &inventory, const Object *batch,
                     std::string_view workflow_status) {
  const auto &supplied = array(inventory, "methods");
  Array methods, units, types, backend_limitations;
  uint64_t recovered = 0, compiler_recovered = 0;
  if (batch) {
    if (number(*batch, "schema_version") != 1 ||
        str(*batch, "status") != "success")
      throw Error("Swift source report has unsupported schema");
    auto source = requiredString(*batch, "source");
    const auto &rows = array(*batch, "methods");
    backend_limitations = array(*batch, "limitations");
    for (const auto &v : backend_limitations)
      if (!v.getAsString())
        throw Error("invalid Swift backend limitation");
    types = array(*batch, "types");
    for (const auto &t : types)
      object(t, "Swift type");
    std::map<Identity, size_t> inputs, outputs;
    std::map<Identity, const Object *> signatures, results, compiler_rows;
    std::set<Identity> recovered_ids;
    for (const auto &v : supplied) {
      const auto &s = object(v, "signature");
      auto id = identity(s);
      ++inputs[id];
      signatures[id] = &s;
    }
    for (const auto &v : rows) {
      const auto &r = object(v, "Swift method");
      auto id = identity(r);
      ++outputs[id];
      results[id] = &r;
    }
    if (inputs != outputs)
      throw Error("Swift backend report disagrees with method inventory");
    static const std::map<std::string, std::string> projections = {
        {"allocating_initializer", "allocating_initializer"},
        {"destructor", "trivial_destructor"},
        {"deallocator", "deallocating_destructor"},
        {"type_metadata_accessor", "type_metadata_accessor"},
        {"modify_accessor", "modify_accessor"},
        {"modify_resume", "modify_resume"}};
    for (const auto &v : rows) {
      const auto &r = object(v, "Swift method");
      auto id = identity(r);
      auto status = str(r, "status");
      const auto &s = *signatures.at(id);
      if (status == "recovered") {
        if (!sourceText(requiredString(r, "source")) ||
            !recovered_ids.insert(id).second)
          throw Error(
              "Swift recovery has empty source or duplicate method identity");
        ++recovered;
        const bool empty_init =
            str(r, "compiler_projection_kind") == "empty_value_initializer";
        if (flag(s, "requires_runtime_source_proof") || empty_init) {
          auto kind = projections.find(str(s, "runtime_source_kind"));
          auto *e = r.getArray("compiler_projection_evidence");
          if ((empty_init
                   ? (!emptyInitializer(s, types) ||
                      flag(s, "requires_runtime_source_proof") ||
                      str(r, "declaration_kind") != "initializer" ||
                      str(r, "source") !=
                          "struct `" + str(s, "context_name") + "` {\n}\n")
                   : (str(s, "declaration_kind") != "runtime" ||
                      kind == projections.end() ||
                      str(r, "compiler_projection_kind") != kind->second)) ||
              str(r, "source_representation") !=
                  "compiler-generated-from-type" ||
              !e || e->empty() || e->size() > 128)
            throw Error("Swift compiler projection lacks exact role or native "
                        "evidence");
          for (const auto &v : *e) {
            auto text = v.getAsString();
            if (!text || text->size() > 8192 || !sourceText(text->str()))
              throw Error("invalid Swift compiler projection evidence");
          }
          compiler_rows[id] = &r;
          ++compiler_recovered;
        } else if (str(r, "source_representation", "native-method-body") !=
                       "native-method-body" ||
                   r.get("compiler_projection_kind") ||
                   r.get("compiler_projection_evidence"))
          throw Error("Swift ordinary method cannot be relabelled as compiler "
                      "projection");
      } else if (status != "unrecovered" || str(r, "reason").empty())
        throw Error("Swift backend omitted unrecovered method reason");
    }
    exactCount(*batch, "method_count", rows.size());
    exactCount(*batch, "recovered_method_count", recovered);
    exactCount(*batch, "unrecovered_method_count", rows.size() - recovered);
    if (compiler_recovered || batch->get("source_body_method_count") ||
        batch->get("compiler_projection_method_count")) {
      exactCount(*batch, "source_body_method_count",
                 recovered - compiler_recovered);
      exactCount(*batch, "compiler_projection_method_count",
                 compiler_recovered);
    }
    std::string combined;
    std::map<Identity, size_t> unit_ids;
    for (const auto &v : array(*batch, "source_units")) {
      const auto &unit = object(v, "Swift source unit");
      auto kind = requiredString(unit, "kind"),
           module = requiredString(unit, "module"),
           name = requiredString(unit, "name"),
           text = requiredString(unit, "source");
      if ((kind != "function" && kind != "type") || module.empty() ||
          name.empty() || !sourceText(text))
        throw Error("invalid Swift source unit");
      const auto &entries = array(unit, "method_entries"),
                 &identities = array(unit, "method_identities");
      if (entries.size() != identities.size())
        throw Error("Swift unit entry projection disagrees");
      for (size_t i = 0; i < identities.size(); ++i) {
        auto id = identity(object(identities[i], "unit identity"));
        auto entry = entries[i].getAsString();
        if (!entry || *entry != id.first)
          throw Error("Swift unit entry projection disagrees");
        if (!recovered_ids.count(id) || ++unit_ids[id] != 1)
          throw Error(
              "Swift source units do not uniquely cover recovered identities");
        if (compiler_rows.count(id)) {
          const auto &s = *signatures.at(id), &r = *compiler_rows.at(id);
          if (kind != "type" || module != str(s, "module") ||
              name != str(s, "context_name") || text != str(r, "source"))
            throw Error("Swift compiler projection does not belong to actual "
                        "type source unit");
          if (str(r, "compiler_projection_kind") == "empty_value_initializer") {
            size_t accessors = 0;
            for (const auto &other : identities) {
              const auto other_id = identity(object(other, "unit identity"));
              auto found = compiler_rows.find(other_id);
              if (found == compiler_rows.end())
                continue;
              auto symbol = other_id.second;
              if (symbol.starts_with("_"))
                symbol.erase(0, 1);
              const auto expected = "$s" + std::to_string(module.size()) +
                                    module + std::to_string(name.size()) +
                                    name + "VMa";
              if (symbol == expected &&
                  str(*found->second, "compiler_projection_kind") ==
                      "type_metadata_accessor" &&
                  str(*found->second, "source") == text)
                ++accessors;
            }
            if (identities.size() != 2 || accessors != 1)
              throw Error("Swift empty initializer lacks its exact nominal "
                          "accessor source unit");
          }
        }
      }
      combined += text + "\n";
      units.push_back(Object{{"kind", kind},
                             {"module", module},
                             {"name", name},
                             {"method_entries", Array(entries)},
                             {"method_identities", Array(identities)}});
    }
    if (unit_ids.size() != recovered_ids.size() || combined != source ||
        str(*batch, "coverage_status") !=
            callableStatus(rows.size(), recovered))
      throw Error("Swift report disagrees with emitted source or coverage");
    for (const auto &v : supplied) {
      const auto &s = object(v, "signature");
      auto id = identity(s);
      const auto &r = *results.at(id);
      Object method = s;
      method.erase("reason");
      method["signature_status"] = requiredString(s, "status");
      method["status"] = str(r, "status");
      if (str(r, "status") == "unrecovered")
        method["reason"] = str(r, "reason");
      else {
        method["source_representation"] =
            str(r, "source_representation", "native-method-body");
        if (compiler_rows.count(id)) {
          method["compiler_projection_kind"] =
              str(r, "compiler_projection_kind");
          method["compiler_projection_evidence"] =
              *r.get("compiler_projection_evidence");
        }
      }
      methods.push_back(std::move(method));
    }
  } else if (!supplied.empty())
    throw Error("Swift callable inventory has no native source report");
  auto unknown = number(inventory, "unclassified_symbol_count");
  std::string coverage = callableStatus(methods.size(), recovered),
              status = workflow_status.empty() ? coverage
                                               : std::string(workflow_status);
  if (unknown && workflow_status.empty())
    status = recovered ? "partial" : "unclassified";
  Array non_methods;
  uint64_t metadata_count = 0, type_count = 0;
  for (const auto &v : array(inventory, "symbols")) {
    auto row = object(v, "Swift symbol");
    if (str(row, "classification") == "metadata") {
      ++metadata_count;
      row["status"] = "not-callable";
      row.erase("reason");
    } else
      row["status"] = "unclassified";
    non_methods.push_back(std::move(row));
  }
  for (const auto &v : units)
    type_count += str(object(v, "unit"), "kind") == "type";
  Array limitations{
      "Swift source coverage counts classified callable symbols; stripped or "
      "unclassified symbols may conceal additional methods.",
      "Recovered Swift source is a projection of supported native bodies and "
      "signatures, not a semantic-equivalence certificate.",
      "Compiler-generated entries are explicitly labelled projections from "
      "recovered type source and native effect evidence, not independent "
      "ordinary source method bodies."};
  appendUnique(limitations, array(inventory, "limitations"));
  appendUnique(limitations, backend_limitations);
  return Object{{"schema_version", 1},
                {"status", status},
                {"coverage_status", coverage},
                {"method_count", methods.size()},
                {"recovered_method_count", recovered},
                {"unrecovered_method_count", methods.size() - recovered},
                {"source_body_method_count", recovered - compiler_recovered},
                {"compiler_projection_method_count", compiler_recovered},
                {"symbol_count", number(inventory, "symbol_count")},
                {"metadata_symbol_count", metadata_count},
                {"unclassified_symbol_count", unknown},
                {"supported_signature_count",
                 number(inventory, "supported_signature_count")},
                {"unsupported_signature_count",
                 number(inventory, "unsupported_signature_count")},
                {"methods", std::move(methods)},
                {"non_method_symbols", std::move(non_methods)},
                {"source_units", std::move(units)},
                {"source_type_count", type_count},
                {"type_metadata_count", types.size()},
                {"types", std::move(types)},
                {"limitations", std::move(limitations)}};
}
SwiftResult swiftSources(const Options &options, const fs::path &staging,
                         const fs::path &binary, const Array &symbols,
                         unsigned ptr, Budget &budget) {
  if (symbols.size() > budget.limits.max_files)
    throw Error("Swift symbol inventory exceeds file limit");
  Object outputs{{"swift_signatures", "metadata/swift-signatures.json"},
                 {"swift_method_coverage", "metadata/swift-methods.json"}},
      inventory;
  std::string workflow;
  if (symbols.empty()) {
    inventory = unclassified(symbols, "");
    workflow = "no-symbols";
  } else if (ptr != 8) {
    inventory = unclassified(
        symbols, "Swift source projection requires a 64-bit Mach-O slice.");
    workflow = "unsupported-architecture";
  } else {
    inventory = swiftSignatures(symbols, ptr, budget);
  }
  auto signature_path = staging / "metadata/swift-signatures.json";
  auto signature_text = jsonText(Value(Object(inventory)));
  if (signature_text.size() >
      std::min<uint64_t>(budget.limits.max_bytes, 32 * 1024 * 1024))
    throw Error("Swift signature inventory exceeds byte limit");
  budget.output(signature_text.size());
  writeFile(signature_path, signature_text);
  std::optional<Value> batch_value;
  const Object *batch = nullptr;
  auto batch_path = staging / "artifacts/swift-recovery.json";
  if (!array(inventory, "methods").empty()) {
    budget.check();
    std::vector<std::string> argv{options.executable,
                                  "export",
                                  pathText(binary),
                                  "--format=swift-methods",
                                  "--source-signatures=" +
                                      pathText(signature_path),
                                  "-o",
                                  pathText(batch_path)};
    if (options.max_functions)
      argv.push_back("--max-func=" + std::to_string(options.max_functions));
    runTool(argv, staging / "logs/swift-native.log", toolTimeout(budget),
            staging, budget.limits);
    batch_value = parseJSON(readFile(batch_path, budget.limits.max_bytes),
                            "Swift backend source report");
    batch = &object(*batch_value, "Swift backend source report");
    outputs["swift_native_log"] = "logs/swift-native.log";
  }
  auto coverage = swiftCoverage(inventory, batch, workflow);
  if (batch) {
    auto source = requiredString(*batch, "source");
    if (!source.empty()) {
      budget.output(source.size());
      writeFile(staging / "sources/swift.swift", source);
      outputs["swift_source"] = "sources/swift.swift";
    }
    fs::remove(batch_path);
  }
  auto text = jsonText(Value(Object(coverage)));
  budget.output(text.size());
  writeFile(staging / "metadata/swift-methods.json", text);
  return {std::move(coverage), std::move(outputs)};
}
} // namespace neverd::mobile::ios
