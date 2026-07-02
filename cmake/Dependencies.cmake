# Dependency acquisition: prefer a system/find_package copy, otherwise fetch
# and build as part of this build (HighLevelDesign.md §11.1). Pinned tags:
#   tl-expected v1.1.0, googletest v1.14.0
include(FetchContent)

# ---- tl::expected (Result backend, header-only; the only core dependency) -----
find_package(tl-expected QUIET)
if(NOT TARGET tl::expected)
  if(CONFIGMANAGER_USE_SYSTEM_DEPS)
    message(FATAL_ERROR
      "CONFIGMANAGER_USE_SYSTEM_DEPS is ON but tl-expected was not found "
      "via find_package")
  endif()
  set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(tl-expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG        v1.1.0)
  FetchContent_MakeAvailable(tl-expected)
endif()

# ---- GoogleTest (tests only) ----------------------------------------------------
if(CONFIGMANAGER_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT TARGET GTest::gtest_main)
    if(CONFIGMANAGER_USE_SYSTEM_DEPS)
      message(FATAL_ERROR
        "CONFIGMANAGER_USE_SYSTEM_DEPS is ON but GTest was not found "
        "via find_package")
    endif()
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
