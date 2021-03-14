function(get_version file_name version_prefix)
  file(STRINGS ${file_name} version_lines REGEX "${version_prefix}VERSION_(MAJOR|MINOR|PATCH)")
  foreach(line ${version_lines})
    if("${line}" MATCHES "(${version_prefix}VERSION_(MAJOR|MINOR|PATCH))[\t ]+([0-9]+)")
      set(${CMAKE_MATCH_1} ${CMAKE_MATCH_3} PARENT_SCOPE)
    endif()
  endforeach()
endfunction()
