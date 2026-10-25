# The fork owns the bounded Rust ABI; NeverD owns the Capstone ID mapping.
include_guard(GLOBAL)
find_program(NEVERD_CARGO_EXECUTABLE cargo REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(NEVERD_ICED_RUST_TARGET "" CACHE STRING "Rust target triple for cross compilation")
if(CMAKE_CROSSCOMPILING AND NOT NEVERD_ICED_RUST_TARGET)
  message(FATAL_ERROR "Set NEVERD_ICED_RUST_TARGET when cross compiling the iced adapter")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _iced_arch_count)
  if(_iced_arch_count GREATER 1)
    message(FATAL_ERROR "Build the iced adapter once per Apple architecture before combining outputs")
  endif()
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR
     AND NOT NEVERD_ICED_RUST_TARGET)
    message(FATAL_ERROR "Set NEVERD_ICED_RUST_TARGET for the selected Apple architecture")
  endif()
endif()
set(_iced_root "${PROJECT_SOURCE_DIR}/third_party/iced")
set(_iced_crate "${_iced_root}/src/rust/iced-x86-capstone")
if(NOT EXISTS "${_iced_crate}/Cargo.toml")
  message(FATAL_ERROR "Initialize the NeverSight iced submodule with its Capstone adapter")
endif()
set(_iced_target_dir "${CMAKE_BINARY_DIR}/iced-cargo")
set(_iced_output_dir "${_iced_target_dir}")
set(_iced_target_arg)
if(NEVERD_ICED_RUST_TARGET)
  list(APPEND _iced_target_arg --target "${NEVERD_ICED_RUST_TARGET}")
  string(APPEND _iced_output_dir "/${NEVERD_ICED_RUST_TARGET}")
endif()
if(WIN32)
  set(_iced_archive "iced_x86_capstone.lib")
else()
  set(_iced_archive "libiced_x86_capstone.a")
endif()
file(GLOB_RECURSE _iced_sources CONFIGURE_DEPENDS
  "${_iced_root}/src/rust/iced-x86/src/*.rs" "${_iced_crate}/src/*.rs")
add_custom_command(
  OUTPUT "${_iced_output_dir}/$<IF:$<CONFIG:Debug>,debug,release>/${_iced_archive}"
  COMMAND "${NEVERD_CARGO_EXECUTABLE}" build --locked
          --manifest-path "${_iced_crate}/Cargo.toml"
          --target-dir "${_iced_target_dir}" ${_iced_target_arg}
          "$<$<NOT:$<CONFIG:Debug>>:--release>"
  DEPENDS ${_iced_sources} "${_iced_crate}/Cargo.toml" "${_iced_crate}/Cargo.lock"
          "${_iced_root}/src/rust/iced-x86/Cargo.toml"
  COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(neverd_iced_build DEPENDS
  "${_iced_output_dir}/$<IF:$<CONFIG:Debug>,debug,release>/${_iced_archive}")
add_library(neverd_iced STATIC IMPORTED GLOBAL)
set_target_properties(neverd_iced PROPERTIES
  IMPORTED_LOCATION "${_iced_output_dir}/release/${_iced_archive}"
  IMPORTED_LOCATION_DEBUG "${_iced_output_dir}/debug/${_iced_archive}"
  INTERFACE_INCLUDE_DIRECTORIES "${_iced_crate}/include")
add_dependencies(neverd_iced neverd_iced_build)
find_package(Threads REQUIRED)
if(WIN32)
  set_property(TARGET neverd_iced PROPERTY INTERFACE_LINK_LIBRARIES
    "ws2_32;userenv;ntdll;bcrypt;advapi32")
else()
  set_property(TARGET neverd_iced PROPERTY INTERFACE_LINK_LIBRARIES
    "Threads::Threads;${CMAKE_DL_LIBS};m")
endif()
set(_iced_ids "${CMAKE_BINARY_DIR}/iced-generated/X86IcedIds.inc")
add_custom_command(OUTPUT "${_iced_ids}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/generate_iced_capstone_map.py"
          "${_iced_root}/src/rust/iced-x86/src/code.rs"
          "${PROJECT_SOURCE_DIR}/third_party/capstone/include/capstone/x86.h"
          "${_iced_ids}"
  DEPENDS "${PROJECT_SOURCE_DIR}/scripts/generate_iced_capstone_map.py"
          "${_iced_root}/src/rust/iced-x86/src/code.rs"
          "${PROJECT_SOURCE_DIR}/third_party/capstone/include/capstone/x86.h"
  VERBATIM)
add_library(NeverDX86IcedScan STATIC
  "${PROJECT_SOURCE_DIR}/lib/decode/X86IcedScan.cpp" "${_iced_ids}")
set_target_properties(NeverDX86IcedScan PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(NeverDX86IcedScan PRIVATE "${CMAKE_BINARY_DIR}/iced-generated"
  "${PROJECT_SOURCE_DIR}/third_party/capstone")
target_include_directories(NeverDX86IcedScan PUBLIC
  "${PROJECT_SOURCE_DIR}/third_party/capstone/include")
target_compile_features(NeverDX86IcedScan PRIVATE cxx_std_20)
target_link_libraries(NeverDX86IcedScan PRIVATE neverd_iced capstone_static)
install(FILES "${_iced_root}/LICENSE.txt"
              "${_iced_crate}/LICENSES/lazy_static-MIT.txt"
  DESTINATION share/neverd/licenses/iced)
