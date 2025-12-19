#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "yaml" for configuration "RelWithDebInfo"
set_property(TARGET yaml APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(yaml PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "C"
  IMPORTED_LINK_INTERFACE_LIBRARIES_RELWITHDEBINFO "/usr/lib/x86_64-linux-gnu/libicui18n.so;/usr/lib/x86_64-linux-gnu/libicuuc.so;/usr/lib/x86_64-linux-gnu/libicudata.so"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/libyaml_static.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS yaml )
list(APPEND _IMPORT_CHECK_FILES_FOR_yaml "${_IMPORT_PREFIX}/lib/libyaml_static.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
