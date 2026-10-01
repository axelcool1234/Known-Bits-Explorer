cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0219)
  cmake_policy(SET CMP0219 NEW)
endif()

project(KnownBitsExplorer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 23)
  message(FATAL_ERROR
    "KnownBits Explorer requires LLVM 23, found ${LLVM_PACKAGE_VERSION}")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
include(HandleLLVMOptions)

add_executable(known-bits-explorer src/main.cpp)
target_include_directories(known-bits-explorer SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_options(known-bits-explorer PRIVATE ${LLVM_DEFINITIONS_LIST})

llvm_map_components_to_libnames(LLVM_LIBRARIES Support)
target_link_libraries(known-bits-explorer PRIVATE ${LLVM_LIBRARIES})

install(TARGETS known-bits-explorer RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME explorer
    COMMAND ${CMAKE_COMMAND}
      -DEXPLORER=$<TARGET_FILE:known-bits-explorer>
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test/input.txt
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/expected.txt
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunTest.cmake)
endif()
