# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "CMakeFiles/mxprog_qt_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/mxprog_qt_autogen.dir/ParseCache.txt"
  "mxprog_qt_autogen"
  )
endif()
