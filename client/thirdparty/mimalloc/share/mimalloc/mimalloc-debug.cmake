#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "mimalloc" for configuration "Debug"
set_property(TARGET mimalloc APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(mimalloc PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/debug/lib/mimalloc-debug.dll.lib"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/bin/mimalloc-debug.dll"
  )

list(APPEND _cmake_import_check_targets mimalloc )
list(APPEND _cmake_import_check_files_for_mimalloc "${_IMPORT_PREFIX}/debug/lib/mimalloc-debug.dll.lib" "${_IMPORT_PREFIX}/debug/bin/mimalloc-debug.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
