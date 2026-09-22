# Builds IBKR's TWS API C++ client from ``TWS_API_DIR`` (the
# ``IBJts/source/cppclient/client`` directory of the API download) as a
# static library ``twsapi``, and detects the two places the API changed
# between 10.30 and 10.37 that the adapter has to follow.
#
# The client's ``Decimal.cpp`` calls Intel's BID64 decimal library, which
# IBKR does not ship. ``HARVESTER_TWS_BID_SHIM`` (default ON) links a small
# implementation of the eight functions it needs; set it OFF and point
# ``TWS_BID_LIBRARY`` at a built ``libbid.a`` to use Intel's instead.
#
# API 10.37+ describes some messages with protobuf. If the client sources
# include generated ``.pb.h`` headers, the ``.proto`` files next to them
# are compiled with the protobuf compiler found on the box.

option(HARVESTER_TWS_BID_SHIM "Provide the BID64 decimal functions the TWS client needs" ON)
set(TWS_BID_LIBRARY "" CACHE FILEPATH "Intel's libbid.a, when HARVESTER_TWS_BID_SHIM is OFF")

file(GLOB TWS_SOURCES "${TWS_API_DIR}/*.cpp")
add_library(twsapi STATIC ${TWS_SOURCES})
target_include_directories(twsapi PUBLIC "${TWS_API_DIR}")
target_compile_options(twsapi PRIVATE -w)
set_target_properties(twsapi PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(READ "${TWS_API_DIR}/EClient.h" _tws_eclient)
if(_tws_eclient MATCHES "\\.pb\\.h")
  find_package(Protobuf REQUIRED)
  get_filename_component(_tws_proto_dir "${TWS_API_DIR}/../../proto" ABSOLUTE)
  file(GLOB _tws_protos "${_tws_proto_dir}/*.proto")
  if(NOT _tws_protos)
    message(FATAL_ERROR "the TWS API at ${TWS_API_DIR} needs protobuf messages but ${_tws_proto_dir} has no .proto files")
  endif()
  protobuf_generate_cpp(_tws_pb_sources _tws_pb_headers ${_tws_protos})
  target_sources(twsapi PRIVATE ${_tws_pb_sources})
  target_include_directories(twsapi PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(twsapi PUBLIC protobuf::libprotobuf)
  message(STATUS "TWS API: protobuf messages compiled from ${_tws_proto_dir}")
endif()

file(READ "${TWS_API_DIR}/EWrapper_prototypes.h" _tws_prototypes)
if(_tws_prototypes MATCHES "time_t errorTime")
  target_compile_definitions(twsapi PUBLIC HARVESTER_TWS_ERROR_HAS_TIME=1)
endif()
# The request-id types went away between 10.37 and 10.45: the depth
# callbacks took ``TickerId`` (a typedef for long) and now take ``int``, so
# a signature written for one line is silently not an override on the
# other -- it compiles as a new member function and the callback is never
# called, or the class stays abstract. 10.45 has no ``TickerId`` anywhere
# in its headers, which is what this tests.
if(_tws_prototypes MATCHES "TickerId")
  target_compile_definitions(twsapi PUBLIC HARVESTER_TWS_TICKER_ID=1)
endif()
if(_tws_prototypes MATCHES "long long permId")
  target_compile_definitions(twsapi PUBLIC HARVESTER_TWS_PERMID_LONGLONG=1)
endif()
if(EXISTS "${TWS_API_DIR}/CommissionAndFeesReport.h")
  target_compile_definitions(twsapi PUBLIC HARVESTER_TWS_FEES_REPORT=1)
endif()

if(HARVESTER_TWS_BID_SHIM)
  target_sources(twsapi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/execution/bid64_shim.cpp")
  target_include_directories(twsapi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
elseif(TWS_BID_LIBRARY)
  target_link_libraries(twsapi PUBLIC "${TWS_BID_LIBRARY}")
else()
  message(FATAL_ERROR "HARVESTER_TWS_BID_SHIM is OFF; set TWS_BID_LIBRARY to Intel's libbid.a")
endif()

if(EXISTS "${TWS_API_DIR}/../../../API_VersionNum.txt")
  file(READ "${TWS_API_DIR}/../../../API_VersionNum.txt" _tws_version)
  string(STRIP "${_tws_version}" _tws_version)
  message(STATUS "TWS API: ${_tws_version} from ${TWS_API_DIR}")
endif()
