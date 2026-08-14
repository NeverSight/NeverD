add_neverd_unittest(NeverDSolverTests
  BitBlasterTests.cpp
  BitVectorSolverTests.cpp
  CnfEncoderTests.cpp
  SatSolverTests.cpp

  LINK_COMPONENTS Solver
)

target_include_directories(NeverDSolverTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
