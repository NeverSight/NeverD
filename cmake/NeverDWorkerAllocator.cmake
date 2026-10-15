include_guard(GLOBAL)

# The allocator belongs to the worker executable. The SDK keeps the allocator
# supplied by its host, including when a user embeds it in another application.
function(neverd_worker_allocator Target)
  set(_default OFF)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_default ON)
  endif()
  set(_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}")
  foreach(_name IN ITEMS Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${_name}" _config)
    string(APPEND _flags " ${CMAKE_C_FLAGS_${_config}} ${CMAKE_CXX_FLAGS_${_config}}")
  endforeach()
  set(_sanitizer OFF)
  if(_flags MATCHES "-fsanitize=[^ ]*(address|thread|memory)")
    set(_sanitizer ON)
    set(_default OFF)
  endif()
  option(NEVERD_WORKER_MIMALLOC "Embed mimalloc in the Linux worker" ${_default})
  if(NOT NEVERD_WORKER_MIMALLOC)
    return()
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NEVERD_WORKER_MIMALLOC requires a 64-bit Linux target")
  endif()
  if(_sanitizer)
    message(FATAL_ERROR "Set NEVERD_WORKER_MIMALLOC=OFF when using an allocation sanitizer")
  endif()
  include(FetchContent)
  # Configure the upstream project in an excluded subdirectory, so its SDK,
  # examples and install rules do not become part of the NeverD distribution.
  FetchContent_Declare(neverd_worker_mimalloc
    URL https://codeload.github.com/microsoft/mimalloc/tar.gz/refs/tags/v3.5.3
    URL_HASH SHA256=3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR neverd-embed-only)
  FetchContent_MakeAvailable(neverd_worker_mimalloc)
  set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(MI_OVERRIDE ON)
  set(MI_BUILD_SHARED OFF)
  set(MI_BUILD_STATIC OFF)
  set(MI_BUILD_OBJECT ON)
  set(MI_BUILD_TESTS OFF)
  # Preserve the worker's existing CPU requirements; do not raise the minimum
  # ISA to AVX2 or ARMv8.1 merely to optimize allocation.
  set(MI_OPT_ARCH OFF)
  set(MI_OPT_SIMD OFF)
  add_subdirectory("${neverd_worker_mimalloc_SOURCE_DIR}"
                   "${neverd_worker_mimalloc_BINARY_DIR}" EXCLUDE_FROM_ALL)
  # Use the pinned upstream platform dependency detection for the object
  # target too (notably libatomic on architectures needing outlined atomics).
  get_directory_property(_system_libraries
    DIRECTORY "${neverd_worker_mimalloc_SOURCE_DIR}" DEFINITION mi_libraries)
  target_link_libraries(mimalloc-obj INTERFACE ${_system_libraries})
  # Keep libatomic when required, without adding an unused runtime dependency
  # on architectures where the compiler emits every atomic inline.
  target_link_options(mimalloc-obj INTERFACE "LINKER:--as-needed")
  target_link_libraries(${Target} PRIVATE mimalloc-obj)
  set_property(TARGET ${Target} PROPERTY NEVERD_WORKER_ALLOCATOR mimalloc)
  set(_license "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../LICENSES/mimalloc/LICENSE")
  add_custom_command(TARGET ${Target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${Target}>/licenses/mimalloc"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_license}"
      "$<TARGET_FILE_DIR:${Target}>/licenses/mimalloc/LICENSE")
  install(FILES "${_license}" DESTINATION share/neverd/licenses/mimalloc)
endfunction()
