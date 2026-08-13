# NeverDLLVMPrebuilt.cmake — fetch a prebuilt NeverD LLVM package.
#
# Downloads the host package produced by the NeverSight/llvm-project
# "NeverD LLVM Release" workflow (.github/workflows/neverd-release.yml),
# verifies its SHA256, extracts it into a user cache, and points LLVM_DIR at
# the extracted lib/cmake/llvm. The root CMakeLists then proceeds with the
# normal find_package(LLVM CONFIG) path — no local LLVM compile needed.
#
# Published hosts:
#   macOS arm64   -> neverd-llvm-macos-arm64.tar.xz
#   Linux x86_64 -> neverd-llvm-linux-x86_64.tar.xz
#   Windows x64  -> neverd-llvm-windows-x64.zip
#
# Enable with -DNEVERD_LLVM_PREBUILT=ON. Tunable cache variables:
#   NEVERD_LLVM_PREBUILT_REPO      owner/repo hosting the releases
#   NEVERD_LLVM_PREBUILT_TAG       release tag to pull
#   NEVERD_LLVM_PREBUILT_BASE_URL  override base URL (mirror); empty = GitHub
#   NEVERD_LLVM_PREBUILT_SHA256    override the archive hash; empty = use the
#                                  digests pinned below, or the published
#                                  .sha256 for a tag they do not cover
#   NEVERD_LLVM_PREBUILT_CACHE_DIR where packages are cached/extracted

set(NEVERD_LLVM_PREBUILT_REPO "NeverSight/llvm-project"
    CACHE STRING "GitHub owner/repo that hosts the prebuilt LLVM releases")
set(NEVERD_LLVM_PREBUILT_TAG "neverd-llvm-v23.0.0"
    CACHE STRING "Release tag of the prebuilt LLVM package to download")
set(NEVERD_LLVM_PREBUILT_BASE_URL ""
    CACHE STRING "Override the release base URL (advanced/mirror). Empty = GitHub releases")
set(NEVERD_LLVM_PREBUILT_SHA256 ""
    CACHE STRING "Override the expected SHA256 of the archive. Empty = use the pinned digest")

# The fork republishes this tag in place whenever its sources change, so the
# tag names a stream of builds rather than one build, and a cache keyed on the
# tag cannot tell which of them it holds.  These digests are what names a
# particular build, and pinning them here is what ties a NeverD revision to the
# LLVM package it was written against.
#
# Refresh all three together after republishing the release.  A stale entry
# stops the configure with a checksum mismatch that says which digest it
# wanted, rather than resurfacing later as a header the older package happened
# not to contain.
set(NEVERD_LLVM_PREBUILT_PINNED_TAG "neverd-llvm-v23.0.0")
set(_NEVERD_LLVM_PIN_LINUX_X86_64
    "6e66d2f96c4015f66b1e98110b1f36fd2f5e2b57acc80742902e2d4fda0e9da9")
set(_NEVERD_LLVM_PIN_MACOS_ARM64
    "4b2986265d1512d1425e48c35bef155259004b1ac6ed3abb14c96e8cac044f07")
set(_NEVERD_LLVM_PIN_WINDOWS_X64
    "9d35a27c004b1271ec0e3b95e9b2a34d9228ff9df39c73b38a40b309625569d2")
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(_neverd_llvm_default_cache "$ENV{HOME}/.cache/neverd-llvm")
elseif(DEFINED ENV{USERPROFILE} AND NOT "$ENV{USERPROFILE}" STREQUAL "")
  set(_neverd_llvm_default_cache "$ENV{USERPROFILE}/.cache/neverd-llvm")
else()
  set(_neverd_llvm_default_cache "${CMAKE_BINARY_DIR}/.cache/neverd-llvm")
endif()
set(NEVERD_LLVM_PREBUILT_CACHE_DIR "${_neverd_llvm_default_cache}"
    CACHE PATH "Where downloaded/extracted prebuilt LLVM packages are cached")
unset(_neverd_llvm_default_cache)

function(_neverd_resolve_prebuilt_llvm_package
         out_platform out_arch out_pkg out_archive)
  set(_host_os "${CMAKE_HOST_SYSTEM_NAME}")

  if(_host_os STREQUAL "Darwin")
    if(CMAKE_OSX_ARCHITECTURES)
      list(LENGTH CMAKE_OSX_ARCHITECTURES _arch_count)
      if(_arch_count GREATER 1)
        message(FATAL_ERROR
          "NEVERD_LLVM_PREBUILT does not support universal builds "
          "(CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}'). Pick a "
          "single architecture or build the LLVM fork locally.")
      endif()
      list(GET CMAKE_OSX_ARCHITECTURES 0 _processor)
    else()
      set(_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${_processor}" _processor)

    if(_processor MATCHES "^(arm64|aarch64)$")
      set(_platform "macos")
      set(_arch "arm64")
      set(_pkg "neverd-llvm-macos-arm64")
      set(_archive "tar.xz")
    else()
      message(FATAL_ERROR
        "NEVERD_LLVM_PREBUILT only publishes arm64 macOS packages; host "
        "architecture is '${_processor}'. Build the LLVM fork locally instead.")
    endif()
  elseif(_host_os STREQUAL "Linux")
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _processor)
    if(_processor MATCHES "^(x86_64|amd64|x64)$")
      set(_platform "linux")
      set(_arch "x86_64")
      set(_pkg "neverd-llvm-linux-x86_64")
      set(_archive "tar.xz")
    else()
      message(FATAL_ERROR
        "NEVERD_LLVM_PREBUILT only publishes x86_64 Linux packages; host "
        "architecture is '${_processor}'. Build the LLVM fork locally instead.")
    endif()
  elseif(_host_os STREQUAL "Windows")
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _processor)
    if(_processor MATCHES "^(x86_64|amd64|x64)$")
      set(_platform "windows")
      set(_arch "x64")
      set(_pkg "neverd-llvm-windows-x64")
      set(_archive "zip")
    else()
      message(FATAL_ERROR
        "NEVERD_LLVM_PREBUILT only publishes x64 Windows packages; host "
        "architecture is '${_processor}'. Build the LLVM fork locally instead.")
    endif()
  else()
    message(FATAL_ERROR
      "NEVERD_LLVM_PREBUILT does not publish packages for host OS "
      "'${_host_os}'. Build the LLVM fork locally instead.")
  endif()

  set(${out_platform} "${_platform}" PARENT_SCOPE)
  set(${out_arch} "${_arch}" PARENT_SCOPE)
  set(${out_pkg} "${_pkg}" PARENT_SCOPE)
  set(${out_archive} "${_archive}" PARENT_SCOPE)
endfunction()

# The pinned digest for a package, or empty when the requested tag is not the
# one the pins describe.  A caller that overrode the tag is asking for a build
# this file makes no claim about, so its archive is verified against the
# checksum published beside it instead.
function(_neverd_pinned_llvm_sha256 out_sha pkg)
  set(_sha "")
  if(NEVERD_LLVM_PREBUILT_TAG STREQUAL NEVERD_LLVM_PREBUILT_PINNED_TAG)
    if(pkg STREQUAL "neverd-llvm-linux-x86_64")
      set(_sha "${_NEVERD_LLVM_PIN_LINUX_X86_64}")
    elseif(pkg STREQUAL "neverd-llvm-macos-arm64")
      set(_sha "${_NEVERD_LLVM_PIN_MACOS_ARM64}")
    elseif(pkg STREQUAL "neverd-llvm-windows-x64")
      set(_sha "${_NEVERD_LLVM_PIN_WINDOWS_X64}")
    endif()
  endif()
  string(TOLOWER "${_sha}" _sha)
  set(${out_sha} "${_sha}" PARENT_SCOPE)
endfunction()

# The digest of the archive an extracted tree came from.  Caches written before
# the stamp existed fall back to hashing that archive, so an install that is
# already current is not made to download the same bytes again to prove it.
function(_neverd_cached_llvm_sha256 out_sha stamp_path archive_path)
  set(_sha "")
  if(EXISTS "${stamp_path}")
    file(READ "${stamp_path}" _sha)
    string(STRIP "${_sha}" _sha)
  elseif(EXISTS "${archive_path}")
    file(SHA256 "${archive_path}" _sha)
  endif()
  string(TOLOWER "${_sha}" _sha)
  set(${out_sha} "${_sha}" PARENT_SCOPE)
endfunction()

function(neverd_fetch_prebuilt_llvm)
  _neverd_resolve_prebuilt_llvm_package(
    _platform _arch _pkg _archive_extension)

  set(_tag "${NEVERD_LLVM_PREBUILT_TAG}")
  set(_root "${NEVERD_LLVM_PREBUILT_CACHE_DIR}/${_tag}/${_arch}")
  set(_prefix "${_root}/${_pkg}")
  set(_cfg "${_prefix}/lib/cmake/llvm/LLVMConfig.cmake")
  set(_archive_name "${_pkg}.${_archive_extension}")
  set(_archive_path "${_root}/${_archive_name}")
  set(_stamp_path "${_root}/${_archive_name}.stamp")

  # Knowing the digest before reaching the network is what lets a cache hit be
  # checked without one, and what turns a republished tag into a mismatch the
  # configure reports rather than a stale package it silently keeps using.
  if(NEVERD_LLVM_PREBUILT_SHA256)
    string(STRIP "${NEVERD_LLVM_PREBUILT_SHA256}" _expected)
    string(TOLOWER "${_expected}" _expected)
    set(_expected_source "NEVERD_LLVM_PREBUILT_SHA256")
  else()
    _neverd_pinned_llvm_sha256(_expected "${_pkg}")
    set(_expected_source "the digest pinned in cmake/NeverDLLVMPrebuilt.cmake")
  endif()

  set(_reuse_cache OFF)
  if(EXISTS "${_cfg}")
    if(_expected)
      _neverd_cached_llvm_sha256(_cached "${_stamp_path}" "${_archive_path}")
      if(_cached STREQUAL "${_expected}")
        set(_reuse_cache ON)
      elseif(_cached)
        message(STATUS
          "NeverD prebuilt LLVM: cached ${_archive_name} is ${_cached} but "
          "${_expected} is expected; refetching")
      else()
        message(STATUS
          "NeverD prebuilt LLVM: cached ${_archive_name} records no digest; "
          "refetching")
      endif()
    else()
      set(_reuse_cache ON)
    endif()
  endif()

  if(_reuse_cache)
    message(STATUS "NeverD prebuilt LLVM: reusing cached ${_prefix}")
    # Recording what a pre-stamp cache turned out to hold keeps the next
    # configure from rehashing the archive, and keeps the tree identifiable
    # once that archive is deleted to reclaim the space.  Nothing is recorded
    # for a tag with no expected digest, because then the reuse above was an
    # assumption rather than a check.
    if(_expected AND NOT EXISTS "${_stamp_path}")
      file(WRITE "${_stamp_path}" "${_expected}\n")
    endif()
  else()
    if(NEVERD_LLVM_PREBUILT_BASE_URL)
      set(_base "${NEVERD_LLVM_PREBUILT_BASE_URL}")
    else()
      set(_base "https://github.com/${NEVERD_LLVM_PREBUILT_REPO}/releases/download/${_tag}")
    endif()

    set(_archive_url "${_base}/${_archive_name}")
    set(_sha_url "${_archive_url}.sha256")

    file(MAKE_DIRECTORY "${_root}")

    message(STATUS "NeverD prebuilt LLVM: downloading ${_archive_url}")
    file(DOWNLOAD "${_archive_url}" "${_archive_path}"
      SHOW_PROGRESS STATUS _download_status TLS_VERIFY ON)
    list(GET _download_status 0 _download_code)
    if(NOT _download_code EQUAL 0)
      list(GET _download_status 1 _download_message)
      file(REMOVE "${_archive_path}")
      message(FATAL_ERROR
        "Failed to download ${_archive_url}: ${_download_message}\n"
        "Check NEVERD_LLVM_PREBUILT_TAG (='${_tag}') and that the asset exists.")
    endif()

    if(NOT _expected)
      set(_sha_file "${_root}/${_archive_name}.sha256")
      file(DOWNLOAD "${_sha_url}" "${_sha_file}"
        STATUS _checksum_download_status TLS_VERIFY ON)
      list(GET _checksum_download_status 0 _checksum_download_code)
      if(NOT _checksum_download_code EQUAL 0)
        file(REMOVE "${_archive_path}")
        message(FATAL_ERROR
          "Downloaded the archive but failed to fetch its checksum ${_sha_url}. "
          "Pin it manually with -DNEVERD_LLVM_PREBUILT_SHA256=<hash>.")
      endif()
      file(READ "${_sha_file}" _sha_contents)
      string(STRIP "${_sha_contents}" _sha_contents)
      string(REGEX MATCH "^[0-9a-fA-F]+" _expected "${_sha_contents}")
      set(_expected_source "the checksum published beside the archive")
    endif()

    string(TOLOWER "${_expected}" _expected)
    string(LENGTH "${_expected}" _expected_length)
    if(NOT _expected_length EQUAL 64 OR NOT _expected MATCHES "^[0-9a-f]+$")
      file(REMOVE "${_archive_path}")
      message(FATAL_ERROR
        "Invalid SHA256 for ${_archive_name}: expected 64 hexadecimal "
        "characters, got '${_expected}'.")
    endif()

    file(SHA256 "${_archive_path}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(NOT _actual STREQUAL _expected)
      file(REMOVE "${_archive_path}")
      message(FATAL_ERROR
        "SHA256 mismatch for ${_archive_name}\n"
        "  expected: ${_expected}\n  actual:   ${_actual}\n"
        "The expectation came from ${_expected_source}. Release ${_tag} is "
        "republished in place, so a package rebuilt after that digest was "
        "recorded lands here: refresh the pins in "
        "cmake/NeverDLLVMPrebuilt.cmake to ${_actual} if this archive is the "
        "one NeverD should build against.")
    endif()
    message(STATUS "NeverD prebuilt LLVM: checksum OK (${_actual})")

    file(REMOVE_RECURSE "${_prefix}")
    message(STATUS "NeverD prebuilt LLVM: extracting into ${_root}")
    file(ARCHIVE_EXTRACT INPUT "${_archive_path}" DESTINATION "${_root}")

    if(NOT EXISTS "${_cfg}")
      message(FATAL_ERROR
        "Extraction did not produce ${_cfg}; the package layout may have changed.")
    endif()

    file(WRITE "${_stamp_path}" "${_actual}\n")
  endif()

  set(LLVM_DIR "${_prefix}/lib/cmake/llvm"
      CACHE PATH "Set by NEVERD_LLVM_PREBUILT" FORCE)
  set(LLVM_DIR "${_prefix}/lib/cmake/llvm" PARENT_SCOPE)
  message(STATUS "NeverD prebuilt LLVM: LLVM_DIR=${_prefix}/lib/cmake/llvm")
endfunction()
