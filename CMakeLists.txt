cmake_minimum_required(VERSION 3.16)
project(distcompile CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DC_SANITIZE "" CACHE STRING "sanitizers to build with, e.g. address,undefined")
add_compile_options(-Wall -Wextra)
if(DC_SANITIZE)
  add_compile_options(-fsanitize=${DC_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${DC_SANITIZE})
endif()

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)

# protobuf + gRPC stubs
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})
set(PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/distcompile.proto)
add_custom_command(
  OUTPUT ${GEN}/distcompile.pb.cc ${GEN}/distcompile.pb.h ${GEN}/distcompile.grpc.pb.cc ${GEN}/distcompile.grpc.pb.h
  COMMAND protobuf::protoc
          --cpp_out=${GEN} --grpc_out=${GEN}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
          -I ${CMAKE_CURRENT_SOURCE_DIR}/proto ${PROTO}
  DEPENDS ${PROTO})

# the schema is compiled into the coordinator, which applies it at startup
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/sql/schema.sql SCHEMA_SQL)
configure_file(src/schema_sql.h.in ${GEN}/schema_sql.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS sql/schema.sql)

add_library(dccore STATIC
  ${GEN}/distcompile.pb.cc
  ${GEN}/distcompile.grpc.pb.cc
  src/util.cpp
  src/cas.cpp
  src/graph.cpp
  src/manifest.cpp
  src/preprocess.cpp)
target_include_directories(dccore PUBLIC src ${GEN})
target_link_libraries(dccore PUBLIC gRPC::grpc++ protobuf::libprotobuf OpenSSL::Crypto Threads::Threads)

add_executable(dc-coordinator src/coordinator.cpp src/db.cpp)
target_link_libraries(dc-coordinator PRIVATE dccore PkgConfig::PQ)

add_executable(dc-worker src/worker.cpp)
target_link_libraries(dc-worker PRIVATE dccore)

add_executable(dcc src/dcc.cpp)
target_link_libraries(dcc PRIVATE dccore)

enable_testing()
add_executable(unit_tests tests/unit_tests.cpp src/db.cpp)
target_link_libraries(unit_tests PRIVATE dccore PkgConfig::PQ)
add_test(NAME unit COMMAND unit_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
