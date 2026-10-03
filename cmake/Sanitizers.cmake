# LLE_SANITIZER selects one sanitizer for the whole build (tests only; never used for
# performance numbers). Values: "" | address | undefined | thread | fuzzer
set(LLE_SANITIZER "" CACHE STRING "Sanitizer: address, undefined, thread")

if(LLE_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(LLE_SANITIZER STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined -fno-sanitize-recover=all)
elseif(LLE_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
elseif(NOT LLE_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown LLE_SANITIZER='${LLE_SANITIZER}'")
endif()
