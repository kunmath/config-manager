include(GNUInstallDirs)

add_library(configmanager_checkpoint src/checkpoint_store.cpp)
add_library(configmanager::checkpoint ALIAS configmanager_checkpoint)
set_target_properties(configmanager_checkpoint PROPERTIES EXPORT_NAME checkpoint)

target_include_directories(configmanager_checkpoint PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(configmanager_checkpoint PUBLIC configmanager::core)
target_compile_features(configmanager_checkpoint PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(configmanager_checkpoint PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

install(TARGETS configmanager_checkpoint
  EXPORT ConfigManagerTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
