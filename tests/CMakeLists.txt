# imslug tests (ctest). The real suites — glyph pipeline unit tests, CPU
# reference rasterizer, packing invariants, golden images — arrive with
# M1/M2; see .agent-docs/milestones/.

add_executable(imslug_tests test_imslug.cpp)
target_link_libraries(imslug_tests PRIVATE imslug)
set_target_properties(imslug_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug_tests)
add_test(NAME imslug_smoke COMMAND imslug_tests)

# The public header must keep compiling as C++11 (AGENTS.md invariant 4), so it
# gets its own translation unit built with the older standard on purpose.
add_executable(imslug_header_cxx11 test_header_cxx11.cpp)
target_link_libraries(imslug_header_cxx11 PRIVATE imslug)
set_target_properties(imslug_header_cxx11 PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug_header_cxx11)
add_test(NAME imslug_header_cxx11 COMMAND imslug_header_cxx11)
