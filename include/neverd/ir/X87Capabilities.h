//===- X87Capabilities.h - Scoped x87 semantic capabilities ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared capability inventory. Availability, numerical behavior and complete
/// architectural state are separate properties; none is an equivalence proof.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X87CAPABILITIES_H
#define NEVERD_IR_X87CAPABILITIES_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <string>

namespace neverd {

inline constexpr unsigned kX87CapabilitiesSchemaVersion = 1;

enum class X87Support { Exact, Approximate, Unknown, Unsupported };
enum class X87SourceMode { I386, X64 };
enum class X87ABI { SystemV, Microsoft, MinGW };
enum class X87StatePath { TypedStack, OwnedContext };
enum class X87Consumer {
  Lift,
  IRAnalysis,
  LLVM,
  HighC,
  LLVMC,
  Concrete,
  Symbolic,
  CheckedRefinement,
  Translation
};
enum class X87Backend { None, NativeX86, Portable, NativeReference };
enum class X87Destination { I386, X64, ARM64 };
enum class X87ValueClass {
  ValueProjection,
  ExactValue,
  ProcessorDependent,
  StateOnly
};

/// Dispatch for the explicitly admitted portable owned-context slice. The
/// instruction inventory owns this mapping; None never falls back to native.
enum class X87PortableOperation {
  None,
  Initialize,
  ClearExceptions,
  Wait,
  LoadControl,
  StoreControl,
  StoreStatus,
  LoadRaw,
  StoreRawPop,
  LoadInteger,
  StoreInteger,
  StoreIntegerPop,
  Negate,
  Absolute,
  LoadRegister,
  StoreRegister,
  StoreRegisterPop,
  Exchange,
  Compare,
  ComparePop,
  ComparePopTwice,
  CompareFlags,
  CompareFlagsPop,
  TestZero,
  Add,
  Multiply,
  Subtract,
  ReverseSubtract,
  Divide,
  ReverseDivide
};

struct X87InstructionForm {
  const char *ID;
  uint8_t Escape;
  uint8_t FirstModRM;
  uint8_t LastModRM;
  bool Memory;
  const char *Mnemonic;
  const char *Operands;
  bool TypedLift;
  X87ValueClass ValueClass;
  X87PortableOperation PortableOperation;
};

/// All dimensions are explicit. A container format does not select an ABI.
struct X87CapabilityQuery {
  uint32_t Selector;
  X87SourceMode SourceMode;
  X87ABI ABI;
  X87StatePath StatePath;
  X87Consumer Consumer;
  X87Backend Backend;
  X87Destination Destination;
};

struct X87Capability {
  bool Implemented = false;
  X87Support Numerical = X87Support::Unsupported;
  X87Support ArchitecturalState = X87Support::Unsupported;
  const char *Profile = "unqualified";
  const char *Reason = "invalid-query";
  const char *Requirements = "none";
  const char *Evidence = "none";
};

llvm::ArrayRef<X87InstructionForm> x87InstructionForms();
const X87InstructionForm *x87InstructionForm(uint32_t Selector);
bool x87InstructionIsProcessorDependent(uint32_t Selector);
X87Capability queryX87Capability(const X87CapabilityQuery &Query);

/// Factorized matrix: explicit source/ABI/state paths, forms, and evaluated
/// consumer/backend/destination cells shared by forms in each coverage class.
/// Contains test-source references, never a claim that those tests were run.
std::string x87CapabilitiesJSON();

} // namespace neverd

#endif // NEVERD_IR_X87CAPABILITIES_H
