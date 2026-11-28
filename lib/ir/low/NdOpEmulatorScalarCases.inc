//===- NdOpEmulatorScalarCases.inc - Copy and arithmetic dispatch --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared switch cases for known operands and for operands validated by
/// the architectural-state path.
//===----------------------------------------------------------------------===//

case NdOp::COPY:
case NdOp::INT_ZEXT:
case NdOp::INT_SEXT:
case NdOp::SUBBYTES:
case NdOp::INT_NEGATE:
case NdOp::INT_NEG2:
case NdOp::CONCAT: {
  return executeCopy(Op);
}

case NdOp::INT_ADD:
case NdOp::INT_SUB:
case NdOp::INT_MULT:
case NdOp::INT_DIV:
case NdOp::INT_SDIV:
case NdOp::INT_REM:
case NdOp::INT_SREM:
case NdOp::INT_AND:
case NdOp::INT_OR:
case NdOp::INT_XOR:
case NdOp::INT_LEFT:
case NdOp::INT_RIGHT:
case NdOp::INT_ASHR: {
  return executeArith(Op);
}
