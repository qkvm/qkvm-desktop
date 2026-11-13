# The keyboard and the mouse of a machine that is controlled (DESIGN.md section 3.8). What a key
# is, and what it does while a machine is controlled, is plain C++ that knows nothing of Windows:
# it is tested without a keyboard.
add_library(qkvm_input STATIC
    include/qkvm/input/keys.h
    src/keys.cpp
)
target_include_directories(qkvm_input PUBLIC include)
target_link_libraries(qkvm_input PRIVATE qkvm_build)

qkvm_add_test(test_input_keys SOURCES tests/test_keys.cpp LIBRARIES qkvm_input)
