# AMD FSR (FSR 4 / FSR 3.1 through AMD's FSR API, Direct3D 12): on when scripts/fetch-fsr.ps1 has
# fetched the AMD FSR SDK pieces into external/fidelityfx. The signed DLLs are loaded at runtime
# from bin/fsr/ (engine/render/fsr_upscaler.cpp), so a build or a machine without them still runs
# and upscales with TAAU. license.md (AMD's) ships next to them.

set(FX_FSR_ROOT "${CMAKE_SOURCE_DIR}/external/fidelityfx")
set(fx_fsr_found OFF)
if(EXISTS "${FX_FSR_ROOT}/bin/amd_fidelityfx_loader_dx12.dll" AND
   EXISTS "${FX_FSR_ROOT}/bin/amd_fidelityfx_upscaler_dx12.dll" AND
   EXISTS "${FX_FSR_ROOT}/include/upscalers/include/ffx_upscale.h")
  set(fx_fsr_found ON)
endif()
option(FALCON_FSR "AMD FSR upscaling on Direct3D 12 (needs scripts/fetch-fsr.ps1)" ${fx_fsr_found})
if(FALCON_FSR AND NOT fx_fsr_found)
  message(WARNING "FALCON_FSR: external/fidelityfx is incomplete (run scripts/fetch-fsr.ps1); FSR is off")
  set(FALCON_FSR OFF CACHE BOOL "" FORCE)
endif()
message(STATUS "Falcon: AMD FSR ${FALCON_FSR}")

if(FALCON_FSR)
  set(FX_FSR_STAGE_DIR "${CMAKE_BINARY_DIR}/bin/fsr")
  add_custom_target(falcon_fsr_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FX_FSR_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FX_FSR_ROOT}/bin/amd_fidelityfx_loader_dx12.dll"
      "${FX_FSR_ROOT}/bin/amd_fidelityfx_upscaler_dx12.dll"
      "${FX_FSR_ROOT}/license.md"
      "${FX_FSR_STAGE_DIR}"
    VERBATIM)
endif()
