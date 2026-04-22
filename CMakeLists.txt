# Minimum CMake required
cmake_minimum_required(VERSION 3.15)

# Project
project(kvstore)

# Add the utils directory to the include path
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/utils)
set(UTILS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/utils/utils.cc)
set(LOGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/utils/logger.cc)
set(SERIALIZER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/utils/serializer.cc)

# Protobuf
set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG REQUIRED)
message(STATUS "Using protobuf ${protobuf_VERSION}")

# Protobuf-compiler
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)

# gRPC
find_package(gRPC CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_VERSION}")
set(_GRPC_GRPCPP gRPC::grpc++)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

# kvstore proto file
get_filename_component(kv_proto "backend/kvstore.proto" ABSOLUTE)
get_filename_component(kv_proto_path "${kv_proto}" PATH)

# master proto file
get_filename_component(master_proto "backend/master.proto" ABSOLUTE)
get_filename_component(master_proto_path "${master_proto}" PATH)

# Generated sources for kvstore.proto
set(kv_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.pb.cc")
set(kv_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.pb.h")
set(kv_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.grpc.pb.cc")
set(kv_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.grpc.pb.h")
add_custom_command(
      OUTPUT "${kv_proto_srcs}" "${kv_proto_hdrs}" "${kv_grpc_srcs}" "${kv_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${kv_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${kv_proto}"
      DEPENDS "${kv_proto}")

# Generated sources for master.proto
set(master_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/master.pb.cc")
set(master_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/master.pb.h")
set(master_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/master.grpc.pb.cc")
set(master_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/master.grpc.pb.h")
add_custom_command(
  OUTPUT "${master_proto_srcs}" "${master_proto_hdrs}" "${master_grpc_srcs}" "${master_grpc_hdrs}"
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
       --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
       -I "${master_proto_path}"
       --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
       "${master_proto}"
  DEPENDS "${master_proto}")


# Targets (client|server)
foreach(_target
  worker coordinator kvstore_client)
  add_executable(${_target} "${CMAKE_CURRENT_SOURCE_DIR}/backend/${_target}.cc"
    ${master_proto_srcs}
    ${master_grpc_srcs}
    ${kv_proto_srcs}
    ${kv_grpc_srcs}
    ${UTILS_SRC}
    ${LOGGER_SRC}
    ${SERIALIZER_SRC})
  target_link_libraries(${_target}
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF})
endforeach()

# add_executable(worker 
# "${CMAKE_CURRENT_SOURCE_DIR}/backend/worker.cc"
#     ${kv_proto_srcs}
#     ${kv_grpc_srcs}
#     ${UTILS_SRC})
# target_link_libraries(worker
#     ${_REFLECTION}
#     ${_GRPC_GRPCPP}
#     ${_PROTOBUF_LIBPROTOBUF})

# add_executable(coordinator 
# "${CMAKE_CURRENT_SOURCE_DIR}/backend/coordinator.cc"
#     ${master_proto_srcs}
#     ${master_grpc_srcs}
#     ${UTILS_SRC})
# target_link_libraries(coordinator
#     ${_REFLECTION}
#     ${_GRPC_GRPCPP}
#     ${_PROTOBUF_LIBPROTOBUF})

# add_executable(kvstore_client 
# "${CMAKE_CURRENT_SOURCE_DIR}/backend/kvstore_client.cc"
#     ${kv_proto_srcs}
#     ${kv_grpc_srcs}
#     ${master_proto_srcs}
#     ${master_grpc_srcs}
#     ${UTILS_SRC})
# target_link_libraries(kvstore_client
#     ${_REFLECTION}
#     ${_GRPC_GRPCPP}
#     ${_PROTOBUF_LIBPROTOBUF})


# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/frontend/email)

include_directories("${CMAKE_CURRENT_BINARY_DIR}")
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/frontend/user_account)
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/frontend/admin)
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/frontend/storage)


# Include web_pages folder in the build output
set(WEB_PAGES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/frontend/web_pages")
set(WEB_PAGES_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/frontend/web_pages")

# Collect all HTML and CSS files
file(GLOB WEB_PAGES_FILES "${WEB_PAGES_DIR}/*.html" "${WEB_PAGES_DIR}/*.css")

# Ensure the output directory exists
file(MAKE_DIRECTORY ${WEB_PAGES_OUTPUT_DIR})

# Copy web files to build directory
add_custom_target(copy_web_pages ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${WEB_PAGES_DIR} ${WEB_PAGES_OUTPUT_DIR}
    COMMENT "Copying web_pages to build directory")

# Make sure the main targets depend on this
foreach(_target coordinator worker kvstore_client)
  add_dependencies(${_target} copy_web_pages)
endforeach()
# Generated source files (assume they are in the /build directory)
set(KV_PROTO_SRCS "../backend/kvstore.pb.cc")
set(KV_GRPC_SRCS  "../backend/kvstore.grpc.pb.cc")


# Add email_service separately with correct path
# add_executable(email_service 
#   "${CMAKE_CURRENT_SOURCE_DIR}/../frontend/email/email_service.cc"
#   ${kv_proto_srcs}
#   ${kv_grpc_srcs}
#   ${UTILS_SRC})
# target_link_libraries(email_service
#   ${_REFLECTION}
#   ${_GRPC_GRPCPP}
#   ${_PROTOBUF_LIBPROTOBUF})


  add_executable(smtp_server 
  "${CMAKE_CURRENT_SOURCE_DIR}/frontend/email/smtp_server.cc"
  ${kv_proto_srcs}
  ${kv_grpc_srcs}
  ${master_proto_srcs}
  ${master_grpc_srcs}
  ${UTILS_SRC})
target_link_libraries(smtp_server resolv
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

	add_executable(load_balancer 
	  "${CMAKE_CURRENT_SOURCE_DIR}/frontend/load_balancer.cc"
	  ${kv_proto_srcs}
	  ${kv_grpc_srcs}
	  ${master_proto_srcs}
	  ${master_grpc_srcs}
	  ${UTILS_SRC})
	target_link_libraries(load_balancer
	  ${_REFLECTION}
	  ${_GRPC_GRPCPP}
	  ${_PROTOBUF_LIBPROTOBUF})

  add_executable(http_server 
	"${CMAKE_CURRENT_SOURCE_DIR}/frontend/http_server.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/frontend/email/email_service.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/storage/storage_service.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/user_account/user_account_service.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/admin/admin_service.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/utils/serializer.cc"
  ${kv_proto_srcs}
  ${kv_grpc_srcs}
  ${master_proto_srcs}
  ${master_grpc_srcs}
  ${UTILS_SRC})
  target_link_libraries(http_server
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF})
    
add_dependencies(http_server copy_web_pages)