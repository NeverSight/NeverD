//===- AndroidObservation.h - Bounded stopped execution reads ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared observation admission and read-only capture at an execution watch.
#ifndef NEVERD_EMULATION_ANDROIDOBSERVATION_H
#define NEVERD_EMULATION_ANDROIDOBSERVATION_H

#include "neverd/emulation/AndroidNative.h"
#include "neverd/emulation/CPU.h"

namespace neverd::emulation::android_model {
llvm::Error
validateExecutionObservation(const NativeExecutionObservationOptions &Options);
llvm::Expected<uint64_t>
executionObservationBytes(const NativeExecutionObservationOptions &Options,
                          uint64_t Available);
llvm::Expected<uint64_t>
finalMemorySnapshotBytes(const AndroidNativeOptions &Options,
                         uint64_t Available);
void captureStoppedMemory(ExecutionBackend &CPU,
                          const AndroidNativeOptions &Options,
                          bool IncludeFixed,
                          std::vector<NativeMemorySnapshot> &Out);
llvm::Expected<NativeExecutionObservation> captureExecutionObservation(
    ExecutionBackend &CPU, const NativeExecutionObservationOptions &Options,
    uint64_t PC, uint64_t ThreadID, uint64_t Instructions);
} // namespace neverd::emulation::android_model
#endif
