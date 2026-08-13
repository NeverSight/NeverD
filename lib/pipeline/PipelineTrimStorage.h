//===- PipelineTrimStorage.h - Pipeline storage trimming --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private storage-compaction helper shared by LowIR and MedIR construction.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINETRIMSTORAGE_H
#define NEVERD_LIB_PIPELINE_PIPELINETRIMSTORAGE_H

namespace neverd {
namespace {

// Both IRs are built by repeated push_back and then kept alive for the rest of
// the run, so every block carries whatever slack its last geometric growth left
// behind -- across tens of thousands of blocks that slack is a sizeable
// fraction of the pipeline's resident set.  Trimming each function once, on the
// worker that just finished building it, costs one copy of data already in
// cache and is invisible next to decode/SSA.
template <typename Func> inline void trimFuncStorage(Func &F) {
  for (auto &B : F.Blocks) {
    B.Ops.shrink_to_fit();
    B.Succs.shrink_to_fit();
    B.Preds.shrink_to_fit();
    if constexpr (requires { B.Phis; })
      B.Phis.shrink_to_fit();
  }
  F.Blocks.shrink_to_fit();
}

} // namespace
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINETRIMSTORAGE_H
