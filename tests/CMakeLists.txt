add_executable(test_shm_manager
    test_shm_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/shm_manager.cpp
)

target_include_directories(test_shm_manager PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_shm_manager PRIVATE
    nexus_common
    ${RT_LIB}          # Shared memory (shm_open etc.)
    Threads::Threads   # std::thread, used by the Phase 4 concurrency test
)

target_compile_features(test_shm_manager PRIVATE cxx_std_20)

add_test(NAME test_shm_manager COMMAND test_shm_manager)

# --- Phase 7: CRC + wire framing --------------------------------------------

add_executable(test_crc test_crc.cpp)
target_include_directories(test_crc PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_crc PRIVATE nexus_common)
target_compile_features(test_crc PRIVATE cxx_std_20)
add_test(NAME test_crc COMMAND test_crc)

add_executable(test_wire test_wire.cpp)
target_include_directories(test_wire PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_wire PRIVATE nexus_common)
target_compile_features(test_wire PRIVATE cxx_std_20)
add_test(NAME test_wire COMMAND test_wire)

# --- Phase 7 (brief numbering): UDP receiver --------------------------------

add_executable(test_udp_receiver
    test_udp_receiver.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/udp_receiver.cpp
)
target_include_directories(test_udp_receiver PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(test_udp_receiver PRIVATE cxx_std_20)
add_test(NAME test_udp_receiver COMMAND test_udp_receiver)

# --- Phase 8: wire validation -> Frame parsing/dispatch ---------------------

add_executable(test_frame_dispatcher
    test_frame_dispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/frame_dispatcher.cpp
)
target_include_directories(test_frame_dispatcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_frame_dispatcher PRIVATE nexus_common nexus_proto)
target_compile_features(test_frame_dispatcher PRIVATE cxx_std_20)
add_test(NAME test_frame_dispatcher COMMAND test_frame_dispatcher)

add_executable(test_receiver_args
    test_receiver_args.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/receiver_args.cpp
)
target_include_directories(test_receiver_args PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(test_receiver_args PRIVATE cxx_std_20)
add_test(NAME test_receiver_args COMMAND test_receiver_args)

# --- Phase 10: Reed-Solomon decode (Intel ISA-L) ----------------------------

add_executable(test_rs_codec
    test_rs_codec.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/rs_codec.cpp
)
target_include_directories(test_rs_codec PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_rs_codec PRIVATE ${ISAL_LIB})
target_compile_features(test_rs_codec PRIVATE cxx_std_20)
add_test(NAME test_rs_codec COMMAND test_rs_codec)

# --- Phase 11: mmap output --------------------------------------------------

add_executable(test_block_writer
    test_block_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/block_writer.cpp
)
target_include_directories(test_block_writer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(test_block_writer PRIVATE cxx_std_20)
add_test(NAME test_block_writer COMMAND test_block_writer)

# --- Phase 12: RX IPC transport ---------------------------------------------

add_executable(test_uds_channel
    test_uds_channel.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/uds_channel.cpp
)
target_include_directories(test_uds_channel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_uds_channel PRIVATE Threads::Threads)
target_compile_features(test_uds_channel PRIVATE cxx_std_20)
add_test(NAME test_uds_channel COMMAND test_uds_channel)

add_executable(test_rx_envelope
    test_rx_envelope.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/rx_envelope.cpp
)
target_include_directories(test_rx_envelope PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_rx_envelope PRIVATE nexus_proto nexus_proto_hash)
target_compile_features(test_rx_envelope PRIVATE cxx_std_20)
add_test(NAME test_rx_envelope COMMAND test_rx_envelope)

# --- Phase 9: DataPacket -> block table (the full vertical slice) ----------

add_executable(test_session_pipeline
    test_session_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/session_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/shm_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/rs_codec.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/block_writer.cpp
)
target_include_directories(test_session_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_session_pipeline PRIVATE nexus_proto ${ISAL_LIB} ${RT_LIB})
target_compile_features(test_session_pipeline PRIVATE cxx_std_20)
add_test(NAME test_session_pipeline COMMAND test_session_pipeline)
