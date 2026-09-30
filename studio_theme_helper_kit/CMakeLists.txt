cmake_minimum_required(VERSION 3.22)

project(studio_theme_qt LANGUAGES CXX)

add_library(studio_theme_qt_helper SHARED
    main.cpp
)

target_compile_features(studio_theme_qt_helper PRIVATE cxx_std_23)

target_link_libraries(studio_theme_qt_helper PRIVATE
    roblox_modloader
)

set_target_properties(studio_theme_qt_helper PROPERTIES
    OUTPUT_NAME "studio_theme_qt_helper"
)
