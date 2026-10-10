# The D3D hook table (no SDK dependency; tested by backend/tests) and, when the
# SDK is present, a compile check of the game-side wrappers. The wrappers are
# linked into king_kong, not into the plugin: kk/CMakeLists.txt adds
# kk_native_hooks.cpp to the game target and links nr_hooks.
add_library(nr_hooks STATIC hook_table.cpp trace_replay.cpp)
target_include_directories(nr_hooks PUBLIC "${NR_ROOT}")
target_compile_features(nr_hooks PUBLIC cxx_std_20)

if(TARGET rex::runtime)
    # Compiles against the SDK headers; the __imp__sub_* symbols come from the
    # game's recompiled code, so this is never linked here.
    add_library(nr_kk_hooks_check OBJECT kk_native_hooks.cpp)
    target_link_libraries(nr_kk_hooks_check PRIVATE nr_hooks rex::runtime)
endif()
