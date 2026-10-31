//===- X64ContinuationSegments.h - Shared segment transitions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Descriptor validation, cache loading and ordered accessed-bit publication.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSEGMENTS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSEGMENTS_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <vector>

namespace neverd::analysis::detail {

/// Encoding-order choices shared by profile-owned compatibility returns.
/// FRED explicitly restores every RSP bit; legacy returns do not define its
/// upper half after the mode switch.
inline std::vector<X64ContinuationCompatibilityChoice>
x64CompatibilityChoices(bool RestoresFullSP) {
  std::vector<X64ContinuationCompatibilityChoice> Choices;
  for (uint8_t I = 0; I != std::size(X64ContinuationStateV1{}.Undefined); ++I) {
    const auto Register = x86reg::generalReg(I);
    if (!RestoresFullSP || Register != x86reg::RSP)
      Choices.push_back({I, NdVar::reg(Register, 8)});
  }
  return Choices;
}

/// Standard hidden caches shared by SYSRET/SYSENTER/SYSEXIT and FRED.
template <class Writer>
void writeX64FlatContinuationSegment(Writer &W, bool Code, NdVar Selector,
                                     bool Wide, unsigned CPL) {
  using V5 = X64ContinuationStateV5;
  using Segment = X64ContinuationSegment;
  const auto Offset = Code ? offsetof(V5, CS) : offsetof(V5, SS);
  W.store(Offset + offsetof(Segment, Base), NdVar::scalar(0, 8));
  W.store(Offset + offsetof(Segment, Limit), NdVar::scalar(UINT32_MAX, 4));
  W.store(Offset + offsetof(Segment, Selector), Selector);
  W.store(Offset + offsetof(Segment, Attributes),
          NdVar::scalar((Code ? (Wide ? 0xa09b : 0xc09b) : 0xc093) | (CPL << 5),
                        2));
}

template <class Writer> class X64ContinuationSegments {
protected:
  using State = X64ContinuationStateV1;
  using V5 = X64ContinuationStateV5;
  using V6 = X64ContinuationStateV6;
  using Segment = X64ContinuationSegment;
  using Metadata = X64ContinuationSegmentMetadata;
  using Table = X64ContinuationDescriptorTable;
  using Exit = X64ContinuationExitV1;
  Writer &W;
  unsigned AddressBits;
  std::optional<NdVar> AccessedAddress;

  static NdVar k(uint64_t V) { return NdVar::scalar(V, 8); }

  NdVar op(NdOp Code, std::initializer_list<NdVar> Inputs, uint16_t Bytes = 8) {
    const auto V = W.temporary(Bytes);
    W.emit(Code, V, Inputs);
    return V;
  }

  NdVar narrow(NdVar V, uint16_t Bytes) {
    return op(NdOp::SUBBYTES, {V, k(0)}, Bytes);
  }

  NdVar load(uint64_t Offset, uint16_t Bytes = 8) {
    const auto V = W.load(Offset, Bytes);
    return Bytes == 8 ? V : op(NdOp::INT_ZEXT, {V});
  }

  NdVar masked(NdVar V, uint64_t Mask) {
    return op(NdOp::INT_AND, {V, k(Mask)});
  }

  NdVar shifted(NdVar V, unsigned Bits) {
    return op(NdOp::INT_RIGHT, {V, k(Bits)});
  }

  NdVar eq(NdVar A, NdVar B) { return op(NdOp::INT_EQUAL, {A, B}, 1); }
  NdVar ne(NdVar A, NdVar B) { return op(NdOp::INT_NOTEQUAL, {A, B}, 1); }
  NdVar less(NdVar A, NdVar B) { return op(NdOp::INT_LESS, {A, B}, 1); }
  NdVar both(NdVar A, NdVar B) { return op(NdOp::BOOL_AND, {A, B}, 1); }
  NdVar either(NdVar A, NdVar B) { return op(NdOp::BOOL_OR, {A, B}, 1); }
  NdVar choose(NdVar C, NdVar A, NdVar B) {
    return op(NdOp::SELECT, {C, A, B}, A.Size);
  }

  void canonical(NdVar V) {
    const auto Shift = k(64 - AddressBits);
    const auto Signed =
        op(NdOp::INT_ASHR, {op(NdOp::INT_LEFT, {V, Shift}), Shift});
    W.rejectIf(ne(V, Signed));
  }

  void range(NdVar Address, NdVar LastByte) {
    W.rejectIf(less(op(NdOp::INT_SUB, {k(UINT64_MAX), LastByte}), Address));
    canonical(Address);
    canonical(op(NdOp::INT_ADD, {Address, LastByte}));
  }

  void range(NdVar Address, uint64_t LastByte) { range(Address, k(LastByte)); }

  NdVar readMemory(NdVar Address, uint16_t Bytes = 8) {
    auto Value = op(NdOp::LOAD, {Address}, Bytes);
    if (Bytes != 8)
      Value = op(NdOp::INT_ZEXT, {Value});
    if (AccessedAddress) {
      const auto Delta = op(NdOp::INT_SUB, {*AccessedAddress, Address});
      // Mask before shifting: a rejected SELECT arm must never introduce a
      // C/LLVM shift outside the value width, even for disjoint addresses.
      const auto Shift = op(NdOp::INT_LEFT, {masked(Delta, 7), k(3)});
      const auto Bit = choose(less(Delta, k(Bytes)),
                              op(NdOp::INT_LEFT, {k(1), Shift}), k(0));
      Value = op(NdOp::INT_OR, {Value, Bit});
    }
    return Value;
  }

  struct Descriptor {
    NdVar Address;
    NdVar Value;
  };

  Descriptor descriptor(NdVar Selector) {
    const auto Local = ne(masked(Selector, 4), k(0));
    const auto BadLdt = either(
        ne(masked(load(offsetof(V6, Unusable), 1), 64), k(0)),
        ne(masked(load(offsetof(V6, LDTR) + offsetof(Segment, Attributes), 2),
                  0x9f),
           k(0x82)));
    W.rejectIf(both(Local, BadLdt));
    const auto Base =
        choose(Local, load(offsetof(V6, LDTR)), load(offsetof(V6, GDTR)));
    const auto Limit =
        choose(Local, load(offsetof(V6, LDTR) + offsetof(Segment, Limit), 4),
               load(offsetof(V6, GDTR) + offsetof(Table, Limit), 2));
    const auto Index = masked(Selector, 0xfff8);
    const auto Last = op(NdOp::INT_ADD, {Index, k(7)});
    W.rejectIf(less(Limit, Last));
    W.rejectIf(less(op(NdOp::INT_SUB, {k(UINT64_MAX), Last}), Base));
    const auto Address = op(NdOp::INT_ADD, {Base, Index});
    range(Address, 7);
    return {Address, readMemory(Address)};
  }

  void accessed(Descriptor D) {
    const auto Address = op(NdOp::INT_ADD, {D.Address, k(5)});
    const auto Byte = narrow(op(NdOp::INT_OR, {shifted(D.Value, 40), k(1)}), 1);
    // A descriptor with A=1 may live in read-only RAM. Do not introduce a
    // redundant write that the original instruction would never perform.
    W.storeIf(eq(masked(D.Value, uint64_t{1} << 40), k(0)), Address, Byte);
  }

  NdVar segmentBase(NdVar Descriptor) {
    return op(NdOp::INT_OR, {masked(shifted(Descriptor, 16), 0xffffff),
                             masked(shifted(Descriptor, 32), 0xff000000)});
  }

  NdVar segmentLimit(NdVar Descriptor) {
    auto Limit = op(NdOp::INT_OR, {masked(Descriptor, 0xffff),
                                   masked(shifted(Descriptor, 32), 0xf0000)});
    Limit =
        choose(ne(masked(Descriptor, uint64_t{1} << 55), k(0)),
               op(NdOp::INT_OR, {op(NdOp::INT_LEFT, {Limit, k(12)}), k(0xfff)}),
               Limit);
    return Limit;
  }

  NdVar segmentAttributes(NdVar Descriptor) {
    return op(NdOp::INT_OR, {masked(shifted(Descriptor, 40), 0xf0ff), k(1)});
  }

  void segment(uint64_t Offset, NdVar Selector, NdVar Descriptor,
               std::optional<NdVar> KeepCache = std::nullopt) {
    auto Base = segmentBase(Descriptor);
    auto Limit = segmentLimit(Descriptor);
    auto Attributes = segmentAttributes(Descriptor);
    if (KeepCache) {
      Base = choose(*KeepCache, load(Offset + offsetof(Segment, Base)), Base);
      Limit =
          choose(*KeepCache, load(Offset + offsetof(Segment, Limit), 4), Limit);
      Attributes =
          choose(*KeepCache, load(Offset + offsetof(Segment, Attributes), 2),
                 Attributes);
    }
    W.store(Offset + offsetof(Segment, Base), Base);
    W.store(Offset + offsetof(Segment, Limit), narrow(Limit, 4));
    W.store(Offset + offsetof(Segment, Selector), narrow(Selector, 2));
    W.store(Offset + offsetof(Segment, Attributes), narrow(Attributes, 2));
  }

  NdVar returnedData(uint64_t Offset, bool Base, unsigned Bit, NdVar CPL,
                     NdVar Unusable) {
    const auto SelectorOffset = Offset + (Base ? offsetof(Segment, Selector)
                                               : offsetof(Metadata, Selector));
    const auto AttributeOffset =
        Offset +
        (Base ? offsetof(Segment, Attributes) : offsetof(Metadata, Attributes));
    const auto Selector = load(SelectorOffset, 2);
    const auto Attributes = load(AttributeOffset, 2);
    const auto Invalidate = both(
        ne(CPL, k(0)), either(eq(masked(Selector, 0xfffc), k(0)),
                              both(less(masked(shifted(Attributes, 5), 3), CPL),
                                   ne(masked(Attributes, 12), k(12)))));
    W.store(SelectorOffset, narrow(choose(Invalidate, k(0), Selector), 2));
    return op(NdOp::INT_OR, {Unusable, choose(Invalidate, k(Bit), k(0))});
  }

  Descriptor returnCode(NdVar CS, NdVar CPL) {
    W.rejectIf(eq(masked(CS, 0xfffc), k(0)));
    const auto Code = descriptor(CS);
    const auto Access = shifted(Code.Value, 40);
    const auto DPL = masked(shifted(Access, 5), 3);
    W.rejectIf(ne(masked(Access, 0x98), k(0x98)));
    W.rejectIf(eq(masked(Access, 0x6000), k(0x6000)));
    W.rejectIf(
        choose(ne(masked(Access, 4), k(0)), less(CPL, DPL), ne(CPL, DPL)));
    AccessedAddress = op(NdOp::INT_ADD, {Code.Address, k(5)});
    return Code;
  }

  Descriptor returnStack(NdVar CS, NdVar CPL, NdVar SS, NdVar NullSS) {
    W.rejectIf(ne(masked(SS, 3), CPL));
    W.rejectIf(both(NullSS, eq(CPL, k(3))));
    // A null SS requires no table access. The already validated CS provides
    // a safe speculative read; its accessed bit is already pending. It cannot
    // acquire another write, and sequential RAM makes the read unobservable.
    const auto Stack = descriptor(choose(NullSS, CS, SS));
    const auto StackAccess = masked(shifted(Stack.Value, 40), 0xfa);
    const auto Expected =
        op(NdOp::INT_OR, {k(0x92), op(NdOp::INT_LEFT, {CPL, k(5)})});
    W.rejectIf(both(ne(masked(SS, 0xfffc), k(0)), ne(StackAccess, Expected)));
    return Stack;
  }

  void returnData(NdVar CPL, NdVar Unusable) {
    Unusable = returnedData(offsetof(V6, DS), true, 4, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, ES), true, 8, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, FS), false, 16, CPL, Unusable);
    Unusable = returnedData(offsetof(V6, GS), false, 32, CPL, Unusable);
    W.store(offsetof(V6, Unusable), narrow(Unusable, 1));
  }

  X64ContinuationSegments(Writer &Output, unsigned Bits)
      : W(Output), AddressBits(Bits) {}
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSEGMENTS_H
