add_library(xenia-cpu STATIC)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR})
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/backend)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler/passes)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/hir)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/ppc)

if(XENIA_STATIC_ONLY)
  # The strict frontend never invokes the runtime translator. Exclude it from
  # the archive as well so its registered cvars cannot retain compilation code.
  get_target_property(_xe_cpu_sources xenia-cpu SOURCES)
  list(FILTER _xe_cpu_sources EXCLUDE REGEX "/ppc/ppc_translator\\.cc$")
  set_property(TARGET xenia-cpu PROPERTY SOURCES ${_xe_cpu_sources})
endif()
target_include_directories(xenia-cpu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/llvm/include
)
target_link_libraries(xenia-cpu PUBLIC xenia-base mspack)
xe_target_defaults(xenia-cpu)

if(XENIA_BUILD_TESTS)
  add_subdirectory(testing)
  add_subdirectory(ppc/testing)
endif()
