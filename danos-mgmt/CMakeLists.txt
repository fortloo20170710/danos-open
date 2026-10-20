cmake_minimum_required(VERSION 3.16)
find_package(OpenSSL 3.0 REQUIRED)
# danos-mgmt: Management plane (E1-E4)
add_library(danos-mgmt STATIC
  src/cli/cli.c
  src/gnmi/gnmi_proto.c
  src/gnmi/hpack.c
  src/gnmi/hpack_huffman.c
  src/gnmi/model_paths.c
  src/gnmi/model_routes.c
  src/gnmi/gnmi_grpc.c
  src/gnmi/gnmi_tls.c
  src/netconf/netconf.c
  src/netconf/netconf_datastore.c
  src/netconf/netconf_framing.c
  src/authz/authz.c
)
target_include_directories(danos-mgmt PUBLIC src)
target_link_libraries(danos-mgmt PUBLIC danos-dpa danos-core danos-security OpenSSL::SSL OpenSSL::Crypto)

# NETCONF over SSH. OpenSSH spawns this as `Subsystem netconf`; it speaks
# RFC 6242 framing on stdin/stdout.
add_executable(danos-netconf-subsystem src/netconf/netconf_subsystem.c)
target_link_libraries(danos-netconf-subsystem PRIVATE danos-mgmt)
install(TARGETS danos-netconf-subsystem RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

# Legacy gNMI-like HTTP/JSON server (E2). It predates the gRPC stack, has
# no TLS or per-object authorization, and is not started by mgrd. Keep it
# out of the daemon link (and out of its attack surface); only its own
# regression test links it.
if(DANOS_BUILD_TESTS)
  add_library(danos-mgmt-legacy STATIC src/gnmi/gnmi.c)
  target_include_directories(danos-mgmt-legacy PUBLIC src)
  target_link_libraries(danos-mgmt-legacy PUBLIC danos-mgmt)
endif()

if(DANOS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
