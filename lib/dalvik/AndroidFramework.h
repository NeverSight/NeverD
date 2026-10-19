//===- AndroidFramework.h - Workload-owned framework identities --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/Library.h"

namespace neverd::mobile::dalvik {
class AndroidFramework {
public:
  explicit AndroidFramework(std::optional<AndroidApplicationSnapshot> input);
  ExecutionResult execute(Heap &heap, const Program &program,
                          const LibraryMethod &method, uint64_t receiver,
                          std::span<const uint64_t> arguments,
                          const AdmitInstruction &admit);
  ExecutionResult field(Heap &heap, const Field &field, uint64_t receiver,
                        std::optional<uint64_t> write,
                        const AdmitInstruction &admit);

private:
  ExecutionResult instrumentationValue(Heap &heap, uint64_t receiver,
                                       std::optional<uint64_t> write,
                                       const AdmitInstruction &admit);
  std::optional<AndroidApplicationSnapshot> snapshot;
  std::optional<uint64_t> instrumentation;
  uint32_t thread = 0, application = 0, context = 0;
  uint32_t boot_loader = 0, system_loader = 0, app_loader = 0, package = 0;
  uint32_t files_directory = 0, files_path = 0;
};
} // namespace neverd::mobile::dalvik
