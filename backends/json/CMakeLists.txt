# JSON serialization backend (docs/HighLevelDesign.md §6.1): configmanager::json.
include(GNUInstallDirs)

add_library(configmanager_json src/json_interface.cpp)
add_library(configmanager::json ALIAS configmanager_json)
# Installed consumers see the same configmanager::json name as the build tree.
set_target_properties(configmanager_json PROPERTIES EXPORT_NAME json)

target_include_directories(configmanager_json PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
# nlohmann/json is header-only and referenced from the .cpp only; the
# BUILD_INTERFACE wrapper keeps it out of the exported link interface so a
# downstream find_package(ConfigManager) does not need nlohmann installed.
target_link_libraries(configmanager_json
  PUBLIC configmanager::core
  PRIVATE $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)
target_compile_features(configmanager_json PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(configmanager_json PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS configmanager_json
  EXPORT ConfigManagerTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
