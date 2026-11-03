# gli_tool: PNG export/import for .gli graphics libraries (see Docs/Widescreen-16x9.md)
add_executable(gli_tool gli_tool.cpp)
target_compile_features(gli_tool PRIVATE cxx_std_17)
target_link_libraries(gli_tool PRIVATE SDL2::SDL2_image SDL2::SDL2)
