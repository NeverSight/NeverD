//===- DataFlowJSON.h - Persisted and paged dependency evidence -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared bounded records for saved SSA observations, graph/slice pages and
/// settings. Parsing checks structure; native binding must recheck provenance.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_DATAFLOWJSON_H
#define NEVERD_DIFF_DATAFLOWJSON_H

#include "neverd/diff/Control.h"
#include "neverd/diff/FlowEvidence.h"

#include "llvm/Support/JSON.h"

namespace neverd::diff {
inline constexpr unsigned DataFlowSchemaVersion = 2;
inline constexpr unsigned LLVMDataFlowSchemaVersion = 3;
inline constexpr unsigned LLVMParameterDataFlowSchemaVersion = 4;
inline constexpr unsigned LLVMProgressDataFlowSchemaVersion = 5;
inline constexpr unsigned LLVMMultiBlockProgressDataFlowSchemaVersion = 6;
inline constexpr unsigned LLVMNestedProgressDataFlowSchemaVersion = 7;
inline constexpr unsigned LLVMBranchProgressDataFlowSchemaVersion = 8;
inline constexpr unsigned LLVMModularProgressDataFlowSchemaVersion = 9;
inline constexpr unsigned LLVMParameterBoundProgressDataFlowSchemaVersion = 10;
inline constexpr unsigned LLVMInvariantBoundProgressDataFlowSchemaVersion = 11;
inline unsigned dataFlowSchemaVersion(llvm::StringRef Profile) {
  if (Profile == LLVMInvariantBoundProgressDataFlowProfile)
    return LLVMInvariantBoundProgressDataFlowSchemaVersion;
  if (Profile == LLVMParameterBoundProgressDataFlowProfile)
    return LLVMParameterBoundProgressDataFlowSchemaVersion;
  if (Profile == LLVMModularProgressDataFlowProfile)
    return LLVMModularProgressDataFlowSchemaVersion;
  if (Profile == LLVMBranchProgressDataFlowProfile)
    return LLVMBranchProgressDataFlowSchemaVersion;
  if (Profile == LLVMNestedProgressDataFlowProfile)
    return LLVMNestedProgressDataFlowSchemaVersion;
  if (Profile == LLVMMultiBlockProgressDataFlowProfile)
    return LLVMMultiBlockProgressDataFlowSchemaVersion;
  if (Profile == LLVMProgressDataFlowProfile)
    return LLVMProgressDataFlowSchemaVersion;
  if (Profile == LLVMParameterDataFlowProfile)
    return LLVMParameterDataFlowSchemaVersion;
  return Profile == LLVMDataFlowProfile ? LLVMDataFlowSchemaVersion
                                        : DataFlowSchemaVersion;
}
llvm::json::Object dataFlowOptionsJSON(const DataFlowOptions &Options);
DataFlowOptions readDataFlowOptions(const llvm::json::Value &Value);
llvm::json::Object dataFlowSettingsJSON(const DataFlowSettings &Settings);
DataFlowSettings readDataFlowSettings(const llvm::json::Value &Value);
llvm::json::Object dataFlowEvidenceSummary(const DataFlowEvidence *Evidence);
void writeDataFlowEvidence(llvm::json::OStream &Out,
                           const DataFlowEvidence *Evidence);
std::shared_ptr<const DataFlowEvidence>
readDataFlowEvidence(const llvm::json::Value &Value,
                     const DataFlowSettings &Settings,
                     const ComparisonControl &Control = {});
llvm::json::Object dataFlowEntriesJSON(const DataFlowEvidence &Evidence,
                                       uint64_t Offset, unsigned Limit);
/// Stream one graph without building a second complete node inventory.
void writeDataFlowGraph(llvm::json::OStream &Out, const DataFlowGraph &Graph);
DataFlowGraph readDataFlowGraph(const llvm::json::Value &Value,
                                const DataFlowOptions &Options = {},
                                const ComparisonControl &Control = {});
llvm::json::Object dataFlowNodeJSON(const DataFlowGraph &Graph, uint32_t Node);
llvm::json::Object dataFlowPageJSON(const DataFlowGraph &Graph, bool Blocks,
                                    uint64_t Offset, unsigned Limit);
llvm::json::Object dataFlowSliceJSON(const DataFlowGraph &Graph,
                                     const FlowSlice &Slice, uint64_t Offset,
                                     unsigned Limit);
} // namespace neverd::diff

#endif // NEVERD_DIFF_DATAFLOWJSON_H
