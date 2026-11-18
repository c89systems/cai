# The released c89.systems codec owns CBOR grammar and UTF-8 validation.
# CAI uses its public types and operations directly.
set(CAI_CBOR_VERSION "0.1.0")
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  set(_cai_cbor_targets
    x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl
    armhf-linux-gnu armhf-linux-musl arm64-apple-darwin)
  set(_cai_cbor_checksums
    14726a029c310a5a755006c488cd63e6648c1125b61e03a875d08da79e27f666
    91e4499a7a4bffe2e9d738e872bc517cdb17c3835cd5d02b9001fd50f2c3770c
    7ed7a6b3bca9dbe2798950980ac819094e74d841edb9bf058596e78a686d25dd
    54ac5539044902047ace7e3b1b4e54304c15d585d2a3f37998212e225fa9aa00
    bcfcdf1eb2dff737a4ab023a35905a282bc07714ab493d4c8cf3bed66b305058
    3b9eb5a11b52338a76ed4ae72c74798ba20e157bc869eab63956b9de7fd844f4
    8d58ff45f12cf2501763bbb809f7394d426d0dfc1bca169a67b4b971a878943b)
  list(FIND _cai_cbor_targets "${CAI_TARGET_ID}" _cai_cbor_index)
  if(_cai_cbor_index LESS 0)
    message(FATAL_ERROR "No pinned libcbor SDK for ${CAI_TARGET_ID}")
  endif()
  list(GET _cai_cbor_checksums ${_cai_cbor_index} CAI_CBOR_SHA256)
  set(CAI_CBOR_ARCHIVE_NAME "libcbor-${CAI_CBOR_VERSION}-${CAI_TARGET_ID}.tar.gz")
  set(CAI_CBOR_URL "https://github.com/c89systems/libcbor/releases/download/v${CAI_CBOR_VERSION}/${CAI_CBOR_ARCHIVE_NAME}")
  set(CAI_CBOR_PREFIX "${CAI_DEPS_DIR}/libcbor-${CAI_CBOR_VERSION}-${CAI_TARGET_ID}")
  cai_dependency_archive_path("${CAI_CBOR_ARCHIVE_NAME}" "${CAI_CBOR_SHA256}" CAI_CBOR_ARCHIVE)
  cai_download_tarball("libcbor ${CAI_CBOR_VERSION} ${CAI_TARGET_ID}"
    "${CAI_CBOR_URL}" "${CAI_CBOR_ARCHIVE}" "${CAI_CBOR_PREFIX}"
    "${CAI_CBOR_SHA256}" "${CAI_CBOR_PREFIX}/include/c89cbor/cbor.h")
  list(PREPEND CMAKE_PREFIX_PATH "${CAI_CBOR_PREFIX}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${CAI_CBOR_PREFIX}")
  set(C89Cbor_DIR "${CAI_CBOR_PREFIX}/lib/cmake/C89Cbor"
    CACHE PATH "c89.systems libcbor package directory" FORCE)
else()
  set(CAI_CBOR_URL "host")
  set(CAI_CBOR_SHA256 "host")
  set(CAI_CBOR_ARCHIVE_NAME "host")
endif()
find_package(C89Cbor ${CAI_CBOR_VERSION} EXACT CONFIG REQUIRED)
