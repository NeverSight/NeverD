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
#include "neverd/ir/X86/X86ReturnRules.h"
#include "neverd/ir/X86/X86SegmentDescriptorRules.h"
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"
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
  using Value = NdVar;
  using Rules =
      neverd::detail::X86SegmentDescriptorRules<X64ContinuationSegments>;
  friend Rules;
  using ReturnRules = neverd::detail::X86ReturnRules<X64ContinuationSegments>;
  friend ReturnRules;
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
  std::optional<NdVar> DeliveryExternal;
  bool InstructionDescriptors = false;

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

  NdVar bitOr(NdVar A, NdVar B) { return op(NdOp::INT_OR, {A, B}); }
  NdVar left(NdVar A, unsigned Bits) {
    return op(NdOp::INT_LEFT, {A, k(Bits)});
  }

  NdVar eq(NdVar A, NdVar B) { return op(NdOp::INT_EQUAL, {A, B}, 1); }
  NdVar ne(NdVar A, NdVar B) { return op(NdOp::INT_NOTEQUAL, {A, B}, 1); }
  NdVar less(NdVar A, NdVar B) { return op(NdOp::INT_LESS, {A, B}, 1); }
  NdVar both(NdVar A, NdVar B) { return op(NdOp::BOOL_AND, {A, B}, 1); }
  NdVar either(NdVar A, NdVar B) { return op(NdOp::BOOL_OR, {A, B}, 1); }
  NdVar choose(NdVar C, NdVar A, NdVar B) {
    return op(NdOp::SELECT, {C, A, B}, A.Size);
  }

  void faultIf(NdVar Bad, uint8_t Vector, NdVar Error) {
    if (DeliveryExternal)
      W.architecturalFaultIf(Bad, Vector, Error);
    else
      W.rejectIf(Bad);
  }

  void canonical(NdVar V, uint8_t Vector = 13) {
    const auto Shift = k(64 - AddressBits);
    const auto Signed =
        op(NdOp::INT_ASHR, {op(NdOp::INT_LEFT, {V, Shift}), Shift});
    faultIf(ne(V, Signed), Vector, DeliveryExternal.value_or(k(0)));
  }

  void range(NdVar Address, NdVar LastByte, uint8_t Vector = 13) {
    if constexpr (!Writer::FaultableMemory)
      faultIf(less(op(NdOp::INT_SUB, {k(UINT64_MAX), LastByte}), Address),
              Vector, DeliveryExternal.value_or(k(0)));
    canonical(Address, Vector);
    canonical(op(NdOp::INT_ADD, {Address, LastByte}), Vector);
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

  NdVar readStackMemory(NdVar Address, uint16_t Bytes,
                        std::optional<NdVar> When = std::nullopt) {
    if constexpr (Writer::FaultableMemory) {
      W.instructionAccess(true, true);
      auto Value = When
                       ? W.loadMemoryIf(*When, Address, NdVar::scalar(0, Bytes))
                       : op(NdOp::LOAD, {Address}, Bytes);
      W.instructionAccess(false);
      return Bytes == 8 ? Value : op(NdOp::INT_ZEXT, {Value});
    } else {
      return readMemory(Address, Bytes);
    }
  }

  Descriptor descriptor(NdVar Selector,
                        std::optional<NdVar> ReadIf = std::nullopt) {
    const auto Error =
        DeliveryExternal
            ? op(NdOp::INT_OR, {masked(Selector, 0xfffc), *DeliveryExternal})
            : k(0);
    const auto Local = ne(masked(Selector, 4), k(0));
    const auto BadLdt = either(
        ne(masked(load(offsetof(V6, Unusable), 1), 64), k(0)),
        ne(masked(load(offsetof(V6, LDTR) + offsetof(Segment, Attributes), 2),
                  0x9f),
           k(0x82)));
    const auto Enabled = [&](NdVar Bad) {
      return ReadIf ? both(*ReadIf, Bad) : Bad;
    };
    faultIf(Enabled(both(Local, BadLdt)), 13, Error);
    const auto Base =
        choose(Local, load(offsetof(V6, LDTR)), load(offsetof(V6, GDTR)));
    const auto Limit =
        choose(Local, load(offsetof(V6, LDTR) + offsetof(Segment, Limit), 4),
               load(offsetof(V6, GDTR) + offsetof(Table, Limit), 2));
    const auto Index = masked(Selector, 0xfff8);
    const auto Last = op(NdOp::INT_ADD, {Index, k(7)});
    faultIf(Enabled(less(Limit, Last)), 13, Error);
    if constexpr (!Writer::FaultableMemory)
      W.rejectIf(less(op(NdOp::INT_SUB, {k(UINT64_MAX), Last}), Base));
    const auto Address = op(NdOp::INT_ADD, {Base, Index});
    if constexpr (Writer::FaultableMemory) {
      if (InstructionDescriptors) {
        // System descriptors are implicit supervisor accesses. A null return
        // SS must not read the CS descriptor again or acquire an accessed
        // write.
        const auto Shift = k(64 - AddressBits);
        const auto Noncanonical = [&](NdVar A) {
          return ne(
              A, op(NdOp::INT_ASHR, {op(NdOp::INT_LEFT, {A, Shift}), Shift}));
        };
        faultIf(
            Enabled(either(Noncanonical(Address),
                           Noncanonical(op(NdOp::INT_ADD, {Address, k(7)})))),
            13, Error);
        if (ReadIf)
          return {Address,
                  W.loadMemoryIf(*ReadIf, Address, k(uint64_t{1} << 40))};
        return {Address, readMemory(Address)};
      }
    }
    range(Address, 7);
    return {Address, readMemory(Address)};
  }

  void accessed(Descriptor D) {
    if constexpr (Writer::FaultableMemory) {
      if (InstructionDescriptors) {
        W.storeIf(eq(masked(D.Value, uint64_t{1} << 40), k(0)), D.Address,
                  op(NdOp::INT_OR, {D.Value, k(uint64_t{1} << 40)}));
        return;
      }
    }
    const auto Address = op(NdOp::INT_ADD, {D.Address, k(5)});
    const auto Byte = narrow(op(NdOp::INT_OR, {shifted(D.Value, 40), k(1)}), 1);
    // A descriptor with A=1 may live in read-only RAM. Do not introduce a
    // redundant write that the original instruction would never perform.
    W.storeIf(eq(masked(D.Value, uint64_t{1} << 40), k(0)), Address, Byte);
  }

  NdVar segmentBase(NdVar Descriptor) { return Rules::base(*this, Descriptor); }

  NdVar segmentLimit(NdVar Descriptor) {
    return Rules::limit(*this, Descriptor);
  }

  NdVar segmentAttributes(NdVar Descriptor) {
    return Rules::attributes(*this, Descriptor);
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
    const auto Invalidate =
        ReturnRules::invalidateData(*this, Selector, Attributes, CPL);
    W.store(SelectorOffset, narrow(choose(Invalidate, k(0), Selector), 2));
    return op(NdOp::INT_OR, {Unusable, choose(Invalidate, k(Bit), k(0))});
  }

  Descriptor returnCode(NdVar CS, std::optional<NdVar> OldCPL = std::nullopt) {
    faultIf(eq(masked(CS, 0xfffc), k(0)), 13, k(0));
    const auto Code = descriptor(CS);
    // Fetch precedes inward-RPL rejection under the bound return profiles.
    ReturnRules::checkCode(*this, CS, Code.Value, OldCPL.value_or(k(0)), true);
    if constexpr (!Writer::FaultableMemory)
      AccessedAddress = op(NdOp::INT_ADD, {Code.Address, k(5)});
    return Code;
  }

  NdVar interruptReturnFlags(NdVar Old, NdVar Saved, NdVar CPL,
                             unsigned Bytes) {
    return ReturnRules::interruptFlags(*this, Old, Saved, CPL, Bytes);
  }

  Descriptor returnStack(NdVar CS, NdVar CPL, NdVar SS, NdVar NullSS,
                         std::optional<NdVar> When = std::nullopt) {
    if constexpr (Writer::FaultableMemory) {
      const auto Enabled = [&](NdVar Bad) {
        return When ? both(*When, Bad) : Bad;
      };
      faultIf(
          Enabled(both(NullSS, either(eq(CPL, k(3)), ne(masked(SS, 3), CPL)))),
          13, k(0));
      const auto Error = masked(SS, 0xfffc);
      const auto Stack =
          descriptor(SS, Enabled(eq(NullSS, NdVar::scalar(0, 1))));
      faultIf(Enabled(both(eq(NullSS, NdVar::scalar(0, 1)),
                           ne(masked(SS, 3), CPL))),
              13, Error);
      const auto Access = shifted(Stack.Value, 40);
      const auto Invalid = either(ne(masked(Access, 0x1a), k(0x12)),
                                  ne(masked(shifted(Access, 5), 3), CPL));
      faultIf(Enabled(both(eq(NullSS, NdVar::scalar(0, 1)), Invalid)), 13,
              Error);
      faultIf(Enabled(both(eq(NullSS, NdVar::scalar(0, 1)),
                           eq(masked(Access, 0x80), k(0)))),
              12, Error);
      return Stack;
    }
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

  /// Shared descriptor validation and final cache publication. Completion
  /// publishes the instruction's coupled GPR/stack state only after success.
  template <class Completion>
  void loadDataSegment(X86SegmentRegister Register, NdVar Selector,
                       std::optional<X64NullSegmentBasePolicy> NullBasePolicy,
                       Completion Complete) {
    const bool SS = Register == X86SegmentRegister::SS;
    const bool GS = Register == X86SegmentRegister::GS;
    const bool FS = Register == X86SegmentRegister::FS;
    const bool FullCache = !GS && !FS;
    const uint64_t Offset = SS   ? offsetof(V5, SS)
                            : GS ? offsetof(V6, GS)
                            : FS ? offsetof(V6, FS)
                            : Register == X86SegmentRegister::DS
                                ? offsetof(V6, DS)
                                : offsetof(V6, ES);
    const uint64_t SelectorOffset =
        Offset + (FullCache ? offsetof(Segment, Selector)
                            : offsetof(Metadata, Selector));
    const uint64_t BaseOffset =
        GS ? offsetof(State, GSBase) : offsetof(State, FSBase);
    const unsigned UnusableBit = SS                                   ? 2
                                 : GS                                 ? 32
                                 : FS                                 ? 16
                                 : Register == X86SegmentRegister::DS ? 4
                                                                      : 8;
    const auto Null = eq(masked(Selector, 0xfffc), k(0));
    NdVar CPL = k(0);
    if constexpr (Writer::FaultableMemory)
      CPL = masked(load(offsetof(V5, CS) + offsetof(Segment, Selector), 2), 3);
    if (FullCache || NullBasePolicy) {
      // Null SS is permitted at CPL0..2 with RPL=CPL. A null load never
      // consults the GDT/LDT, even when their addresses are unmapped or
      // noncanonical.
      if (SS) {
        if constexpr (Writer::FaultableMemory)
          faultIf(both(Null, either(eq(CPL, k(3)), ne(Selector, CPL))), 13,
                  k(0));
        else
          W.rejectIf(both(Null, ne(Selector, k(0))));
      }
      W.exitIf(Null, [&] {
        if (!FullCache && *NullBasePolicy == X64NullSegmentBasePolicy::Clear)
          W.store(BaseOffset, k(0));
        W.store(SelectorOffset, narrow(Selector, 2));
        W.store(offsetof(V6, Unusable),
                narrow(op(NdOp::INT_OR,
                          {load(offsetof(V6, Unusable), 1), k(UnusableBit)}),
                       1));
        Complete();
      });
    } else {
      W.rejectIf(Null);
    }
    const auto D = descriptor(Selector);
    if constexpr (Writer::FaultableMemory) {
      Rules::checkData(*this, SS, Selector, D.Value, CPL);
    } else {
      const auto Access = shifted(D.Value, 40);
      if (SS) {
        W.rejectIf(ne(masked(Selector, 3), k(0)));
        // Present writable data, DPL=CPL=0; expand-down and A are independent.
        W.rejectIf(ne(masked(Access, 0xfa), k(0x92)));
      } else {
        W.rejectIf(ne(masked(Access, 0x90), k(0x90)));
        W.rejectIf(eq(masked(Access, 0x0a), k(8))); // Execute-only code.
        const auto DPL = masked(shifted(Access, 5), 3);
        W.rejectIf(both(ne(masked(Access, 12), k(12)),
                        less(DPL, masked(Selector, 3))));
      }
    }
    // Explicit operands are captured before the conditional descriptor write.
    accessed(D);
    if (FullCache) {
      segment(Offset, Selector, D.Value);
    } else {
      W.store(BaseOffset, segmentBase(D.Value));
      W.store(Offset + offsetof(Metadata, Limit),
              narrow(segmentLimit(D.Value), 4));
      W.store(SelectorOffset, narrow(Selector, 2));
      W.store(Offset + offsetof(Metadata, Attributes),
              narrow(segmentAttributes(D.Value), 2));
    }
    W.store(
        offsetof(V6, Unusable),
        narrow(masked(load(offsetof(V6, Unusable), 1), 0xff ^ UnusableBit), 1));
    Complete();
  }

  X64ContinuationSegments(Writer &Output, unsigned Bits)
      : W(Output), AddressBits(Bits) {}
};

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSEGMENTS_H
