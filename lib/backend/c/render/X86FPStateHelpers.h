//===- X86FPStateHelpers.h - Readable scalar FP state helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_X86FPSTATEHELPERS_H
#define NEVERD_BACKEND_C_RENDER_X86FPSTATEHELPERS_H

#include "neverd/ir/X86FPState.h"

#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>
#include <utility>

namespace neverd {

using X86FPStateCHelperNames =
    std::map<std::pair<Intrinsic, unsigned>, std::string>;

inline std::string x86FPStateRawCType(unsigned Bytes) {
  return Bytes <= 8 ? "uint" + std::to_string(Bytes * 8) + "_t"
                    : "unsigned _BitInt(" + std::to_string(Bytes * 8) + ")";
}

inline std::string x86FPApprox12CHelperName(Intrinsic Id, unsigned Layout) {
  std::string Name = std::string("neverd_x86_") +
                     x86FPApprox12Mnemonic(Layout) + "_bits" +
                     std::to_string(x86FPStateSourceBytes(Layout) * 8);
  if (Id == Intrinsic::X86FPApprox12MemoryState) {
    const auto Space = x86FPRoundStateAddressSpace(Layout);
    Name += Space == NdMemoryAddressSpace::X86FS   ? "_memory_fs"
            : Space == NdMemoryAddressSpace::X86GS ? "_memory_gs"
                                                   : "_memory";
  }
  return Name;
}

template <typename Stream>
inline void writeX86FPApprox12CHelper(Stream &OS, Intrinsic Id, unsigned Layout,
                                      const std::string &Name) {
  const unsigned Bytes = x86FPStateSourceBytes(Layout);
  const unsigned Control = x86FPRoundStateControl(Layout);
  const bool Memory = Id == Intrinsic::X86FPApprox12MemoryState;
  const auto Space = x86FPRoundStateAddressSpace(Layout);
  const auto Raw = x86FPStateRawCType(Bytes);
  OS << "/* Approximate reciprocal, preserving MXCSR and raw NaN bits. */\n"
     << "static inline ";
  if (Bytes == 32 || (Control & 4))
    OS << "__attribute__((target(\"avx\"))) ";
  OS << Raw << " " << Name << "(" << (Memory ? "void *address" : Raw + " bits")
     << ") {\n    ";
  if (x86FPApprox12IsScalar(Control))
    OS << "float";
  else
    OS << "typedef float approx_vector __attribute__((vector_size(" << Bytes
       << ")));\n    approx_vector";
  OS << (Memory ? " result_value;\n" : " value, result_value;\n") << "    "
     << Raw << " result;\n";
  if (!Memory)
    OS << "    __builtin_memcpy(&value, &bits, " << Bytes << ");\n";
  OS << "    __asm__ volatile(\"" << x86FPApprox12Mnemonic(Layout) << " ";
  if (Memory)
    OS << (Space == NdMemoryAddressSpace::X86FS   ? "%%fs:"
           : Space == NdMemoryAddressSpace::X86GS ? "%%gs:"
                                                  : "")
       << "(%1)";
  else
    OS << "%1";
  OS << ",%0\" : \"=&x\"(result_value) : "
     << (Memory ? "\"r\"(address)" : "\"x\"(value)") << " : \"memory\");\n"
     << "    __builtin_memcpy(&result, &result_value, " << Bytes << ");\n"
     << "    return result;\n}\n\n";
}

inline std::string x86FPRoundStateCHelperName(Intrinsic Id, unsigned Layout) {
  std::string Name = std::string("neverd_x86_") +
                     x86FPRoundStateMnemonic(Layout) + "_bits" +
                     std::to_string(x86FPStateSourceBytes(Layout) * 8) +
                     "_imm" + std::to_string(x86FPRoundStateImmediate(Layout));
  if (Id == Intrinsic::X86FPRoundMemoryState) {
    const auto Space = x86FPRoundStateAddressSpace(Layout);
    Name += Space == NdMemoryAddressSpace::X86FS   ? "_memory_fs"
            : Space == NdMemoryAddressSpace::X86GS ? "_memory_gs"
                                                   : "_memory";
  }
  return Name;
}

/// Both C routes use the same instruction completion scope. The immediate is
/// part of the helper identity so it remains an assembler constant at -O0.
template <typename Stream>
inline void
writeX86FPRoundStateCHelper(Stream &OS, Intrinsic Id, unsigned Layout,
                            const std::string &Name, bool StateAddress) {
  const unsigned Bytes = x86FPStateSourceBytes(Layout);
  const unsigned Control = x86FPRoundStateControl(Layout);
  const bool Scalar = x86FPRoundStateIsScalar(Control);
  const bool Memory = Id == Intrinsic::X86FPRoundMemoryState;
  const auto Space = x86FPRoundStateAddressSpace(Layout);
  const auto Raw = x86FPStateRawCType(Bytes);
  const auto Result = StateAddress ? Raw : x86FPStateRawCType(Bytes + 4);
  OS << "/* ROUND numerical bits and MXCSR, with one instruction completion. "
        "*/\n"
     << "static inline ";
  if (Bytes == 32 || (Control & 8))
    OS << "__attribute__((target(\"avx\"))) ";
  OS << Result << " " << Name << "("
     << (Memory ? "void *address" : Raw + " bits") << ", "
     << (StateAddress ? "void *state_address" : "uint32_t state") << ") {\n";
  if (StateAddress)
    OS << "    uint32_t state;\n"
       << "    __builtin_memcpy(&state, state_address, 4);\n";
  OS << "    ";
  if (Scalar)
    OS << (x86FPRoundStateElementBytes(Control) == 8 ? "double" : "float");
  else
    OS << "typedef "
       << (x86FPRoundStateElementBytes(Control) == 8 ? "double" : "float")
       << " round_vector __attribute__((vector_size(" << Bytes << ")));\n"
       << "    round_vector";
  OS << (Memory ? " rounded;\n" : " value, rounded;\n") << "    " << Raw
     << " result;\n";
  if (!Memory)
    OS << "    __builtin_memcpy(&value, &bits, " << Bytes << ");\n";
  OS << "    __asm__ volatile(\"ldmxcsr %1\\n\\t"
     << x86FPRoundStateMnemonic(Layout) << " $"
     << x86FPRoundStateImmediate(Layout) << ",";
  if (Memory) {
    OS << (Space == NdMemoryAddressSpace::X86FS   ? "%%fs:"
           : Space == NdMemoryAddressSpace::X86GS ? "%%gs:"
                                                  : "")
       << "(%2)";
  } else
    OS << "%2";
  OS << ",%0\\n\\tstmxcsr %1\"\n"
     << "        : \"=&x\"(rounded), \"+m\"(state) : "
     << (Memory ? "\"r\"(address)" : "\"x\"(value)") << " : \"memory\");\n"
     << "    __builtin_memcpy(&result, &rounded, " << Bytes << ");\n";
  if (StateAddress)
    OS << "    __builtin_memcpy(state_address, &state, 4);\n"
       << "    return result;\n}\n\n";
  else
    OS << "    return (" << Result << ")result | ((" << Result << ")state << "
       << Bytes * 8 << ");\n}\n\n";
}

inline std::string x86FPArithStateCHelperName(Intrinsic Id, unsigned Layout) {
  std::string Name = "neverd_x86_" + x86FPArithStateMnemonic(Layout) + "_bits" +
                     std::to_string(x86FPStateSourceBytes(Layout) * 8) +
                     "_state";
  if (isX86FPStateMemoryIntrinsic(Id)) {
    const auto Space = x86FPRoundStateAddressSpace(Layout);
    Name += Space == NdMemoryAddressSpace::X86FS   ? "_memory_fs"
            : Space == NdMemoryAddressSpace::X86GS ? "_memory_gs"
                                                   : "_memory";
  }
  return Name;
}

template <typename Stream>
inline void
writeX86FPArithStateCHelper(Stream &OS, Intrinsic Id, unsigned Layout,
                            const std::string &Name, bool StateAddress) {
  const unsigned Control = x86FPArithStateControl(Layout);
  const unsigned Bytes = x86FPStateSourceBytes(Layout);
  const bool Fma = isX86FPFmaStateIntrinsic(Id);
  const bool Unary = x86FPArithStateIsUnary(Control);
  const bool Scalar = x86FPArithStateIsScalar(Control);
  const bool Vex = Fma || Bytes == 32 || (Control & 32);
  const bool Memory = isX86FPStateMemoryIntrinsic(Id);
  const auto Space = x86FPRoundStateAddressSpace(Layout);
  const auto Raw = x86FPStateRawCType(Bytes);
  const auto Return = StateAddress ? Raw : x86FPStateRawCType(Bytes + 4);
  OS << "/* SIMD numerical bits and MXCSR complete in one instruction. */\n"
     << "static inline ";
  if (Vex)
    OS << "__attribute__((target(\"" << (Fma ? "avx,fma" : "avx") << "\"))) ";
  OS << Return << " " << Name << "(";
  if (Memory)
    OS << "void *address, " << Raw << " left_bits";
  else
    OS << Raw << " left_bits, " << Raw << " right_bits";
  if (Fma)
    OS << ", " << Raw << (Memory ? " right_bits" : " third_bits");
  OS << ", " << (StateAddress ? "void *state_address" : "uint32_t state")
     << ") {\n";
  if (StateAddress)
    OS << "    uint32_t state; __builtin_memcpy(&state, state_address, 4);\n";
  const char *ScalarName = (Control & 8) ? "double" : "float";
  if (!Scalar)
    OS << "    typedef " << ScalarName
       << " fp_vector __attribute__((vector_size(" << Bytes << ")));\n";
  const std::string Type = Scalar ? ScalarName : "fp_vector";
  OS << "    " << Type << " value;\n";
  if (!Unary)
    OS << "    __builtin_memcpy(&value, &left_bits, " << Bytes << ");\n";
  if (!Memory || Fma)
    OS << "    " << Type << " right;\n"
       << "    __builtin_memcpy(&right, &right_bits, " << Bytes << ");\n";
  if (Fma && !Memory)
    OS << "    " << Type << " third;\n"
       << "    __builtin_memcpy(&third, &third_bits, " << Bytes << ");\n";
  OS << "    __asm__ volatile(\"ldmxcsr %1\\n\\t"
     << x86FPArithStateMnemonic(Layout) << " ";
  if (Memory)
    OS << (Space == NdMemoryAddressSpace::X86FS   ? "%%fs:"
           : Space == NdMemoryAddressSpace::X86GS ? "%%gs:"
                                                  : "")
       << (Fma ? "(%3)" : "(%2)");
  else
    OS << (Fma ? "%3" : "%2");
  if (Fma)
    OS << ",%2";
  else if (!Unary && Vex)
    OS << ",%0";
  OS << ",%0\\n\\tstmxcsr %1\"\n"
     << "        : \"" << (Unary ? "=&x" : "+x")
     << "\"(value), \"+m\"(state) : "
     << (Fma ? (Memory ? "\"x\"(right), \"r\"(address)"
                       : "\"x\"(right), \"x\"(third)")
             : (Memory ? "\"r\"(address)" : "\"x\"(right)"))
     << " : \"memory\");\n"
     << "    " << Raw << " result;\n"
     << "    __builtin_memcpy(&result, &value, " << Bytes << ");\n";
  if (StateAddress)
    OS << "    __builtin_memcpy(state_address, &state, 4);\n    return "
          "result;\n}\n\n";
  else
    OS << "    return (" << Return << ")result | ((" << Return << ")state << "
       << Bytes * 8 << ");\n}\n\n";
}

inline std::string x86FPScalarValueCHelper(Intrinsic Id, unsigned Bytes) {
  if (isX86FPArithStateIntrinsic(Id))
    return x86FPArithStateCHelperName(Id, Bytes) + "_value";
  if (isX86FPApprox12Intrinsic(Id))
    return x86FPApprox12CHelperName(Id, Bytes);
  if (isX86FPRoundStateIntrinsic(Id))
    return x86FPRoundStateCHelperName(Id, Bytes) + "_value";
  if (isX86FPConversionStateIntrinsic(Id))
    return std::string("neverd_x86_") +
           x86FPStateConversionMnemonic(Id, x86FPStateSourceBytes(Bytes)) +
           "_value_f" + std::to_string(x86FPStateSourceBytes(Bytes) * 8) +
           "_i" + std::to_string(x86FPStateDestinationBytes(Id, Bytes) * 8);
  return std::string("neverd_x86_") + x86ScalarFPStateMnemonic(Id) +
         "_value_f" + std::to_string(Bytes * 8);
}

template <typename Stream>
inline void writeX86FPScalarValueCHelpers(Stream &OS,
                                          const X86FPStateCHelperNames &Used) {
  for (const auto &[Shape, Name] : Used) {
    const auto [Id, Bytes] = Shape;
    if (isX86FPArithStateIntrinsic(Id)) {
      writeX86FPArithStateCHelper(OS, Id, Bytes, Name, true);
      continue;
    }
    if (isX86FPApprox12Intrinsic(Id)) {
      writeX86FPApprox12CHelper(OS, Id, Bytes, Name);
      continue;
    }
    if (isX86FPRoundStateIntrinsic(Id)) {
      writeX86FPRoundStateCHelper(OS, Id, Bytes, Name, true);
      continue;
    }
    if (isX86FPConversionStateIntrinsic(Id)) {
      const unsigned SourceBytes = x86FPStateSourceBytes(Bytes);
      const unsigned DestinationBytes = x86FPStateDestinationBytes(Id, Bytes);
      const std::string SourceRaw =
          "uint" + std::to_string(SourceBytes * 8) + "_t";
      const std::string DestinationRaw =
          "uint" + std::to_string(DestinationBytes * 8) + "_t";
      OS << "static inline " << DestinationRaw << " " << Name << "("
         << SourceRaw << " bits, void *state_address) {\n"
         << "    " << (SourceBytes == 4 ? "float" : "double") << " value;\n"
         << "    " << DestinationRaw << " result; uint32_t state;\n"
         << "    __builtin_memcpy(&state, state_address, 4);\n"
         << "    __builtin_memcpy(&value, &bits, " << SourceBytes << ");\n"
         << "    __asm__ volatile(\"ldmxcsr %1\\n\\t"
         << x86FPStateConversionMnemonic(Id, SourceBytes)
         << " %2,%0\\n\\tstmxcsr %1\"\n"
         << "        : \"=&r\"(result), \"+m\"(state) : \"x\"(value) : "
            "\"memory\");\n"
         << "    __builtin_memcpy(state_address, &state, 4);\n"
         << "    return result;\n}\n\n";
      continue;
    }
    if (!isX86ScalarFPStateIntrinsic(Id))
      continue;
    const std::string Raw = "uint" + std::to_string(Bytes * 8) + "_t";
    const char *Scalar = Bytes == 4 ? "float" : "double";
    OS << "static inline " << Raw << " " << Name << "(" << Raw << " a_bits, "
       << Raw << " b_bits, void *state_address) {\n"
       << "    " << Scalar << " a, b; " << Raw << " result;\n"
       << "    uint32_t state;\n"
       << "    __builtin_memcpy(&state, state_address, 4);\n"
       << "    __builtin_memcpy(&a, &a_bits, " << Bytes << ");\n"
       << "    __builtin_memcpy(&b, &b_bits, " << Bytes << ");\n"
       << "    __asm__ volatile(\"ldmxcsr %1\\n\\t"
       << x86ScalarFPStateMnemonic(Id) << (Bytes == 4 ? "ss" : "sd")
       << " %2,%0\\n\\tstmxcsr %1\"\n"
       << "        : \"+x\"(a), \"+m\"(state) : \"x\"(b) : \"memory\");\n"
       << "    __builtin_memcpy(state_address, &state, 4);\n"
       << "    __builtin_memcpy(&result, &a, " << Bytes << ");\n"
       << "    return result;\n}\n\n";
  }
}

inline std::string x86FPStateCHelper(Intrinsic Id, unsigned ScalarBytes) {
  if (isX86FPArithStateIntrinsic(Id))
    return x86FPArithStateCHelperName(Id, ScalarBytes);
  if (isX86FPApprox12Intrinsic(Id))
    return x86FPApprox12CHelperName(Id, ScalarBytes);
  if (isX86FPRoundStateIntrinsic(Id))
    return x86FPRoundStateCHelperName(Id, ScalarBytes);
  if (isX86FPConversionStateIntrinsic(Id))
    return std::string(intrinsicCName(Id)) + "_f" +
           std::to_string(x86FPStateSourceBytes(ScalarBytes) * 8) + "_i" +
           std::to_string(x86FPStateDestinationBytes(Id, ScalarBytes) * 8);
  return std::string(intrinsicCName(Id)) +
         (isX86ScalarFPStateIntrinsic(Id)
              ? "_f" + std::to_string(ScalarBytes * 8)
              : "");
}

inline void writeX86FPStateCHelpers(llvm::raw_ostream &OS,
                                    const X86FPStateCHelperNames &Used) {
  for (const auto &[Shape, Name] : Used) {
    const auto [Id, Bytes] = Shape;
    if (isX86FPArithStateIntrinsic(Id)) {
      writeX86FPArithStateCHelper(OS, Id, Bytes, Name, false);
      continue;
    }
    if (isX86FPApprox12Intrinsic(Id)) {
      writeX86FPApprox12CHelper(OS, Id, Bytes, Name);
      continue;
    }
    if (isX86FPRoundStateIntrinsic(Id)) {
      writeX86FPRoundStateCHelper(OS, Id, Bytes, Name, false);
      continue;
    }
    if (Id == Intrinsic::X86ReadMXCSR) {
      OS << "static inline uint32_t " << Name << "(void) {\n"
         << "    uint32_t state;\n"
         << "    __asm__ volatile(\"stmxcsr %0\" : \"=m\"(state) :: "
            "\"memory\");\n"
         << "    return state;\n}\n\n";
      continue;
    }
    if (Id == Intrinsic::X86WriteMXCSR) {
      OS << "static inline void " << Name << "(uint32_t state) {\n"
         << "    __asm__ volatile(\"ldmxcsr %0\" :: \"m\"(state) : "
            "\"memory\");\n"
         << "}\n\n";
      continue;
    }
    if (isX86FPConversionStateIntrinsic(Id)) {
      const unsigned SourceBytes = x86FPStateSourceBytes(Bytes);
      const unsigned DestinationBytes = x86FPStateDestinationBytes(Id, Bytes);
      const std::string SourceRaw =
          "uint" + std::to_string(SourceBytes * 8) + "_t";
      const std::string DestinationRaw =
          "uint" + std::to_string(DestinationBytes * 8) + "_t";
      const std::string Result =
          DestinationBytes == 4 ? "uint64_t" : "unsigned _BitInt(96)";
      OS << "/* signed integer conversion under MXCSR; integer bits and "
            "outgoing state. */\n"
         << "static inline " << Result << " " << Name << "(" << SourceRaw
         << " bits, uint32_t state) {\n"
         << "    " << (SourceBytes == 4 ? "float" : "double") << " value;\n"
         << "    " << DestinationRaw << " result;\n"
         << "    __builtin_memcpy(&value, &bits, " << SourceBytes << ");\n"
         << "    __asm__ volatile(\"ldmxcsr %1\\n\\t"
         << x86FPStateConversionMnemonic(Id, SourceBytes)
         << " %2,%0\\n\\tstmxcsr %1\"\n"
         << "        : \"=&r\"(result), \"+m\"(state) : \"x\"(value) : "
            "\"memory\");\n"
         << "    return (" << Result << ")result | ((" << Result << ")state << "
         << DestinationBytes * 8 << ");\n}\n\n";
      continue;
    }
    const unsigned Bits = Bytes * 8;
    const std::string Raw = "uint" + std::to_string(Bits) + "_t";
    const std::string Result = Bytes == 4 ? "uint64_t" : "unsigned _BitInt(96)";
    const char *Scalar = Bytes == 4 ? "float" : "double";
    const char *Mnemonic = x86ScalarFPStateMnemonic(Id);
    const char *Operator = Id == Intrinsic::X86FPAddState   ? "+"
                           : Id == Intrinsic::X86FPSubState ? "-"
                           : Id == Intrinsic::X86FPMulState ? "*"
                                                            : "/";
    OS << "/* a " << Operator
       << " b under incoming MXCSR; result bits and outgoing state. */\n"
       << "static inline " << Result << " " << Name << "(" << Raw << " a_bits, "
       << Raw << " b_bits, uint32_t state) {\n"
       << "    " << Scalar << " a, b;\n"
       << "    " << Raw << " result;\n"
       << "    __builtin_memcpy(&a, &a_bits, " << Bytes << ");\n"
       << "    __builtin_memcpy(&b, &b_bits, " << Bytes << ");\n"
       << "    __asm__ volatile(\"ldmxcsr %1\\n\\t" << Mnemonic
       << (Bytes == 4 ? "ss" : "sd") << " %2,%0\\n\\tstmxcsr %1\"\n"
       << "        : \"+x\"(a), \"+m\"(state) : \"x\"(b) : \"memory\");\n"
       << "    __builtin_memcpy(&result, &a, " << Bytes << ");\n"
       << "    return (" << Result << ")result | ((" << Result << ")state << "
       << Bits << ");\n}\n\n";
  }
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_X86FPSTATEHELPERS_H
