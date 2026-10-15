cmake_minimum_required(VERSION 3.25)
project(ui_paint_tests LANGUAGES CXX)
find_package(rexglue CONFIG REQUIRED)
add_executable(ui-paint-tests main.cpp)
target_link_libraries(ui-paint-tests PRIVATE rex::runtime)
target_compile_features(ui-paint-tests PRIVATE cxx_std_23)
rexglue_apply_target_settings(ui-paint-tests)
# Presenter contains feature-dependent members. Match this SDK's rexui flags
# when subclassing it (the installed runtime interface omits these definitions).
target_compile_definitions(ui-paint-tests PRIVATE REX_HAS_NIS=1 REX_HAS_FIDELITYFX_SDK=1)
