cmake_minimum_required(VERSION 3.20)
project(hft_conflator_asm C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ENABLE_HFT_AGGRESSIVE_OPT "Aggressive HFT opts (LTO, fast-math, CPU tune)" OFF)

set(ASM_SOURCES
  src/asm/spsc.S
  src/asm/vwap.S
  src/asm/latency.S
  src/asm/parse.S
)

set(C_SOURCES
  src/main.c
  src/pipeline.c
  src/feed.c
  src/parse_fast.c
  src/report.c
  src/util.c
)

add_executable(hft_conflator ${C_SOURCES} ${ASM_SOURCES})
target_include_directories(hft_conflator PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(ENABLE_HFT_AGGRESSIVE_OPT)
  target_compile_definitions(hft_conflator PRIVATE BUILD_TYPE_STR="opt_release")
else()
  target_compile_definitions(hft_conflator PRIVATE BUILD_TYPE_STR="release")
endif()

# Target-safe CPU flags (never -march=native)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(CPU_TUNE "-mcpu=apple-m1")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(CPU_TUNE "-march=armv8-a+simd")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(CPU_TUNE "-march=x86-64-v2")
else()
  set(CPU_TUNE "")
endif()

target_compile_options(hft_conflator PRIVATE
  -Wall -Wextra -Wpedantic
  -fno-exceptions
  ${CPU_TUNE}
)

if(ENABLE_HFT_AGGRESSIVE_OPT)
  target_compile_options(hft_conflator PRIVATE
    -O3 -ffast-math -fomit-frame-pointer -funroll-loops
  )
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_ok OUTPUT ipo_err)
  if(ipo_ok)
    set_property(TARGET hft_conflator PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
  target_link_options(hft_conflator PRIVATE -O3)
else()
  target_compile_options(hft_conflator PRIVATE -O2)
endif()

target_link_libraries(hft_conflator PRIVATE pthread)

# Tests
add_executable(hft_tests
  tests/test_main.c
  src/pipeline.c
  src/feed.c
  src/parse_fast.c
  src/report.c
  src/util.c
  ${ASM_SOURCES}
)
target_include_directories(hft_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(hft_tests PRIVATE -O2 -Wall -Wextra ${CPU_TUNE})
target_link_libraries(hft_tests PRIVATE pthread)

enable_testing()
add_test(NAME unit COMMAND hft_tests)

install(TARGETS hft_conflator RUNTIME DESTINATION bin)
