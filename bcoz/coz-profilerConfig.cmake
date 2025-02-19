

set(COZDIR "/usr")

set(COZ_BIN ${COZDIR}/bin/coz)
set(COZ_INCLUDE_DIR ${COZDIR}/include)
get_filename_component(LIBRARY_DIR ${CMAKE_CURRENT_LIST_FILE} DIRECTORY)
set(COZ_LIBRARY ${LIBRARY_DIR}/libcoz.so)

set(COZ_FOUND ON)

message(INFO " ${COZ_INCLUDE_DIR} ${COZ_LIBRARY} ${COZ_FOUND}")

mark_as_advanced(COZ_FOUND COZ_INCLUDE_DIR COZ_LIBRARY)

add_library(coz::coz UNKNOWN IMPORTED)
set_target_properties(coz::coz PROPERTIES 
    INTERFACE_INCLUDE_DIRECTORIES "${COZ_INCLUDE_DIR}")

set_target_properties(coz::coz PROPERTIES 
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${COZ_INCLUDE_DIR}")

add_executable(coz::profiler IMPORTED)

set_property(TARGET coz::profiler PROPERTY IMPORTED_LOCATION ${COZ_BIN})
