cmake_minimum_required(VERSION 3.24)
project(NeverDDecoderComparison LANGUAGES CXX)

# This project is configured separately; it never changes product dependencies.
foreach(input CAPSTONE_SOURCE ZYDIS_SOURCE XED_KIT)
  set(${input} "" CACHE PATH "Local source/install root for ${input}")
  if(NOT IS_DIRECTORY "${${input}}")
    message(FATAL_ERROR "Pass -D${input}=<existing-directory>")
  endif()
endforeach()
foreach(input CAPSTONE_LIBRARY ZYDIS_LIBRARY ZYCORE_LIBRARY XED_LIBRARY ICED_LIBRARY)
  set(${input} "" CACHE FILEPATH "Prebuilt static library for ${input}")
  if(NOT EXISTS "${${input}}" OR IS_DIRECTORY "${${input}}")
    message(FATAL_ERROR "Pass -D${input}=<existing-static-library>")
  endif()
endforeach()

add_executable(decoder-compare compare.cpp)
target_compile_features(decoder-compare PRIVATE cxx_std_17)
target_compile_definitions(decoder-compare PRIVATE ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)
target_include_directories(decoder-compare PRIVATE
  "${CAPSTONE_SOURCE}/include" "${ZYDIS_SOURCE}/include"
  "${ZYDIS_SOURCE}/dependencies/zycore/include" "${XED_KIT}/include")
target_link_libraries(decoder-compare PRIVATE
  "${CAPSTONE_LIBRARY}" "${ZYDIS_LIBRARY}" "${ZYCORE_LIBRARY}"
  "${XED_LIBRARY}" "${ICED_LIBRARY}")
if(WIN32)
  # Rust std's native system dependencies for the MSVC static library.
  target_link_libraries(decoder-compare PRIVATE advapi32 bcrypt ntdll synchronization userenv ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(decoder-compare PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  if(NOT APPLE)
    target_link_libraries(decoder-compare PRIVATE m)
  endif()
endif()

add_executable(decoder-snapshot snapshot.cpp)
target_compile_features(decoder-snapshot PRIVATE cxx_std_17)
target_include_directories(decoder-snapshot PRIVATE "${CAPSTONE_SOURCE}/include")
target_link_libraries(decoder-snapshot PRIVATE "${CAPSTONE_LIBRARY}")
