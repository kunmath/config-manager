# XML serialization backend (docs/HighLevelDesign.md §6.1): configmanager::xml.
include(GNUInstallDirs)

add_library(configmanager_xml src/xml_interface.cpp)
add_library(configmanager::xml ALIAS configmanager_xml)
# Installed consumers see the same configmanager::xml name as the build tree.
set_target_properties(configmanager_xml PROPERTIES EXPORT_NAME xml)

target_include_directories(configmanager_xml PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(configmanager_xml PUBLIC configmanager::core)
target_compile_features(configmanager_xml PUBLIC cxx_std_20)

# When pugixml is fetched, its "header + small src" pair is compiled into
# this target via PUGIXML_HEADER_ONLY, so the installed archive is
# self-contained and the exported package config needs no pugixml
# find_dependency (same rationale as the JSON backend's nlohmann handling).
# A system copy found by find_package ships no sources to inline, so it is a
# real link dependency: the static archive contains unresolved pugixml
# symbols, so the PRIVATE link must reach the export (CMake emits it as
# $<LINK_ONLY:pugixml::pugixml>) and the package config must find_dependency
# it (see the root CMakeLists).
if(pugixml_SOURCE_DIR)
  target_compile_definitions(configmanager_xml PRIVATE PUGIXML_HEADER_ONLY)
  target_include_directories(configmanager_xml PRIVATE
    ${pugixml_SOURCE_DIR}/src)
else()
  target_link_libraries(configmanager_xml PRIVATE pugixml::pugixml)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(configmanager_xml PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS configmanager_xml
  EXPORT ConfigManagerTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
