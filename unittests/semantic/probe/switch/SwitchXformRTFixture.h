//===- SwitchXformRTFixture.h - switch xform probe fixtures -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The four parameterized fixtures shared by the AllPlatform_SwitchXform*RTTests
// translation units of the NeverDSwitchXformTests target.  The suite's dispatch
// shapes are split across several TUs that each instantiate these fixtures, so
// the class definitions live here; the single `Verify` pattern for each fixture
// is defined once, in AllPlatform_SwitchXformRTTests.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SEMANTIC_PROBE_SWITCH_SWITCHXFORMRTFIXTURE_H
#define NEVERD_UNITTESTS_SEMANTIC_PROBE_SWITCH_SWITCHXFORMRTFIXTURE_H

#include "SemanticRoundTripFixture.h"

class X64SwXformRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
class X86SwXformRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
class A64SwXformRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
class ARM32SwXformRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};

#endif // NEVERD_UNITTESTS_SEMANTIC_PROBE_SWITCH_SWITCHXFORMRTFIXTURE_H
