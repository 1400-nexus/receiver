cmake_minimum_required(VERSION 3.20)
project(receiver)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. Find Dependencies
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

# Find Intel ISA-L (for Reed-Solomon)
find_library(ISAL_LIB NAMES isal REQUIRED)
# Find librt (for Shared Memory /dev/shm)
find_library(RT_LIB NAMES rt REQUIRED)

# 2. Proto Path Setup (Matching the Team)
set(NEXUS_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/nexus-proto)
set(NEXUS_PROTO_SRC ${NEXUS_PROTO_DIR}/proto)
set(NEXUS_PROTO_GEN ${NEXUS_PROTO_DIR}/generated/cpp)
set(NEXUS_PROTO_NAMES common ipc net rx)

if(NOT EXISTS ${NEXUS_PROTO_SRC}/net.proto)
    message(FATAL_ERROR "nexus-proto not found! Run: git submodule update --init --recursive")
endif()

# 3. Team's Protobuf Generation Logic (Copied for compatibility)
option(NEXUS_USE_CHECKED_IN_GENCODE "Use libs/nexus-proto/generated/cpp" OFF)
set(NEXUS_PROTO_SRCS "")

if(NOT NEXUS_USE_CHECKED_IN_GENCODE AND TARGET protobuf::protoc)
    set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated/proto)
    file(MAKE_DIRECTORY ${_gen})
    foreach(_n IN LISTS NEXUS_PROTO_NAMES)
        add_custom_command(
            OUTPUT ${_gen}/${_n}.pb.cc ${_gen}/${_n}.pb.h
            COMMAND protobuf::protoc --cpp_out ${_gen} -I ${NEXUS_PROTO_SRC} ${NEXUS_PROTO_SRC}/${_n}.proto
            DEPENDS ${NEXUS_PROTO_SRC}/${_n}.proto protobuf::protoc
            VERBATIM)
        list(APPEND NEXUS_PROTO_SRCS ${_gen}/${_n}.pb.cc)
    endforeach()
    set(NEXUS_PROTO_INC ${_gen})
else()
    # (Checked-in gencode logic omitted for brevity, same as team's)
    foreach(_n IN LISTS NEXUS_PROTO_NAMES)
        list(APPEND NEXUS_PROTO_SRCS ${NEXUS_PROTO_GEN}/${_n}.pb.cc)
    endforeach()
    set(NEXUS_PROTO_INC ${NEXUS_PROTO_GEN})
endif()

# 3.5 proto_hash: cross-language BLAKE3 hash of the shared .proto contract
# (rx.proto ReceiverHello.proto_hash, Phase 12). Computed once here, at
# configure time, and baked into a generated header as a constexpr byte
# array -- the BUILT BINARY has zero BLAKE3 dependency; only the machine
# running this configure step needs Python's blake3 package. Algorithm
# and reasoning: cmake/proto_hash.py's own docstring, cross-checked
# against Session-Manager's real ipc/handshake.py
# (docs/ANSWERS_FROM_C.md §5 -- that's what the UDS handshake actually
# compares against).
find_package(Python3 COMPONENTS Interpreter REQUIRED)

execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import blake3"
    RESULT_VARIABLE _blake3_missing
    ERROR_QUIET OUTPUT_QUIET)
if(_blake3_missing)
    message(FATAL_ERROR
        "proto_hash: the Python 'blake3' package is required at build time "
        "(never linked into the built binary -- see cmake/proto_hash.py). "
        "Install it for ${Python3_EXECUTABLE}, e.g.: pip install blake3")
endif()

file(GLOB _proto_hash_files "${NEXUS_PROTO_SRC}/*.proto")
list(SORT _proto_hash_files) # lexicographic byte-wise -- matches handshake.py's sorted(glob(...))

execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/proto_hash.py ${_proto_hash_files}
    OUTPUT_VARIABLE NEXUS_PROTO_HASH_BYTES
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _proto_hash_rc
    ERROR_VARIABLE _proto_hash_err)
if(_proto_hash_rc)
    message(FATAL_ERROR "proto_hash computation failed: ${_proto_hash_err}")
endif()

# Reconfigure automatically if any .proto file changes -- the hash must
# never go stale relative to the schema it's meant to describe.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_proto_hash_files})

# Generated at generated/receiver/proto_hash.hpp so `#include
# "receiver/proto_hash.hpp"` resolves the same way every other
# `receiver/*.hpp` include does relative to src/ -- the include directory
# added below is "generated" (one level up), not "generated/receiver".
set(NEXUS_PROTO_HASH_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/receiver/proto_hash.hpp.in
    ${NEXUS_PROTO_HASH_GEN_DIR}/receiver/proto_hash.hpp
    @ONLY)

add_library(nexus_proto_hash INTERFACE)
target_include_directories(nexus_proto_hash INTERFACE ${NEXUS_PROTO_HASH_GEN_DIR})

# 4. Create Library Targets
add_library(nexus_proto STATIC ${NEXUS_PROTO_SRCS})
target_include_directories(nexus_proto PUBLIC ${NEXUS_PROTO_INC})
target_link_libraries(nexus_proto PUBLIC protobuf::libprotobuf)

# nexus_common (Where you'll put your CRC and Wire logic)
add_library(nexus_common STATIC
    src/common/crc.cpp
    src/common/wire.cpp
)
target_include_directories(nexus_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 5. THE RECEIVER EXECUTABLE
add_executable(receiver
    src/receiver/main.cpp
    src/receiver/receiver.cpp      # You'll create this
    src/receiver/shm_manager.cpp  # You'll create this
    src/receiver/udp_receiver.cpp
    src/receiver/frame_dispatcher.cpp
    src/receiver/receiver_args.cpp
    src/receiver/rs_codec.cpp
    src/receiver/block_writer.cpp
    src/receiver/uds_channel.cpp
    src/receiver/rx_envelope.cpp
    src/receiver/session_pipeline.cpp
)

# Link everything together
target_link_libraries(receiver PRIVATE
    nexus_proto
    nexus_common
    nexus_proto_hash
    Threads::Threads
    ${ISAL_LIB}   # Reed-Solomon math
    ${RT_LIB}     # Shared memory
)

# 6. Testing (matching team style)
enable_testing()
add_subdirectory(tests)