# Tools (standalone build, or NR_BUILD_TOOLS=ON):
#   nr_pipelines  merges the pipeline files of several runs into the list shipped beside the
#                 game (docs/backend.md, "Pipeline lists")
add_executable(nr_pipelines nr_pipelines.cpp)
target_link_libraries(nr_pipelines PRIVATE nr_draw)
