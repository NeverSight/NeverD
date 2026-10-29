//===- x64_continuation_event_state.h - Native event test wire state -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// C fixture layout checked against the public continuation V6 offsets.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_DEVIRTUALIZATION_FIXTURES_X64_CONTINUATION_EVENT_STATE_H
#define NEVERD_UNITTESTS_DEVIRTUALIZATION_FIXTURES_X64_CONTINUATION_EVENT_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint64_t base;
  uint32_t limit;
  uint16_t selector, attributes;
} event_segment_t;

typedef struct {
  uint32_t limit;
  uint16_t selector, attributes;
} event_metadata_t;

typedef struct {
  uint64_t base;
  uint16_t limit, reserved;
  uint32_t reserved_tail;
} event_table_t;

typedef struct __attribute__((aligned(16))) {
  uint64_t gpr[16], flags, pc, fs_base, gs_base;
  uint8_t zmm[32][64];
  uint64_t arbitrary[8];
  uint64_t cr8, kernel_gs;
  uint32_t mxcsr, reserved_v2;
  uint64_t reserved_v2_tail, cr2, reserved_v4;
  uint64_t star;
  event_segment_t cs, ss;
  uint64_t reserved_v5;
  event_table_t gdt, idt;
  event_segment_t ldt, tr, ds, es;
  event_metadata_t fs, gs;
  uint8_t unusable, nmi_blocked, reserved[14];
} event_state_t;

_Static_assert(sizeof(event_state_t) == 2496, "continuation V6 fixture size");
_Static_assert(offsetof(event_state_t, gdt) == 2368, "V6 GDTR");
_Static_assert(offsetof(event_state_t, fs) == 2464, "V6 FS metadata");
_Static_assert(offsetof(event_state_t, nmi_blocked) == 2481, "V6 NMI state");

#endif // NEVERD_UNITTESTS_DEVIRTUALIZATION_FIXTURES_X64_CONTINUATION_EVENT_STATE_H
