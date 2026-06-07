pybind11_add_module(citadel_sim forge_sim.cpp)
target_link_libraries(citadel_sim PRIVATE citadel_core)
target_include_directories(citadel_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
    # /utf-8 — match citadel_core string encoding; /W3 — pybind11 headers trigger
    # some /W4 warnings we don't own so don't use WX here.
    target_compile_options(citadel_sim PRIVATE /W3 /utf-8)
    # Match the 8MB stack used by training worker threads in TrainingScreen.
    target_link_options(citadel_sim PRIVATE /STACK:8388608 /MAP)
endif()
