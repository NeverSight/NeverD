# NeverDLLVMPrebuilt.cmake — fetch a prebuilt NeverD LLVM package.
#
# Downloads a per-arch macOS package produced by the NeverSight/llvm-project
# "NeverD LLVM Release" workflow (.github/workflows/neverd-release.yml),
# verifies its SHA256, extracts it into a user cache, and points LLVM_DIR at
# the extracted lib/cmake/llvm. The root CMakeLists then proceeds with the
# normal find_package(LLVM CONFIG) path — no local LLVM compile needed. This is
# the alternative to the default channel (building the fork locally).
#
# Enable with -DNEVERD_LLVM_PREBUILT=ON. Tunable cache variables:
#   NEVERD_LLVM_PREBUILT_REPO      owner/repo hosting the releases
#   NEVERD_LLVM_PREBUILT_TAG       release tag to pull
#   NEVERD_LLVM_PREBUILT_BASE_URL  override base URL (mirror); empty = GitHub
#   NEVERD_LLVM_PREBUILT_SHA256    pin the tarball hash; empty = use published .sha256
#   NEVERD_LLVM_PREBUILT_CACHE_DIR where packages are cached/extracted

set(NEVERD_LLVM_PREBUILT_REPO "NeverSight/llvm-project"
    CACHE STRING "GitHub owner/repo that hosts the prebuilt LLVM releases")
set(NEVERD_LLVM_PREBUILT_TAG "neverd-llvm-v23.0.0"
    CACHE STRING "Release tag of the prebuilt LLVM package to download")
set(NEVERD_LLVM_PREBUILT_BASE_URL ""
    CACHE STRING "Override the release base URL (advanced/mirror). Empty = GitHub releases")
set(NEVERD_LLVM_PREBUILT_SHA256 ""
    CACHE STRING "Expected SHA256 of the tarball (strong pin). Empty = verify against published .sha256")
set(NEVERD_LLVM_PREBUILT_CACHE_DIR "$ENV{HOME}/.cache/neverd-llvm"
    CACHE PATH "Where downloaded/extracted prebuilt LLVM packages are cached")

function(neverd_fetch_prebuilt_llvm)
  if(NOT APPLE)
    message(FATAL_ERROR
      "NEVERD_LLVM_PREBUILT only provides macOS packages. On other platforms "
      "build the fork locally (the default — omit -DNEVERD_LLVM_PREBUILT).")
  endif()

  # --- Resolve host arch -> package token (arm64 / x86_64) ---
  if(CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _n)
    if(_n GREATER 1)
      message(FATAL_ERROR
        "NEVERD_LLVM_PREBUILT does not support universal builds "
        "(CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}'). Pick a single "
        "arch, or build the fork locally (the default).")
    endif()
    set(_proc "${CMAKE_OSX_ARCHITECTURES}")
  else()
    set(_proc "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()

  # Only arm64 (Apple Silicon) packages are published. For Intel hosts, build
  # the fork locally (the default channel).
  if(_proc MATCHES "^(arm64|aarch64)$")
    set(_arch "arm64")
  else()
    message(FATAL_ERROR
      "NEVERD_LLVM_PREBUILT only publishes arm64 (Apple Silicon) packages; "
      "host arch is '${_proc}'. Build the fork locally (the default — omit "
      "-DNEVERD_LLVM_PREBUILT) instead.")
  endif()

  set(_pkg "neverd-llvm-macos-${_arch}")
  set(_tag "${NEVERD_LLVM_PREBUILT_TAG}")
  set(_root "${NEVERD_LLVM_PREBUILT_CACHE_DIR}/${_tag}/${_arch}")
  set(_prefix "${_root}/${_pkg}")
  set(_cfg "${_prefix}/lib/cmake/llvm/LLVMConfig.cmake")

  if(EXISTS "${_cfg}")
    message(STATUS "NeverD prebuilt LLVM: reusing cached ${_prefix}")
  else()
    if(NEVERD_LLVM_PREBUILT_BASE_URL)
      set(_base "${NEVERD_LLVM_PREBUILT_BASE_URL}")
    else()
      set(_base "https://github.com/${NEVERD_LLVM_PREBUILT_REPO}/releases/download/${_tag}")
    endif()
    set(_tar_url "${_base}/${_pkg}.tar.xz")
    set(_sha_url "${_base}/${_pkg}.tar.xz.sha256")
    set(_tar "${_root}/${_pkg}.tar.xz")

    file(MAKE_DIRECTORY "${_root}")

    message(STATUS "NeverD prebuilt LLVM: downloading ${_tar_url}")
    file(DOWNLOAD "${_tar_url}" "${_tar}" SHOW_PROGRESS STATUS _dl TLS_VERIFY ON)
    list(GET _dl 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
      list(GET _dl 1 _dl_msg)
      file(REMOVE "${_tar}")
      message(FATAL_ERROR
        "Failed to download ${_tar_url}: ${_dl_msg}\n"
        "Check NEVERD_LLVM_PREBUILT_TAG (='${_tag}') and that the asset exists.")
    endif()

    # --- expected checksum ---
    if(NEVERD_LLVM_PREBUILT_SHA256)
      string(TOLOWER "${NEVERD_LLVM_PREBUILT_SHA256}" _expected)
    else()
      set(_sha_file "${_root}/${_pkg}.tar.xz.sha256")
      file(DOWNLOAD "${_sha_url}" "${_sha_file}" STATUS _sdl TLS_VERIFY ON)
      list(GET _sdl 0 _sdl_code)
      if(NOT _sdl_code EQUAL 0)
        file(REMOVE "${_tar}")
        message(FATAL_ERROR
          "Downloaded the tarball but failed to fetch its checksum ${_sha_url}. "
          "Pin it manually with -DNEVERD_LLVM_PREBUILT_SHA256=<hash>.")
      endif()
      file(READ "${_sha_file}" _sha_raw)
      string(REGEX MATCH "[0-9a-fA-F]+" _expected "${_sha_raw}")
      string(TOLOWER "${_expected}" _expected)
    endif()

    file(SHA256 "${_tar}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(NOT _actual STREQUAL _expected)
      file(REMOVE "${_tar}")
      message(FATAL_ERROR
        "SHA256 mismatch for ${_pkg}.tar.xz\n"
        "  expected: ${_expected}\n  actual:   ${_actual}")
    endif()
    message(STATUS "NeverD prebuilt LLVM: checksum OK (${_actual})")

    message(STATUS "NeverD prebuilt LLVM: extracting into ${_root}")
    file(ARCHIVE_EXTRACT INPUT "${_tar}" DESTINATION "${_root}")

    if(NOT EXISTS "${_cfg}")
      message(FATAL_ERROR
        "Extraction did not produce ${_cfg}; the package layout may have changed.")
    endif()
  endif()

  set(LLVM_DIR "${_prefix}/lib/cmake/llvm" CACHE PATH "Set by NEVERD_LLVM_PREBUILT" FORCE)
  set(LLVM_DIR "${_prefix}/lib/cmake/llvm" PARENT_SCOPE)
  message(STATUS "NeverD prebuilt LLVM: LLVM_DIR=${_prefix}/lib/cmake/llvm")
endfunction()
