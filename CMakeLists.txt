cmake_minimum_required(VERSION 3.20)
project(harvester VERSION 0.1.0 LANGUAGES CXX)

# Order-flow / toxicity-aware market making on CME futures: the Databento
# MDP 3.0 book, Avellaneda-Stoikov quotes and IBKR execution of
# CME-Futures-Spread-Harvester, in C++ for the live path.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#
# Options:
#   HARVESTER_WITH_DATABENTO  the live feed, DBN replay and fetch (databento-cpp)
#   HARVESTER_WITH_IBKR       the IBKR router (needs TWS_API_DIR)
#   HARVESTER_BUILD_TESTS     the doctest suite
#   HARVESTER_BUILD_BENCH     the latency benchmark

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HARVESTER_WITH_DATABENTO "Build the Databento live feed, DBN replay and fetch" ON)
option(HARVESTER_FETCH_DATABENTO "Fetch databento-cpp with FetchContent when it is not installed" ON)
option(HARVESTER_WITH_IBKR "Build the IBKR TWS API router" OFF)
set(TWS_API_DIR "" CACHE PATH "IBKR TWS API C++ client sources (IBJts/source/cppclient/client)")
option(HARVESTER_BUILD_TESTS "Build the test suite" ON)
option(HARVESTER_BUILD_BENCH "Build the latency benchmark" ON)
option(HARVESTER_NATIVE "Tune for the build machine's CPU (-march=native)" OFF)

include(FetchContent)
find_package(Threads REQUIRED)

# -- yaml-cpp ---------------------------------------------------------------
find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
  message(STATUS "yaml-cpp not installed; fetching 0.8.0")
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(yaml-cpp GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git GIT_TAG 0.8.0)
  FetchContent_MakeAvailable(yaml-cpp)
endif()
if(TARGET yaml-cpp::yaml-cpp)
  set(HARVESTER_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(HARVESTER_YAML_TARGET yaml-cpp)
endif()

# -- the core library ----------------------------------------------------------
add_library(harvester_core STATIC
  src/util/log.cpp
  src/util/format.cpp
  src/util/json.cpp
  src/instruments.cpp
  src/config.cpp
  src/book/book.cpp
  src/book/builder.cpp
  src/book/feed.cpp
  src/signals/vpin.cpp
  src/signals/flow.cpp
  src/signals/external.cpp
  src/quoting/engine.cpp
  src/execution/throttle.cpp
  src/execution/quotes.cpp
  src/execution/simulated.cpp
  src/execution/ibkr.cpp
  src/inventory.cpp
  src/risk.cpp
  src/live/journal.cpp
  src/live/pipeline.cpp
  src/live/runner.cpp
  src/replay/synthetic.cpp
  src/replay/runner.cpp
)
target_include_directories(harvester_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(harvester_core PUBLIC ${HARVESTER_YAML_TARGET} Threads::Threads)
target_compile_options(harvester_core PRIVATE -Wall -Wextra -Wpedantic)
if(HARVESTER_NATIVE)
  target_compile_options(harvester_core PUBLIC -march=native)
endif()

# -- Databento --------------------------------------------------------------
if(HARVESTER_WITH_DATABENTO)
  find_package(databento CONFIG QUIET)
  if(NOT databento_FOUND)
    if(NOT HARVESTER_FETCH_DATABENTO)
      message(FATAL_ERROR "databento-cpp not found and HARVESTER_FETCH_DATABENTO is OFF")
    endif()
    message(STATUS "databento-cpp not installed; fetching v0.42.0 (needs OpenSSL and zstd headers)")
    set(DATABENTO_ENABLE_UNIT_TESTING OFF CACHE BOOL "" FORCE)
    set(DATABENTO_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(databento GIT_REPOSITORY https://github.com/databento/databento-cpp.git GIT_TAG v0.42.0)
    FetchContent_MakeAvailable(databento)
  endif()
  target_sources(harvester_core PRIVATE src/databento/feed.cpp src/databento/dbn_source.cpp src/databento/fetch.cpp)
  target_compile_definitions(harvester_core PUBLIC HARVESTER_WITH_DATABENTO=1)
  target_link_libraries(harvester_core PUBLIC databento::databento)
else()
  target_sources(harvester_core PRIVATE src/databento/stub.cpp)
endif()

# -- IBKR ---------------------------------------------------------------
if(HARVESTER_WITH_IBKR)
  if(NOT TWS_API_DIR OR NOT EXISTS "${TWS_API_DIR}/EClientSocket.h")
    message(FATAL_ERROR "HARVESTER_WITH_IBKR needs TWS_API_DIR pointing at IBJts/source/cppclient/client")
  endif()
  include(cmake/TwsApi.cmake)
  target_sources(harvester_core PRIVATE src/execution/tws_gateway.cpp)
  target_compile_definitions(harvester_core PUBLIC HARVESTER_WITH_IBKR=1)
  target_link_libraries(harvester_core PUBLIC twsapi)
endif()

# -- the binary --------------------------------------------------------------
add_executable(harvester src/main.cpp)
target_link_libraries(harvester PRIVATE harvester_core)
target_compile_options(harvester PRIVATE -Wall -Wextra)
install(TARGETS harvester RUNTIME DESTINATION bin)

# -- tests ------------------------------------------------------------------
if(HARVESTER_BUILD_TESTS)
  enable_testing()
  add_executable(harvester_tests
    tests/main.cpp
    tests/test_book.cpp
    tests/test_config.cpp
    tests/test_engine.cpp
    tests/test_execution.cpp
    tests/test_ibkr.cpp
    tests/test_inventory_risk.cpp
    tests/test_replay_live.cpp
    tests/test_signals.cpp
    tests/test_util.cpp
  )
  target_include_directories(harvester_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(harvester_tests PRIVATE harvester_core)
  target_compile_definitions(harvester_tests PRIVATE HARVESTER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(harvester_tests PRIVATE -Wall -Wextra)
  add_test(NAME harvester_tests COMMAND harvester_tests)
endif()

# -- benchmark ----------------------------------------------------------------
if(HARVESTER_BUILD_BENCH)
  add_executable(harvester_bench bench/bench.cpp)
  target_link_libraries(harvester_bench PRIVATE harvester_core)
endif()

# -- the edge probe ------------------------------------------------------------
# Measures what a passive quote is paid and what it is charged, per sweep,
# on any GLBX tape. Needs the DBN reader.
if(HARVESTER_WITH_DATABENTO)
  add_executable(harvester_edge tools/edge_probe.cpp)
  target_link_libraries(harvester_edge PRIVATE harvester_core)
  target_compile_options(harvester_edge PRIVATE -Wall -Wextra)
endif()
