# QUIC (Microsoft's MsQuic, MIT; its OpenSSL build): on when scripts/fetch-msquic.ps1 has fetched
# the headers and Windows x64 msquic.dll into external/msquic. msquic.dll is loaded at run time
# from bin/net/ (engine/net/quic_transport.cpp), so a build or a machine without it still runs,
# without quic:// addresses. Its LICENSE and THIRD-PARTY-NOTICES ship next to it.

set(FX_MSQUIC_ROOT "${CMAKE_SOURCE_DIR}/external/msquic")
set(fx_msquic_found OFF)
if(EXISTS "${FX_MSQUIC_ROOT}/include/msquic.h" AND EXISTS "${FX_MSQUIC_ROOT}/bin/msquic.dll")
  set(fx_msquic_found ON)
endif()
option(FALCON_QUIC "QUIC networking through MsQuic (needs scripts/fetch-msquic.ps1)" ${fx_msquic_found})
if(FALCON_QUIC AND NOT fx_msquic_found)
  message(WARNING "FALCON_QUIC: external/msquic is incomplete (run scripts/fetch-msquic.ps1); QUIC is off")
  set(FALCON_QUIC OFF CACHE BOOL "" FORCE)
endif()
message(STATUS "Falcon: QUIC ${FALCON_QUIC}")

if(FALCON_QUIC)
  set(FX_MSQUIC_STAGE_DIR "${CMAKE_BINARY_DIR}/bin/net")
  add_custom_target(falcon_msquic_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FX_MSQUIC_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FX_MSQUIC_ROOT}/bin/msquic.dll"
      "${FX_MSQUIC_ROOT}/LICENSE"
      "${FX_MSQUIC_ROOT}/THIRD-PARTY-NOTICES"
      "${FX_MSQUIC_STAGE_DIR}"
    VERBATIM)
endif()
