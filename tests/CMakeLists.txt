add_executable(configmanager_tests
  config_path_test.cpp
  config_value_test.cpp
  result_test.cpp)

target_link_libraries(configmanager_tests PRIVATE
  configmanager::core
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(configmanager_tests)
