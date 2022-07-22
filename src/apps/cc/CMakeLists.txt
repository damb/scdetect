set(DETECT_TARGET scdetect-cc)

set(
  DETECT_SOURCES
    app.cpp
    binding.cpp
    config/detector.cpp
    config/exception.cpp
    config/template_family.cpp
    config/validators.cpp
    datamodel/ddl.cpp
    detail/sqlite.cpp
    detector/arrival.cpp
    detector/detector.cpp
    detector/exception.cpp
    detector/detection_processor.cpp
    detector/detection_candidate_processor.cpp
    detector/detection_candidate.cpp
    detector/linker.cpp
    detector/linker/exception.cpp
    detector/linker/association.cpp
    detector/linker/pot.cpp
    detector/template_processor.cpp
    detector/template_processor/buffer.cpp
    detector/template_processor/state_machine.cpp
    eventstore.cpp
    exception.cpp
    filter.cpp
    log.cpp
    main.cpp
    processing/detail/gap_interpolate.cpp
    processing/processor.cpp
    processing/stream.cpp
    resamplerstore.cpp
    template_waveform.cpp
    util/filter.cpp
    util/horizontal_components.cpp
    util/util.cpp
    util/waveform_stream_id.cpp
    waveform.cpp
    worker.cpp
    worker/exception.cpp
    worker/recordstream.cpp
    worker/detector_worker.cpp
    worker/event/command.cpp
)


option("SCDETECT_CC_COMPILE_WITH_ASAN" "Build scdetect-cc with address sanitizier" OFF)
mark_as_advanced(${SCDETECT_CC_COMPILE_WITH_ASAN})
if (${SCDETECT_CC_COMPILE_WITH_ASAN} AND ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()


sc_add_executable(DETECT ${DETECT_TARGET})
find_package(SQLite3 REQUIRED)
target_link_libraries(${DETECT_TARGET} ${SQLITE3_LIBRARIES})
sc_link_libraries_internal(${DETECT_TARGET} config client)
sc_install_init(${DETECT_TARGET}
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../base/common/apps/templates/initd.py")

file(GLOB descs "${CMAKE_CURRENT_SOURCE_DIR}/descriptions/*.xml")
install(FILES ${descs} DESTINATION ${SC3_PACKAGE_APP_DESC_DIR})

option("SCDETECT_CC_BUILD_BENCHMARKS" "Build the scdetect-cc benchmarks" OFF)
if (${SCDETECT_CC_BUILD_BENCHMARKS})
  add_subdirectory(perf)
endif()
