// Append-only x86 scalar SSE numerical/state transport.
X86ReadMXCSR, X86WriteMXCSR, X86FPAddState, X86FPSubState, X86FPMulState,
    X86FPDivState, X86FPCvtToIntState, X86FPTruncToIntState,
    // Append-only: ROUND[PS/PD/SS/SD] numerical bits and outgoing MXCSR.
    X86FPRoundState,
    // Memory source is read inside the ROUND completion, including #GP
    // ordering.
    X86FPRoundMemoryState,
    // Legacy/VEX approximate reciprocal operations preserve MXCSR completely.
    X86FPApprox12State, X86FPApprox12MemoryState,
    // Legacy/VEX elementwise arithmetic completes numerical bits and MXCSR.
    X86FPArithState, X86FPArithMemoryState,
