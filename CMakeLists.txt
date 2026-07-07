cmake_minimum_required(VERSION 3.21)

project(ConfigManager
  VERSION 0.1.0
  DESCRIPTION "Configuration versioning, migration, and synchronization library"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CONFIGMANAGER_USE_SYSTEM_DEPS
  "Resolve all dependencies via find_package only (no network fetch)" OFF)
option(CONFIGMANAGER_BUILD_TESTS "Build ConfigManager unit tests" ON)
option(CONFIGMANAGER_BUILD_JSON
  "Build the JSON serialization backend (configmanager::json)" ON)
option(CONFIGMANAGER_BUILD_XML
  "Build the XML serialization backend (configmanager::xml)" ON)
option(CONFIGMANAGER_BUILD_EXAMPLES "Build ConfigManager example programs" ON)

include(cmake/Dependencies.cmake)

# ---- Core library (no serialization backends) --------------------------------
add_library(configmanager_core
  src/config_model.cpp
  src/config_node.cpp
  src/config_path.cpp
  src/config_runtime.cpp
  src/config_value.cpp
  src/migration_engine.cpp
  src/migration_registry.cpp
  src/version_catalog.cpp)
add_library(configmanager::core ALIAS configmanager_core)
# Installed consumers see the same configmanager::core name as the build tree.
set_target_properties(configmanager_core PROPERTIES EXPORT_NAME core)

target_include_directories(configmanager_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(configmanager_core PUBLIC tl::expected)
target_compile_features(configmanager_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(configmanager_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- Serialization backends (opt-in targets) -----------------------------------
if(CONFIGMANAGER_BUILD_JSON)
  add_subdirectory(backends/json)
endif()
if(CONFIGMANAGER_BUILD_XML)
  add_subdirectory(backends/xml)
endif()

# ---- Tests & examples (both register with CTest) -------------------------------
if(CONFIGMANAGER_BUILD_TESTS OR CONFIGMANAGER_BUILD_EXAMPLES)
  enable_testing()
endif()
if(CONFIGMANAGER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(CONFIGMANAGER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---- Installation / package export ---------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS configmanager_core
  EXPORT ConfigManagerTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ConfigManagerTargets
  NAMESPACE configmanager::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ConfigManager)

# The XML backend is self-contained when pugixml was fetched (compiled in
# header-only, pugixml_SOURCE_DIR set); against a system pugixml it is a real
# link dependency the installed package must resolve downstream.
if(CONFIGMANAGER_BUILD_XML AND NOT pugixml_SOURCE_DIR)
  set(CONFIGMANAGER_FIND_PUGIXML TRUE)
else()
  set(CONFIGMANAGER_FIND_PUGIXML FALSE)
endif()

configure_package_config_file(
  cmake/ConfigManagerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ConfigManagerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ConfigManager)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ConfigManagerConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ConfigManagerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ConfigManagerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ConfigManager)
