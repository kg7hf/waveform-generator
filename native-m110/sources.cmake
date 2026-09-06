get_filename_component(WFG_NATIVE_M110_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(WFG_NATIVE_M110_INCLUDE "${WFG_NATIVE_M110_ROOT}")
set(WFG_NATIVE_M110_SOURCES
    "${WFG_NATIVE_M110_ROOT}/source.cpp"
    "${WFG_NATIVE_M110_ROOT}/wav_generator.cpp"
    "${WFG_NATIVE_M110_ROOT}/../waveform-source/wav_generator.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/transmitter.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/body_waveform.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/demapper.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/fec.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/interleaver.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/scrambler.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/synchronization.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/types.cpp"
    "${WFG_NATIVE_M110_ROOT}/core/waveform.cpp")
