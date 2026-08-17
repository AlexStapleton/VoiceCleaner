# Copy ${SRC} to ${DST} only when ${DST} does not already exist — never clobbers a user's edited
# settings.toml on rebuild. Invoked via `cmake -DSRC=.. -DDST=.. -P copy_if_absent.cmake`.
if(NOT EXISTS "${DST}")
  configure_file("${SRC}" "${DST}" COPYONLY)
  message(STATUS "copy_if_absent: wrote ${DST}")
else()
  message(STATUS "copy_if_absent: kept existing ${DST}")
endif()
