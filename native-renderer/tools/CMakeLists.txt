# Tools (standalone build, or NR_BUILD_TOOLS=ON):
#   nr_pipelines  merges the pipeline files of several runs into the list shipped beside the
#                 game (docs/backend.md, "Pipeline lists")
add_executable(nr_pipelines nr_pipelines.cpp)
target_link_libraries(nr_pipelines PRIVATE nr_draw)

# Resource descriptions/hashes from local chapter recordings, with no GPU or
# runtime dependency. The bundle is produced beside packages, never checked in.
if(TARGET kknr_resources)
    add_executable(nr_resources nr_resources.cpp)
    target_include_directories(nr_resources PRIVATE "${NR_ROOT}")
    target_link_libraries(nr_resources PRIVATE kknr_resources)
endif()
