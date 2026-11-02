include_guard(GLOBAL)

# A Qt/LLVM-independent reader shared by the SDK and standalone desktop build.
function(neverd_add_project_database)
  if(TARGET NeverDProjectDatabase)
    return()
  endif()
  include(FetchContent)
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NeverDZlib.cmake")
  FetchContent_Declare(neverd_project_sqlite
    URL https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(neverd_project_sqlite)
  add_library(neverd_project_sqlite STATIC "${neverd_project_sqlite_SOURCE_DIR}/sqlite3.c")
  target_include_directories(neverd_project_sqlite SYSTEM PUBLIC "${neverd_project_sqlite_SOURCE_DIR}")
  target_compile_definitions(neverd_project_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
  set_target_properties(neverd_project_sqlite PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  find_package(Threads REQUIRED)
  target_link_libraries(neverd_project_sqlite PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
  add_library(NeverDProjectDatabase STATIC "${_root}/lib/support/ProjectDatabase.cpp")
  target_include_directories(NeverDProjectDatabase PUBLIC "${_root}/include")
  target_compile_features(NeverDProjectDatabase PUBLIC cxx_std_20)
  target_link_libraries(NeverDProjectDatabase PRIVATE neverd_project_sqlite ZLIB::ZLIB)
  set_target_properties(NeverDProjectDatabase PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
  install(FILES "${_root}/LICENSES/sqlite/SOURCE.md"
    DESTINATION share/neverd/licenses/sqlite)
endfunction()
