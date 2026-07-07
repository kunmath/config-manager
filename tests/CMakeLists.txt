add_executable(configmanager_tests
  config_model_test.cpp
  config_node_test.cpp
  config_path_test.cpp
  config_runtime_test.cpp
  config_value_test.cpp
  migration_test.cpp
  result_test.cpp
  version_catalog_test.cpp)

target_link_libraries(configmanager_tests PRIVATE
  configmanager::core
  GTest::gtest_main)

if(CONFIGMANAGER_BUILD_JSON)
  target_sources(configmanager_tests PRIVATE json_interface_test.cpp)
  target_link_libraries(configmanager_tests PRIVATE configmanager::json)
endif()

if(CONFIGMANAGER_BUILD_XML)
  target_sources(configmanager_tests PRIVATE xml_interface_test.cpp)
  target_link_libraries(configmanager_tests PRIVATE configmanager::xml)
endif()

include(GoogleTest)
gtest_discover_tests(configmanager_tests)
