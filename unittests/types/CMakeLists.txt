add_neverd_unittest(NeverDTypesTests
  DWARFTypeImportTests.cpp
  HeaderLibraryCallModelTests.cpp
  HeaderLibraryTests.cpp
  LocalTypesTests.cpp
  PDBTypeImportTests.cpp
  TypeFitTests.cpp
  TypeImportTests.cpp
  TypeJSONTests.cpp
  TypeParseTests.cpp
  TypeRenderTests.cpp

  LINK_COMPONENTS
  LibC
  Types
)

target_include_directories(NeverDTypesTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(NeverDTypesTests PRIVATE
  NEVERD_TYPES_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# Keep parser lifetime checks independent of loader/call-model dependencies
# so a sanitizer build need not instrument the binary-analysis pipeline.
add_neverd_unittest(NeverDTypeParseTests
  TypeParseTests.cpp

  LINK_COMPONENTS
  Types
)

target_compile_definitions(NeverDTypeParseTests PRIVATE
  NEVERD_TYPES_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
