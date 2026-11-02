//===- SessionTestAccess.h - Private session test mutations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/DataAccessWidths.h"
#include "neverd/sdk/NeverDCAPITypes.h"

#include <optional>
#include <utility>
#include <vector>

namespace neverd {
struct BinaryImage;
class Decoder;
struct DecodedInsn;
} // namespace neverd

namespace neverd::sdk {
struct Session;
struct FunctionPage;
struct InstructionFlow;

// Same-build test seam. Mutations that destroy engine objects run in the
// shared library; this header is not installed with the public SDK.
struct SessionTestAccess {
  NEVERD_API static void resetFunctions(Session &State);
  NEVERD_API static void selectFunction(Session &State, neverd_va_t Entry);
  /// Whether the next selection keeps the function list as it is.
  NEVERD_API static bool keepsFunctionList(const Session &State);
  /// Whether the function list and the names it was built under are those a
  /// rebuild from the image makes now.  The session is left as it was.
  NEVERD_API static bool functionListIsRebuilt(Session &State);
  NEVERD_API static void clearPipeline(Session &State);
  NEVERD_API static void clearFeatures(Session &State);
  NEVERD_API static const DataAccessWidths *dataAccessWidths(Session &State);
  NEVERD_API static std::optional<DataAccessWidths>
  collectDataAccessWidths(Session &State);
  /// Same-build comparison of the filtered width scan with complete LowIR.
  NEVERD_API static std::vector<std::pair<neverd_va_t, unsigned>>
  instructionDataAccesses(const BinaryImage &Img, Decoder &Dec,
                          const DecodedInsn &Insn, bool Filter);
  NEVERD_API static InstructionFlow instructionFlow(const BinaryImage &Img,
                                                    Decoder &Dec,
                                                    const DecodedInsn &Insn);
  NEVERD_API static FunctionPage functionPage(const Session &State,
                                              neverd_va_t First, int Limit);
  /// findProgramMain of \p Img.
  NEVERD_API static std::optional<neverd_va_t>
  programMain(const BinaryImage &Img);
};
} // namespace neverd::sdk
