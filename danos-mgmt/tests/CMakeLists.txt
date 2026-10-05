cmake_minimum_required(VERSION 3.16)

add_executable(cli_test test_cli.c)
target_link_libraries(cli_test PRIVATE danos-mgmt danos-dpa pthread)
target_compile_options(cli_test PRIVATE -UNDEBUG)
add_test(NAME cli COMMAND cli_test)

add_executable(gnmi_test test_gnmi.c)
target_link_libraries(gnmi_test PRIVATE danos-mgmt danos-dpa pthread)
target_compile_options(gnmi_test PRIVATE -UNDEBUG)
add_test(NAME gnmi COMMAND gnmi_test)

add_executable(netconf_test test_netconf.c)
target_link_libraries(netconf_test PRIVATE danos-mgmt danos-dpa pthread)
target_compile_options(netconf_test PRIVATE -UNDEBUG)
add_test(NAME netconf COMMAND netconf_test)

add_executable(gnmi_proto_test test_gnmi_proto.c)
target_link_libraries(gnmi_proto_test PRIVATE danos-mgmt danos-dpa pthread)
target_compile_options(gnmi_proto_test PRIVATE -UNDEBUG)
add_test(NAME gnmi_proto COMMAND gnmi_proto_test)

add_executable(hpack_test test_hpack.c)
target_link_libraries(hpack_test PRIVATE danos-mgmt pthread)
target_compile_options(hpack_test PRIVATE -UNDEBUG)
add_test(NAME hpack COMMAND hpack_test)

add_executable(gnmi_grpc_test test_gnmi_grpc.c)
target_link_libraries(gnmi_grpc_test PRIVATE danos-mgmt danos-dpa pthread)
target_compile_options(gnmi_grpc_test PRIVATE -UNDEBUG)
add_test(NAME gnmi_grpc COMMAND gnmi_grpc_test)

add_executable(frontends_consistent_test test_frontends_consistent.c)
target_link_libraries(frontends_consistent_test PRIVATE danos-mgmt danos-core danos-dpa pthread)
target_compile_options(frontends_consistent_test PRIVATE -UNDEBUG)
add_test(NAME frontends_consistent COMMAND frontends_consistent_test)

add_executable(authz_test test_authz.c)
target_link_libraries(authz_test PRIVATE danos-mgmt danos-security pthread)
target_compile_options(authz_test PRIVATE -UNDEBUG)
add_test(NAME authz COMMAND authz_test)

if(DANOS_BUILD_VPP)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(DANOS_OPENSSL_EXECUTABLE openssl)
  if(NOT DANOS_OPENSSL_EXECUTABLE)
    message(FATAL_ERROR "openssl executable is required for mTLS acceptance")
  endif()
  add_test(NAME gnmi_mtls COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_mtls.py
    $<TARGET_FILE:danos-mgrd> ${DANOS_OPENSSL_EXECUTABLE})
  set_tests_properties(gnmi_mtls PROPERTIES TIMEOUT 120)
endif()
