# Used after deployment too, so packaging doesn't replace a certificate-backed
# signature with an ad-hoc signature.
include("${CMAKE_CURRENT_LIST_DIR}/MacOSCodeSigning.cmake")
if(NOT DEFINED DISQUISITION_APP_BUNDLE OR DISQUISITION_APP_BUNDLE STREQUAL "")
  message(FATAL_ERROR "DISQUISITION_APP_BUNDLE is required")
endif()
if(NOT DEFINED DISQUISITION_CODESIGN_IDENTITY)
  set(DISQUISITION_CODESIGN_IDENTITY "AUTO")
endif()
disquisition_resolve_codesign_identity("${DISQUISITION_CODESIGN_IDENTITY}" identity)
execute_process(COMMAND codesign --force --deep --sign "${identity}" "${DISQUISITION_APP_BUNDLE}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Signing Disquisition failed with exit code ${result}")
endif()
execute_process(COMMAND codesign --verify --deep --strict "${DISQUISITION_APP_BUNDLE}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Disquisition's signature failed verification")
endif()
