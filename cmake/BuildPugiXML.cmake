include(ExternalProject)

ExternalProject_Add(
  pugixml_build
  GIT_REPOSITORY git@github.com:zeux/pugixml.git
  GIT_TAG v1.13
  GIT_SHALLOW TRUE
  CMAKE_GENERATOR ${CMAKE_GENERATOR}
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DBUILD_SHARED_LIBS=OFF -DPUGIXML_BUILD_TESTS=OFF)

ExternalProject_Get_Property(pugixml_build INSTALL_DIR)

message(STATUS "pugixml will be installed to ${INSTALL_DIR}")

# find the library
set(pugixml_lib_filename ${CMAKE_STATIC_LIBRARY_PREFIX}pugixml${CMAKE_STATIC_LIBRARY_SUFFIX})
set(pugixml_lib_location ${INSTALL_DIR}/lib/${pugixml_lib_filename})

message(STATUS "pugixml library expected at ${pugixml_lib_location}")

add_library(pugixml STATIC IMPORTED)
set_target_properties(
  pugixml
  PROPERTIES IMPORTED_LOCATION ${pugixml_lib_location})

add_library(libpugixml INTERFACE)
add_dependencies(libpugixml pugixml_build)
target_link_libraries(libpugixml INTERFACE pugixml)
set_target_properties(libpugixml PROPERTIES INTERFACE_INCLUDE_DIRECTORIES  ${INSTALL_DIR}/include)