add_neverd_unittest(NeverDBytePatchTests
  BytePatchTests.cpp
  BytePatchCAPIHeaderC.c
  PatchJSONTests.cpp
  LINK_COMPONENTS Patch Sigs LibC
)
target_compile_features(NeverDBytePatchTests PRIVATE c_std_11)

if(NEVERD_ENABLE_SIMDJSON)
  # Exercise the same reader without acceleration in the default test build.
  # Its definitions satisfy the archive references before PatchJSON is pulled.
  add_neverd_unittest(NeverDPatchJSONFallbackTests
    PatchJSONTests.cpp
    ${CMAKE_SOURCE_DIR}/lib/patch/PatchJSON.cpp
    LINK_COMPONENTS Patch
  )
  target_compile_definitions(NeverDPatchJSONFallbackTests PRIVATE NEVERD_USE_SIMDJSON=0)
endif()
