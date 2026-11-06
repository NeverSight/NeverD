//===- X86KVMBacking.h - Prepare native oracle backing --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Populate second-level mappings without executing or changing guest state.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86KVMBACKING_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86KVMBACKING_H

#include "gtest/gtest.h"

#include <cstdint>

#if defined(__linux__) && defined(__x86_64__)
#include <cerrno>
#include <linux/kvm.h>
#include <sys/ioctl.h>

namespace neverd::test {

inline bool supportsX86KVMPrefault(int KVM) {
#ifdef KVM_CAP_PRE_FAULT_MEMORY
  return ioctl(KVM, KVM_CHECK_EXTENSION, KVM_CAP_PRE_FAULT_MEMORY) > 0;
#else
  return false;
#endif
}

/// Cold EPT faults during software-vector delivery can lose a MOV-SS delayed
/// data trap in the KVM oracle. Populate stage-2 mappings before arming it;
/// guest page tables, instruction bytes and architectural state stay intact.
/// Requires KVM_PRE_FAULT_MEMORY (Linux KVM API section 4.143). The caller
/// checks capability at the test boundary so absence is a skip, not a pass.
inline void prefaultX86KVMBacking(int CPU, uint64_t Bytes) {
#ifdef KVM_PRE_FAULT_MEMORY
  kvm_pre_fault_memory Range{};
  Range.size = Bytes;
  while (Range.size) {
    const uint64_t Before = Range.size;
    ASSERT_EQ(ioctl(CPU, KVM_PRE_FAULT_MEMORY, &Range), 0) << errno;
    ASSERT_LT(Range.size, Before);
  }
#else
  ADD_FAILURE() << "KVM pre-fault memory requires newer kernel headers";
#endif
}

} // namespace neverd::test
#endif

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86KVMBACKING_H
