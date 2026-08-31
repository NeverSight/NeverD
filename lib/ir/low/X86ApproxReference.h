//===- X86ApproxReference.h - x86 approximation reference API -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_APPROX_REFERENCE_H
#define NEVERD_IR_LOW_X86_APPROX_REFERENCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t neverd_x86_rcp14_f32(uint32_t mxcsr, uint32_t source);
uint64_t neverd_x86_rcp14_f64(uint32_t mxcsr, uint64_t source);
uint32_t neverd_x86_rsqrt14_f32(uint32_t mxcsr, uint32_t source);
uint64_t neverd_x86_rsqrt14_f64(uint32_t mxcsr, uint64_t source);
uint32_t neverd_x86_rcp28_f32(uint32_t source, uint32_t *flags);
uint64_t neverd_x86_rcp28_f64(uint64_t source, uint32_t *flags);
uint32_t neverd_x86_rsqrt28_f32(uint32_t source, uint32_t *flags);
uint64_t neverd_x86_rsqrt28_f64(uint64_t source, uint32_t *flags);
uint32_t neverd_x86_exp2_f32(uint32_t source, uint32_t *flags);
uint64_t neverd_x86_exp2_f64(uint64_t source, uint32_t *flags);

#ifdef __cplusplus
}
#endif

#endif
