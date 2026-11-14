cmake_minimum_required(VERSION 3.16)

add_executable(vpp_mapper_test test_vpp_mapper.c)
target_link_libraries(vpp_mapper_test PRIVATE danos-vpp danos-dpa pthread)
target_compile_options(vpp_mapper_test PRIVATE -UNDEBUG)
add_test(NAME vpp_mapper COMMAND vpp_mapper_test)

add_executable(vpp_api_test test_vpp_api.c)
target_link_libraries(vpp_api_test PRIVATE danos-vpp danos-dpa pthread)
target_compile_options(vpp_api_test PRIVATE -UNDEBUG)
add_test(NAME vpp_api COMMAND vpp_api_test)

add_executable(vpp_proto_test test_vpp_proto.c mock_vpp_server.c)
target_link_libraries(vpp_proto_test PRIVATE danos-vpp danos-dpa pthread)
target_compile_options(vpp_proto_test PRIVATE -UNDEBUG)
add_test(NAME vpp_proto COMMAND vpp_proto_test)

# Golden-bytes conformance: encoders transcribed from upstream VPP, not
# literals, so the production codecs cannot agree with a shared assumption.
add_executable(vpp_golden_test test_vpp_golden.c)
target_link_libraries(vpp_golden_test PRIVATE danos-vpp danos-dpa pthread)
target_compile_options(vpp_golden_test PRIVATE -UNDEBUG)
add_test(NAME vpp_golden COMMAND vpp_golden_test)

add_executable(vpp_adapter_test test_vpp_adapter.c)
target_link_libraries(vpp_adapter_test PRIVATE danos-vpp danos-core danos-dpa pthread)
target_compile_options(vpp_adapter_test PRIVATE -UNDEBUG)
add_test(NAME vpp_adapter COMMAND vpp_adapter_test)
add_executable(vpp_io_test test_vpp_io.c)
target_link_libraries(vpp_io_test PRIVATE danos-vpp pthread)
target_compile_options(vpp_io_test PRIVATE -UNDEBUG)
add_test(NAME vpp_io COMMAND vpp_io_test)
set_tests_properties(vpp_io PROPERTIES TIMEOUT 5)
add_executable(vpp_readback_test test_vpp_readback.c)
target_link_libraries(vpp_readback_test PRIVATE danos-vpp pthread)
target_compile_options(vpp_readback_test PRIVATE -UNDEBUG)
target_link_options(vpp_readback_test PRIVATE
  -Wl,--wrap=danos_vpp_api_is_mock -Wl,--wrap=danos_vpp_api_is_connected
  -Wl,--wrap=danos_vpp_api_query -Wl,--wrap=danos_vpp_api_dump_query)
add_test(NAME vpp_readback COMMAND vpp_readback_test)
