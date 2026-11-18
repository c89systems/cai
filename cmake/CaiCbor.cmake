# The released c89.systems codec owns CBOR grammar and UTF-8 validation.
# CAI uses its public types and operations directly.
set(CAI_CBOR_VERSION "0.2.0")
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  set(_cai_cbor_targets
    x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl
    armhf-linux-gnu armhf-linux-musl arm64-apple-darwin)
  set(_cai_cbor_checksums
    136ba69b0765654cc0fbf7e42db2128f45b1dfe9b6fa5ca6b6307a0092281b74
    d671237d7fa13fea23e2dd9e55fca20d0cfc43890bc37e0113e5ccd5f0c90780
    840e216b2ecd07edaed31e8af05205cbd0b7d2016095354dbb075a82a2a3534c
    eaf93027ef05df7efafd03837b6c1db787eada66ac7672485c7a0469ea885128
    4276041efdc4d94e7d0de58ccb53497282ce7be4ca3e15e900b24e236c573618
    a5403cf3ee5072e017c8f8198f11320ea5e2cbd92856da7fd67c88a851209544
    8c77f1274e5d9c5aa7b29d6c823aa0531166d09cd0d6837c322cfbfc4b2bf2a1)
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
