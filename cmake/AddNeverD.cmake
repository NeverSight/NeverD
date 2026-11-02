# NeverD CMake helpers — mirrors LLVM's AddLLVM.cmake conventions.
#
# add_neverd_component_library(<name> source1 [source2 ...]
#     [LINK_COMPONENTS comp1 ...]
#     [LLVM_COMPONENTS comp1 ...]
#     [LINK_LIBS lib1 ...])
#
# add_neverd_tool(<name> source1 [source2 ...]
#     [LINK_COMPONENTS comp1 ...]
#     [LINK_LIBS lib1 ...])

include(${CMAKE_CURRENT_LIST_DIR}/NeverDMimalloc.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/NeverDPerformance.cmake)
neverd_add_task_scheduler()

# Global list that accumulates every component library target.
define_property(GLOBAL PROPERTY NEVERD_COMPONENT_LIBS
  BRIEF_DOCS "List of all NeverD component library targets"
  FULL_DOCS  "Populated by add_neverd_component_library()")

# Default third-party dependencies for NeverD components. Libraries with a
# narrower LLVM boundary can specify LLVM_COMPONENTS. Dependencies are PUBLIC
# so transitive consumers inherit them automatically.
set(NEVERD_COMMON_LIBS
  ${LLVM_LIBS}
  capstone_static neverd_task_scheduler)

# Place native plugins below the executable directory on every generator.  A
# multi-configuration generator otherwise appends the configuration after the
# generic `plugins` directory, which does not match the CLI's sibling lookup.
function(set_neverd_plugin_output_directories target)
  if(CMAKE_CONFIGURATION_TYPES)
    set(_runtime_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/plugins")
    set(_library_dir "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/$<CONFIG>/plugins")
  else()
    set(_runtime_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    set(_library_dir "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/plugins")
  endif()

  set_target_properties(${target} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_library_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${_runtime_dir}")
endfunction()

function(add_neverd_component_library name)
  cmake_parse_arguments(ARG
    ""
    ""
    "LINK_COMPONENTS;LLVM_COMPONENTS;LINK_LIBS"
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

  set(_common_libs ${NEVERD_COMMON_LIBS})
  if(ARG_LLVM_COMPONENTS)
    llvm_map_components_to_libnames(_llvm_libs ${ARG_LLVM_COMPONENTS})
    set(_common_libs ${_llvm_libs} capstone_static neverd_task_scheduler)
  endif()

  target_link_libraries(${name}
    PUBLIC  ${_resolved_components} ${_common_libs}
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
  neverd_link_mimalloc(${name})
  neverd_stage_performance_notices(${name})

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
#     [TIMEOUT seconds]
#     [DISCOVERY_TIMEOUT seconds]
#     [NO_PRETTY_VALUES]
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
    "NO_PRETTY_VALUES"
    "TIMEOUT;DISCOVERY_TIMEOUT"
    "LINK_COMPONENTS;LINK_LIBS"
    ${ARGN})

  # Per-test ctest timeout (seconds).  Defaults to 120; suites with a few
  # legitimately heavy cases (e.g. NeverDPatchFullTests, whose fully-obfuscated
  # ARM forms compile a single huge function) pass a larger value so they don't
  # flake under ctest -j parallelism on a busy machine.
  if(NOT DEFINED ARG_TIMEOUT OR ARG_TIMEOUT STREQUAL "")
    set(ARG_TIMEOUT 120)
  endif()

  # GoogleTest enumeration has a separate finite timeout.  Most binaries
  # discover quickly; inventory-heavy suites can opt into a larger bound.
  if(NOT DEFINED ARG_DISCOVERY_TIMEOUT OR ARG_DISCOVERY_TIMEOUT STREQUAL "")
    set(ARG_DISCOVERY_TIMEOUT 120)
  endif()

  add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})
  neverd_link_mimalloc(${name})

  target_compile_features(${name} PRIVATE cxx_std_20)

  # GoogleTest and LLVM template instantiations can exceed the standard COFF
  # section count even in a small fixture translation unit.
  if(MSVC)
    target_compile_options(${name} PRIVATE /bigobj)
  endif()

  set(_resolved_components "")
  foreach(_comp ${ARG_LINK_COMPONENTS})
    list(APPEND _resolved_components "NeverD${_comp}")
  endforeach()

  target_link_libraries(${name}
    PRIVATE ${_resolved_components} ${ARG_LINK_LIBS}
            GTest::gtest GTest::gtest_main)

  set_target_properties(${name} PROPERTIES FOLDER "NeverD/Tests")
  add_dependencies(NeverDUnitTests ${name})

  # Keep focused SBF aggregation complete as the suite grows. Test binaries
  # follow the existing NeverDSBF* naming convention and register themselves
  # here rather than duplicating a dependency list in unittests/CMakeLists.txt.
  if(name MATCHES "^NeverDSBF")
    set_property(GLOBAL APPEND PROPERTY NEVERD_SBF_TEST_TARGETS ${name})
  endif()

  if(CMAKE_CURRENT_SOURCE_DIR STREQUAL "${CMAKE_SOURCE_DIR}/unittests/emulation")
    neverd_sign_hypervisor(${name})
  endif()
  include(GoogleTest)
  # LABELS ${name} lets `ctest -L <binary>` run just this binary's cases.
  # PRE_TEST avoids concurrent POST_BUILD discovery races across test targets.
  # Discovery gets its own finite timeout; PROPERTIES TIMEOUT remains the
  # independent per-test execution timeout.  A test keeps no signature files
  # in the user's cache (see include/neverd/sigs/SignatureCache.def); one that
  # tests the cache gives it a directory of its own.
  # Required named parameter cases need stable identities, not diagnostic
  # GetParam dumps that can contain process addresses.
  set(_discovery_options)
  if(ARG_NO_PRETTY_VALUES)
    list(APPEND _discovery_options NO_PRETTY_VALUES)
  endif()
  gtest_discover_tests(${name}
    ${_discovery_options}
    PROPERTIES TIMEOUT ${ARG_TIMEOUT} LABELS ${name}
               ENVIRONMENT "NEVERD_SIGNATURE_CACHE=off"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT ${ARG_DISCOVERY_TIMEOUT})
endfunction()
