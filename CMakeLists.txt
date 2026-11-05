cmake_minimum_required(VERSION 3.10)
project(git-lfs-c VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)

# Всё, кроме main.c, — в библиотеку: её же линкуют unit-тесты
add_library(lfscore STATIC
  src/util.c
  src/run.c
  src/json.c
  src/pktline.c
  src/pointer.c
  src/git.c
  src/store.c
  src/http.c
  src/endpoint.c
  src/transfer.c
  src/scan.c
  src/pathfilter.c
  src/cmd_filter.c
  src/cmd_repo.c
  src/cmd_transfer.c
  src/cmd_info.c
  src/cmd_lock.c
  src/migrate.c
)
target_compile_definitions(lfscore PUBLIC
  _GNU_SOURCE
  LFS_VERSION="${PROJECT_VERSION}"
)
target_include_directories(lfscore PUBLIC src)
target_link_libraries(lfscore PUBLIC PkgConfig::CURL PkgConfig::CRYPTO Threads::Threads)

add_executable(git-lfs src/main.c)
target_link_libraries(git-lfs PRIVATE lfscore)

add_executable(unit-tests tests/unit.c)
target_link_libraries(unit-tests PRIVATE lfscore)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|LCC")
  target_compile_options(lfscore PRIVATE -Wall -Wextra)
  target_compile_options(git-lfs PRIVATE -Wall -Wextra)
  target_compile_options(unit-tests PRIVATE -Wall -Wextra)
endif()

install(TARGETS git-lfs RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME unit COMMAND unit-tests)
add_test(NAME integration
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh $<TARGET_FILE:git-lfs>)
