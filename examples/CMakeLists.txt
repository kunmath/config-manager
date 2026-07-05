# Example programs. Each is a self-contained, heavily commented consumer of
# the public API, registered with CTest (prefix `examples.`) so it is executed
# — not just compiled — by the test suite and by CI.

function(configmanager_add_example name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE configmanager::core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  # Examples resolve their data/output paths relative to the working
  # directory, so pin it to this build dir (where data/ is copied below).
  add_test(NAME examples.${name}
    COMMAND ${name}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

configmanager_add_example(01_basic_model)
configmanager_add_example(02_versioning_and_migration)

# The capstone exercises the serialization boundary, so it exists only when
# the JSON backend is built; without it the other examples still work.
if(CONFIGMANAGER_BUILD_JSON)
  configmanager_add_example(03_relaygate_lifecycle)
  target_link_libraries(03_relaygate_lifecycle PRIVATE configmanager::json)
  # configure_file (unlike file(COPY)) re-copies whenever the source changes.
  configure_file(data/relaygate_v1.json data/relaygate_v1.json COPYONLY)
  configure_file(data/relaygate_future.json data/relaygate_future.json COPYONLY)
endif()

# 04 showcases the XML wire mapping, so it exists only when the XML backend
# is built.
if(CONFIGMANAGER_BUILD_XML)
  configmanager_add_example(04_sensorbridge_xml)
  target_link_libraries(04_sensorbridge_xml PRIVATE configmanager::xml)
  configure_file(data/sensorbridge_v1.xml data/sensorbridge_v1.xml COPYONLY)
endif()
