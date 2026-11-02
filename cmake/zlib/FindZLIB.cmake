# Keep dependency find_package calls on the provider selected before LLVM/Qt.
include("${CMAKE_CURRENT_LIST_DIR}/../NeverDZlib.cmake")
get_target_property(ZLIB_INCLUDE_DIRS ZLIB::ZLIB INTERFACE_INCLUDE_DIRECTORIES)
list(GET ZLIB_INCLUDE_DIRS -1 ZLIB_INCLUDE_DIR)
get_target_property(ZLIB_VERSION ZLIB::ZLIB NEVERD_ZLIB_VERSION)
set(ZLIB_VERSION_STRING "${ZLIB_VERSION}")
set(ZLIB_LIBRARIES ZLIB::ZLIB)
set(ZLIB_LIBRARY ZLIB::ZLIB)
foreach(_config RELEASE DEBUG)
  get_target_property(ZLIB_LIBRARY_${_config} ZLIB::ZLIB "IMPORTED_LOCATION_${_config}")
endforeach()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZLIB
  REQUIRED_VARS ZLIB_LIBRARIES ZLIB_INCLUDE_DIRS VERSION_VAR ZLIB_VERSION)
