# cmake -P tools/mgba_cmake_diag.cmake
# Replicates mGBA's LIBMGBA_ONLY check to see what get_directory_property returns.

get_directory_property(_excl EXCLUDE_FROM_ALL)
message(STATUS "cmake version: ${CMAKE_VERSION}")
message(STATUS "EXCLUDE_FROM_ALL raw: '${_excl}'")

if(NOT DEFINED _exc2)
    get_directory_property(_exc2 EXCLUDE_FROM_ALL)
endif()
if(NOT _exc2)
    message(STATUS "LIBMGBA_ONLY would be FALSY -> normal build, BUILD_SDL=ON expected")
else()
    message(STATUS "LIBMGBA_ONLY would be TRUTHY='${_exc2}' -> DISABLE_FRONTENDS=ON, BUILD_SDL=OFF!")
endif()
