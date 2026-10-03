# Warning set applied to every first-party target (never to fetched dependencies).
# -Wold-style-cast and -Wconversion keep implicit narrowing and C casts out of wire-format code.
add_library(lle_warnings INTERFACE)
target_compile_options(lle_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
if(LLE_WERROR)
    target_compile_options(lle_warnings INTERFACE -Werror)
endif()
