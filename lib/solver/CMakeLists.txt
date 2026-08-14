add_neverd_component_library(NeverDSolver
  bv/BitBlastArith.cpp
  bv/BitBlastShift.cpp
  bv/BitBlaster.cpp
  bv/BitVectorSolver.cpp
  bv/SymSynthVerifier.cpp
  cnf/CnfEncoder.cpp
  sat/SatAnalyze.cpp
  sat/SatHeuristic.cpp
  sat/SatPropagate.cpp
  sat/SatSolver.cpp

  LINK_COMPONENTS
  Symbolic
)
