# NeverD Agent ships with the desktop workbench. NEVERD_BUILD_AGENT is AUTO,
# ON or OFF: AUTO builds the Agent with the workbench when the Agent's own
# dependencies are present and says why it does not otherwise; ON requires
# them. The decision is NEVERD_AGENT_ENABLED.
include_guard(GLOBAL)

set(NEVERD_BUILD_AGENT AUTO CACHE STRING
  "Build NeverD Agent with the workbench: AUTO (when its dependencies are present), ON or OFF")
set_property(CACHE NEVERD_BUILD_AGENT PROPERTY STRINGS AUTO ON OFF)

set(NEVERD_AGENT_ENABLED OFF)
set(_neverd_agent_auto OFF)
if(NEVERD_BUILD_AGENT STREQUAL "AUTO")
  set(_neverd_agent_auto ON)
endif()
# AUTO in a whole-repository build follows the workbench.
if(_neverd_agent_auto AND DEFINED NEVERD_BUILD_GUI AND NOT NEVERD_BUILD_GUI)
  set(_neverd_agent_auto OFF)
elseif(_neverd_agent_auto OR NEVERD_BUILD_AGENT)
  set(_neverd_agent_missing "")
  if(NOT WIN32 AND NOT APPLE)
    # Model credentials live in the desktop's keyring through libsecret.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(NEVERD_AGENT_LIBSECRET QUIET libsecret-1)
    endif()
    if(NOT NEVERD_AGENT_LIBSECRET_FOUND)
      set(_neverd_agent_missing "the libsecret-1 development files (libsecret-1-dev)")
    endif()
  endif()
  if(_neverd_agent_missing STREQUAL "")
    set(NEVERD_AGENT_ENABLED ON)
  elseif(_neverd_agent_auto)
    message(STATUS "NeverD Agent omitted: ${_neverd_agent_missing} not found. "
                   "Install them, or set NEVERD_BUILD_AGENT=OFF.")
  else()
    message(FATAL_ERROR "NEVERD_BUILD_AGENT=ON requires ${_neverd_agent_missing}.")
  endif()
endif()
# Packaging scripts read the decision from the cache.
set(NEVERD_AGENT_ENABLED "${NEVERD_AGENT_ENABLED}" CACHE INTERNAL
  "Whether NeverD Agent is part of this build")
