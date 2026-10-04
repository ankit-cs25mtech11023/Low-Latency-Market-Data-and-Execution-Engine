# Developer tools (synthetic ITCH fixture generator, slicers) are added here from Phase 1.
add_executable(gen_fixture gen_fixture/main.cpp)
target_link_libraries(gen_fixture PRIVATE lle_core lle_warnings)
add_executable(slice_itch slice_itch/main.cpp)
target_link_libraries(slice_itch PRIVATE lle_core lle_warnings)
add_executable(diff_books diff_books/main.cpp)
target_link_libraries(diff_books PRIVATE lle_core lle_warnings)
