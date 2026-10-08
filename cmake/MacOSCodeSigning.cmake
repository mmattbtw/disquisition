# Ad-hoc signatures identify each build by its cdhash. TCC grants therefore
# stop matching after a rebuild, even if System Settings still shows access.
# Prefer a certificate-backed identity for local builds. Keep CI usable when
# no certificate is installed, and honor an explicitly selected identity.
function(disquisition_select_codesign_identity requested available out)
  if(NOT requested STREQUAL "AUTO")
    set(${out} "${requested}" PARENT_SCOPE)
    return()
  endif()

  string(REGEX MATCH "[0-9A-Fa-f]+ \"Apple Development:[^\"\n]+\"" development "${available}")
  if(development)
    string(REGEX MATCH "^[0-9A-Fa-f]+" identity "${development}")
  else()
    set(identity "-")
  endif()
  set(${out} "${identity}" PARENT_SCOPE)
endfunction()

function(disquisition_resolve_codesign_identity requested out)
  if(requested STREQUAL "AUTO")
    execute_process(COMMAND security find-identity -v -p codesigning
      RESULT_VARIABLE result OUTPUT_VARIABLE available ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
      message(WARNING "Could not list macOS signing identities: ${error}")
      set(available "")
    endif()
  endif()
  disquisition_select_codesign_identity("${requested}" "${available}" identity)
  if(identity STREQUAL "-")
    message(WARNING
      "Disquisition uses ad-hoc signing. Screen recording and microphone grants "
      "may need to be renewed after each rebuild or artifact update. Install an "
      "Apple Development certificate or set DISQUISITION_CODESIGN_IDENTITY to "
      "a certificate identity to keep permissions across builds.")
  else()
    message(STATUS "Disquisition macOS signing identity: ${identity}")
  endif()
  set(${out} "${identity}" PARENT_SCOPE)
endfunction()
