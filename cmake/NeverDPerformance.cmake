# Optional acceleration libraries from NeverSight default branches.
# No allocator replacement is supplied by these dependencies.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/NeverDForks.cmake")
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

option(NEVERD_ENABLE_HIGHWAY "Use Highway for bounded byte and integer searches" ON)
option(NEVERD_ENABLE_SIMDUTF "Use simdutf for valid UTF-8 fast paths" ON)
option(NEVERD_ENABLE_SIMDJSON "Use simdjson for transport and patch parsing" ON)
option(NEVERD_ENABLE_TBB "Use oneTBB pipeline scheduling" ON)

function(neverd_add_task_scheduler)
  if(TARGET neverd_task_scheduler)
    return()
  endif()
  get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  find_package(Threads REQUIRED)
  add_library(neverd_task_scheduler_runtime STATIC "${_root}/lib/support/Parallel.cpp")
  target_compile_features(neverd_task_scheduler_runtime PUBLIC cxx_std_20)
  target_include_directories(neverd_task_scheduler_runtime PUBLIC "${_root}/include")
  target_link_libraries(neverd_task_scheduler_runtime PRIVATE Threads::Threads)
  target_compile_definitions(neverd_task_scheduler_runtime PRIVATE NEVERD_TASK_SCHEDULER_EXPORTS)
  set_target_properties(neverd_task_scheduler_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
  # Component archives are reused by standalone tools and SDK clients. Select
  # the implementation at the final link, so an SDK client cannot embed a
  # second scheduler through a component's transitive dependencies.
  add_library(neverd_task_scheduler INTERFACE)
  target_compile_features(neverd_task_scheduler INTERFACE cxx_std_20)
  target_include_directories(neverd_task_scheduler INTERFACE "${_root}/include")
  # Inspect the final link list at generation time: some SDK tests add their
  # shared dependency after add_neverd_unittest has created the executable.
  set(_shared_sdk "$<IN_LIST:neverd_shared,$<TARGET_PROPERTY:LINK_LIBRARIES>>")
  set(_shared_override "$<BOOL:$<TARGET_PROPERTY:NEVERD_SHARED_TASK_SCHEDULER>>")
  target_link_libraries(neverd_task_scheduler INTERFACE
    "$<$<NOT:$<OR:${_shared_sdk},${_shared_override}>>:neverd_task_scheduler_runtime>")
  if(NOT NEVERD_ENABLE_TBB)
    return()
  endif()
  neverd_populate_fork(neverd_tbb)
  set(TBB_TEST OFF)
  set(TCM_BUILD OFF)
  set(TCM_TEST OFF)
  set(TBB_EXAMPLES OFF)
  set(TBB_STRICT OFF)
  set(TBBMALLOC_BUILD OFF)
  set(TBBMALLOC_PROXY_BUILD OFF)
  set(TBB_USE_SYSTEM_ALLOCATOR ON)
  set(TBB_INSTALL OFF)
  set(TBB_ENABLE_IPO OFF)
  set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON)
  set(BUILD_SHARED_LIBS OFF)
  add_subdirectory("${neverd_tbb_SOURCE_DIR}"
    "${neverd_tbb_BINARY_DIR}" EXCLUDE_FROM_ALL)
  set_target_properties(tbb PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(tbb INTERFACE __TBB_NO_IMPLICIT_LINKAGE=1)
  target_compile_definitions(neverd_task_scheduler_runtime PRIVATE NEVERD_USE_TBB=1)
  target_link_libraries(neverd_task_scheduler_runtime PRIVATE TBB::tbb)
  install(DIRECTORY "${neverd_tbb_NOTICES_DIR}/" DESTINATION share/neverd/licenses/oneTBB)
endfunction()

function(neverd_use_shared_task_scheduler Target)
  set_property(TARGET ${Target} PROPERTY NEVERD_SHARED_TASK_SCHEDULER ON)
endfunction()

function(neverd_own_task_scheduler Target)
  set_property(TARGET ${Target} PROPERTY NEVERD_SHARED_TASK_SCHEDULER OFF)
  target_link_libraries(${Target} PRIVATE neverd_task_scheduler)
  if(NEVERD_ENABLE_TBB)
    # Only NeverD's callback boundary is shared. Keep the embedded TBB ABI
    # private so an embedding host cannot accidentally bind to our runtime.
    if(APPLE)
      target_link_options(${Target} PRIVATE "LINKER:-load_hidden,$<TARGET_FILE:tbb>")
    elseif(UNIX)
      target_link_options(${Target} PRIVATE "LINKER:--exclude-libs,$<TARGET_FILE_NAME:tbb>")
    endif()
  endif()
endfunction()

# Stage \p Source, a notice directory or file, as licenses/<Directory> beside
# \p Target. Every target in the shared output directory takes it from one
# target, as the mimalloc notices do (NeverDMimalloc.cmake): a copy after each
# link raced with the others' copies into the same directory when they linked
# in parallel, which Windows reports as "Error copying directory ...
# Permission denied". A target with a directory of its own copies there.
function(neverd_stage_notices Target Directory Source)
  get_target_property(_type ${Target} TYPE)
  if(_type STREQUAL "SHARED_LIBRARY" AND NOT WIN32)
    get_target_property(_output_dir ${Target} LIBRARY_OUTPUT_DIRECTORY)
  else()
    get_target_property(_output_dir ${Target} RUNTIME_OUTPUT_DIRECTORY)
  endif()
  if(IS_DIRECTORY "${Source}")
    set(_copy copy_directory)
  else()
    set(_copy copy_if_different)
  endif()
  if(_output_dir STREQUAL "${CMAKE_BINARY_DIR}/bin")
    set(_stage neverd-notices-${Directory})
    if(NOT TARGET ${_stage})
      set(_dir "${CMAKE_BINARY_DIR}/bin")
      get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
      if(_multi_config)
        string(APPEND _dir "/$<CONFIG>")
      endif()
      add_custom_target(${_stage}
        COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${_dir}/licenses/${Directory}"
        COMMAND "${CMAKE_COMMAND}" -E ${_copy} "${Source}"
          "${_dir}/licenses/${Directory}"
        COMMENT "Copying the ${Directory} notices next to NeverD executables"
        VERBATIM)
    endif()
    add_dependencies(${Target} ${_stage})
  else()
    add_custom_command(TARGET ${Target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${Target}>/licenses/${Directory}"
      COMMAND "${CMAKE_COMMAND}" -E ${_copy} "${Source}"
        "$<TARGET_FILE_DIR:${Target}>/licenses/${Directory}"
      VERBATIM)
  endif()
endfunction()

function(neverd_stage_performance_notices Target)
  # The SDK registers its post-build notices before the transport directory
  # is configured. Resolve that provider before capturing its notice path.
  neverd_add_simdjson()
  foreach(_pair "HIGHWAY;highway" "SIMDUTF;simdutf" "SIMDJSON;simdjson" "TBB;oneTBB")
    list(GET _pair 0 _option)
    list(GET _pair 1 _directory)
    if(NEVERD_ENABLE_${_option})
      string(TOLOWER "${_option}" _name)
      get_property(_notices GLOBAL PROPERTY NEVERD_FORK_NOTICES_neverd_${_name})
      neverd_stage_notices(${Target} ${_directory} "${_notices}")
    endif()
  endforeach()
endfunction()

function(neverd_add_highway)
  if(NOT NEVERD_ENABLE_HIGHWAY OR TARGET hwy)
    return()
  endif()
  neverd_populate_fork(neverd_highway)
  # Highway declares these with set(CACHE), rather than option(). Its older
  # policy baseline would otherwise discard ordinary scoped overrides.
  set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "Build Highway contrib" FORCE)
  set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "Build Highway examples" FORCE)
  set(HWY_ENABLE_TESTS OFF CACHE BOOL "Build Highway tests" FORCE)
  set(HWY_ENABLE_INSTALL OFF CACHE BOOL "Install Highway development files" FORCE)
  set(HWY_FORCE_STATIC_LIBS ON)
  add_subdirectory("${neverd_highway_SOURCE_DIR}"
    "${neverd_highway_BINARY_DIR}" EXCLUDE_FROM_ALL)
  set_target_properties(hwy PROPERTIES POSITION_INDEPENDENT_CODE ON)
  install(DIRECTORY "${neverd_highway_NOTICES_DIR}/" DESTINATION share/neverd/licenses/highway)
endfunction()

function(neverd_add_sorted_search)
  if(TARGET neverd_sorted_search)
    return()
  endif()
  get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  add_library(neverd_sorted_search STATIC "${_root}/lib/support/SortedSearch.cpp")
  target_compile_features(neverd_sorted_search PUBLIC cxx_std_20)
  target_include_directories(neverd_sorted_search PUBLIC "${_root}/include")
  set_target_properties(neverd_sorted_search PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(NEVERD_ENABLE_HIGHWAY)
    neverd_add_highway()
    target_link_libraries(neverd_sorted_search PRIVATE hwy)
    target_compile_definitions(neverd_sorted_search PRIVATE NEVERD_USE_HIGHWAY=1)
    target_include_directories(neverd_sorted_search PRIVATE "${_root}/lib/support")
  endif()
endfunction()

function(neverd_add_text_acceleration)
  if(TARGET neverd_text_acceleration)
    return()
  endif()
  get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  add_library(neverd_text_acceleration STATIC
    "${_root}/lib/support/ByteScan.cpp"
    "${_root}/lib/support/Unicode.cpp")
  target_compile_features(neverd_text_acceleration PUBLIC cxx_std_20)
  target_include_directories(neverd_text_acceleration PUBLIC "${_root}/include")
  set_target_properties(neverd_text_acceleration PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  if(NEVERD_ENABLE_HIGHWAY)
    neverd_add_highway()
    target_link_libraries(neverd_text_acceleration PRIVATE hwy)
    target_compile_definitions(neverd_text_acceleration PRIVATE NEVERD_USE_HIGHWAY=1)
    target_include_directories(neverd_text_acceleration PRIVATE "${_root}/lib/support")
  endif()
  if(NEVERD_ENABLE_SIMDUTF)
    neverd_populate_fork(neverd_simdutf)
    set(SIMDUTF_TESTS OFF)
    set(SIMDUTF_TOOLS OFF)
    set(SIMDUTF_BENCHMARKS OFF)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${neverd_simdutf_SOURCE_DIR}"
      "${neverd_simdutf_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set_target_properties(simdutf PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(neverd_text_acceleration PRIVATE simdutf)
    target_compile_definitions(neverd_text_acceleration PRIVATE NEVERD_USE_SIMDUTF=1)
    install(DIRECTORY "${neverd_simdutf_NOTICES_DIR}/" DESTINATION share/neverd/licenses/simdutf)
  endif()
endfunction()

function(neverd_add_simdjson)
  if(NOT NEVERD_ENABLE_SIMDJSON OR TARGET simdjson)
    return()
  endif()
  neverd_populate_fork(neverd_simdjson)
  set(SIMDJSON_DEVELOPER_MODE OFF)
  set(BUILD_SHARED_LIBS OFF)
  add_subdirectory("${neverd_simdjson_SOURCE_DIR}"
    "${neverd_simdjson_BINARY_DIR}" EXCLUDE_FROM_ALL)
  set_target_properties(simdjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
  install(DIRECTORY "${neverd_simdjson_NOTICES_DIR}/"
    DESTINATION share/neverd/licenses/simdjson)
endfunction()
