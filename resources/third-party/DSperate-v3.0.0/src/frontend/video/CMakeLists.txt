# SPDX-License-Identifier: GPL-3.0-or-later
# Frontend video code with no windowing dependency: screen layout (and, as
# the video overhaul lands, the presenters' shared pieces). Used by the SDL
# frontend and the tests.
add_library(dsperate_frontend_video STATIC layout.cpp select.cpp)
target_link_libraries(dsperate_frontend_video PUBLIC dsperate_core)
target_include_directories(dsperate_frontend_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# Placement is float arithmetic: no fused multiply-add, so every architecture
# lands on the same pixel (test_layout pins it).
set_source_files_properties(layout.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
