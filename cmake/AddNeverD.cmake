# NeverD CMake helpers — mirrors LLVM's AddLLVM.cmake conventions.
#
# add_neverd_component_library(<name> source1 [source2 ...]
#     [LINK_COMPONENTS comp1 ...]
#     [LINK_LIBS lib1 ...])
#
# add_neverd_tool(<name> source1 [source2 ...]
#     [LINK_COMPONENTS comp1 ...]
#     [LINK_LIBS lib1 ...])

# Global list that accumulates every component library target.
define_property(GLOBAL PROPERTY NEVERD_COMPONENT_LIBS
  BRIEF_DOCS "List of all NeverD component library targets"
  FULL_DOCS  "Populated by add_neverd_component_library()")

# Common third-party libraries that all NeverD components need.
# Linked PUBLIC so transitive consumers inherit them automatically.
set(NEVERD_COMMON_LIBS
  ${LLVM_LIBS}
  capstone_static)

function(add_neverd_component_library name)
  cmake_parse_arguments(ARG
    ""
    ""
    "LINK_COMPONENTS;LINK_LIBS"
    ${ARGN})

  add_library(${name} STATIC ${ARG_UNPARSED_ARGUMENTS})

  target_include_directories(${name} PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
    $<TARGET_PROPERTY:capstone_static,INTERFACE_INCLUDE_DIRECTORIES>)

  target_compile_features(${name} PUBLIC cxx_std_20)

  set(_resolved_components "")
  foreach(_comp ${ARG_LINK_COMPONENTS})
    list(APPEND _resolved_components "NeverD${_comp}")
  endforeach()

  target_link_libraries(${name}
    PUBLIC  ${_resolved_components} ${NEVERD_COMMON_LIBS}
    PRIVATE ${ARG_LINK_LIBS})

  # Component archives are also linked into libneverd.  Their objects must be
  # position-independent on ELF platforms or the shared-library link fails.
  set_target_properties(${name} PROPERTIES
    FOLDER "NeverD/Libraries"
    POSITION_INDEPENDENT_CODE ON)
  set_property(GLOBAL APPEND PROPERTY NEVERD_COMPONENT_LIBS ${name})
endfunction()

function(add_neverd_tool name)
  cmake_parse_arguments(ARG
    ""
    ""
    "LINK_COMPONENTS;LINK_LIBS"
    ${ARGN})

  add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})

  target_compile_features(${name} PRIVATE cxx_std_20)

  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
    $<TARGET_PROPERTY:capstone_static,INTERFACE_INCLUDE_DIRECTORIES>)

  set(_resolved_components "")
  foreach(_comp ${ARG_LINK_COMPONENTS})
    list(APPEND _resolved_components "NeverD${_comp}")
  endforeach()

  target_link_libraries(${name}
    PRIVATE ${_resolved_components} ${ARG_LINK_LIBS})

  # On macOS, a tool backed by neverd_shared must resolve LLVM support symbols
  # from the dylib.  Another static LLVMSupport creates a second command-line
  # registry and makes categorized help crash.  Other platforms retain the
  # static dependency because DLL/shared-library symbol export rules differ.
  if(NOT APPLE OR NOT "neverd_shared" IN_LIST ARG_LINK_LIBS)
    if(TARGET LLVMSupport)
      target_link_libraries(${name} PRIVATE LLVMSupport LLVMDemangle)
    else()
      llvm_map_components_to_libnames(_tool_llvm_support_libs support)
      target_link_libraries(${name} PRIVATE ${_tool_llvm_support_libs})
    endif()
  endif()

  set_target_properties(${name} PROPERTIES FOLDER "NeverD/Tools")
endfunction()

# add_neverd_unittest(<name> source1 [source2 ...]
#     [LINK_COMPONENTS comp1 ...]
#     [LINK_LIBS lib1 ...])
#
# Mirrors LLVM's add_llvm_unittest().  Creates a gtest executable,
# links the requested NeverD component libraries, and registers
# tests via gtest_discover_tests().

add_custom_target(NeverDUnitTests)
set_target_properties(NeverDUnitTests PROPERTIES FOLDER "NeverD/Tests")

function(add_neverd_unittest name)
  cmake_parse_arguments(ARG
    ""
    "TIMEOUT"
    "LINK_COMPONENTS;LINK_LIBS"
    ${ARGN})

  # Per-test ctest timeout (seconds).  Defaults to 120; suites with a few
  # legitimately heavy cases (e.g. NeverDPatchFullTests, whose fully-obfuscated
  # ARM forms compile a single huge function) pass a larger value so they don't
  # flake under ctest -j parallelism on a busy machine.
  if(NOT DEFINED ARG_TIMEOUT OR ARG_TIMEOUT STREQUAL "")
    set(ARG_TIMEOUT 120)
  endif()

  add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})

  target_compile_features(${name} PRIVATE cxx_std_20)

  set(_resolved_components "")
  foreach(_comp ${ARG_LINK_COMPONENTS})
    list(APPEND _resolved_components "NeverD${_comp}")
  endforeach()

  target_link_libraries(${name}
    PRIVATE ${_resolved_components} ${ARG_LINK_LIBS}
            GTest::gtest GTest::gtest_main)

  set_target_properties(${name} PROPERTIES FOLDER "NeverD/Tests")
  add_dependencies(NeverDUnitTests ${name})

  include(GoogleTest)
  # LABELS ${name} lets `ctest -L <binary>` run just this binary's cases
  # (e.g. ctest -L NeverDSemanticTests) — pure CTest, no helper scripts.
  gtest_discover_tests(${name}
    PROPERTIES TIMEOUT ${ARG_TIMEOUT} LABELS ${name}
    # CMake treats a negative execute_process timeout as unlimited.  Use that
    # sentinel here because enumerating the full parameterized suite can take
    # longer than the GoogleTest module's finite discovery defaults.
    DISCOVERY_TIMEOUT -1)
endfunction()
