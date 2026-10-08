include("${CMAKE_CURRENT_LIST_DIR}/../cmake/MacOSCodeSigning.cmake")

function(expect_identity requested available expected)
  disquisition_select_codesign_identity("${requested}" "${available}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Expected '${expected}', got '${actual}' for '${requested}'")
  endif()
endfunction()

# An unsigned CI runner must remain buildable. Non-development identities
# must not be selected automatically, even when they precede a valid one.
expect_identity("AUTO" "  0 valid identities found" "-")
set(identities [=[
  1) 1111111111111111111111111111111111111111 "Developer ID Application: Example (TEAM)"
  2) ABCDEF1234567890ABCDEF1234567890ABCDEF12 "Apple Development: Example (TEAM)"
  3) 2222222222222222222222222222222222222222 "Apple Development: Other (OTHER)"
     3 valid identities found
]=])
expect_identity("AUTO" "${identities}" "ABCDEF1234567890ABCDEF1234567890ABCDEF12")
expect_identity("AUTO" "  1) 1111111111111111111111111111111111111111 \"Developer ID Application: Example (TEAM)\"" "-")
# Explicit names, fingerprints and ad-hoc overrides must survive selection.
expect_identity("-" "${identities}" "-")
expect_identity("Apple Development: Other (OTHER)" "${identities}" "Apple Development: Other (OTHER)")
expect_identity("2222222222222222222222222222222222222222" "" "2222222222222222222222222222222222222222")
