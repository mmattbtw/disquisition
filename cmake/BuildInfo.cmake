# Writes the git branch and commit the app was built from. Runs on every build
# so a rebuild after switching branches or committing never shows stale values,
# and only rewrites the header when they change.
#
# Inputs: SOURCE_DIR, OUTPUT, and optionally GIT_BRANCH / GIT_COMMIT, which CI
# sets because pull request checkouts are a detached merge commit.

function(git_value out)
  execute_process(
    COMMAND git ${ARGN}
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE value
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(result EQUAL 0)
    set(${out} "${value}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

if(GIT_BRANCH STREQUAL "")
  git_value(GIT_BRANCH rev-parse --abbrev-ref HEAD)
  # A detached checkout reports "HEAD", which says nothing about the branch.
  if(GIT_BRANCH STREQUAL "HEAD")
    set(GIT_BRANCH "detached")
  endif()
endif()
if(GIT_COMMIT STREQUAL "")
  git_value(GIT_COMMIT rev-parse HEAD)
endif()
foreach(name IN ITEMS GIT_BRANCH GIT_COMMIT)
  if(${name} STREQUAL "")
    set(${name} "unknown")
  endif()
  # Branch names may not contain these, but keep the C++ literal valid anyway.
  string(REPLACE "\\" "\\\\" ${name} "${${name}}")
  string(REPLACE "\"" "\\\"" ${name} "${${name}}")
endforeach()

file(CONFIGURE OUTPUT "${OUTPUT}" @ONLY CONTENT [[
#pragma once

namespace disquisition {
inline constexpr char kGitBranch[] = "@GIT_BRANCH@";
inline constexpr char kGitCommit[] = "@GIT_COMMIT@";
}  // namespace disquisition
]])
