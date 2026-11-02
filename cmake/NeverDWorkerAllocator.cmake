include_guard(GLOBAL)

if(NOT COMMAND neverd_link_mimalloc)
  # The standalone GUI also adds the worker to a CXX-only project.
  enable_language(C)
  include("${CMAKE_CURRENT_LIST_DIR}/NeverDMimalloc.cmake")
endif()

# Preserve the existing worker-only opt-out while sharing the process policy
# and exactly one allocator dependency with NeverD's other executables.
function(neverd_worker_allocator Target)
  option(NEVERD_WORKER_MIMALLOC "Use mimalloc in the worker"
    ${NEVERD_ENABLE_MIMALLOC})
  set_property(TARGET ${Target} PROPERTY NEVERD_WORKER_ALLOCATOR system)
  if(NEVERD_WORKER_MIMALLOC AND NEVERD_MIMALLOC_ENABLED)
    neverd_link_mimalloc(${Target})
    target_compile_definitions(${Target} PRIVATE NEVERD_WORKER_HAS_MIMALLOC=1)
    set_property(TARGET ${Target} PROPERTY NEVERD_WORKER_ALLOCATOR mimalloc)
  endif()
endfunction()
