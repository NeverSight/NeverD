add_neverd_unittest(NeverDTypesTests
  DWARFTypeImportTests.cpp
  HeaderLibraryTests.cpp
  LocalTypesTests.cpp
  PDBTypeImportTests.cpp
  TypeFitTests.cpp
  TypeJSONTests.cpp
  TypeParseTests.cpp
  TypeRenderTests.cpp

  LINK_COMPONENTS
  Types
)

target_include_directories(NeverDTypesTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(NeverDTypesTests PRIVATE
  NEVERD_TYPES_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
