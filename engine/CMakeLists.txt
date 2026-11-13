# FFmpeg as tools/build_ffmpeg.ps1 built it (or tools/fetch_ffmpeg.ps1 brought it, for development). Only the two
# libraries the engine uses: packets arrive
# already cut, and colours are turned in the shader (DESIGN.md section 9).
add_library(qkvm_ffmpeg INTERFACE)
target_include_directories(qkvm_ffmpeg SYSTEM INTERFACE "${QKVM_FFMPEG_DIR}/include")
target_link_libraries(qkvm_ffmpeg INTERFACE "${QKVM_FFMPEG_DIR}/lib/avcodec.lib" "${QKVM_FFMPEG_DIR}/lib/avutil.lib")

# The shaders are compiled when the engine is built, by the compiler of the Windows SDK, each into a
# header of bytes: the program carries no shader compiler and reads no shader file.
find_program(QKVM_FXC fxc REQUIRED)
set(QKVM_SHADER_HEADERS "")
function(qkvm_shader name kind variable)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.${kind}.hlsl")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.${kind}.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${QKVM_FXC}" /nologo /T ${kind}_4_0 /E main /O3 /WX /Fh "${header}" /Vn ${variable} "${source}"
        DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tile.hlsli"
        COMMENT "Compiling the shader ${name}.${kind}.hlsl"
        VERBATIM
    )
    set(QKVM_SHADER_HEADERS ${QKVM_SHADER_HEADERS} "${header}" PARENT_SCOPE)
endfunction()
qkvm_shader(quad vs g_quadVs)
qkvm_shader(planar ps g_planarPs)
qkvm_shader(card ps g_cardPs)
qkvm_shader(fill ps g_fillPs)

# The engine knows nothing of Qt (DESIGN.md section 1). Nothing of FFmpeg or Direct3D shows in its headers.
add_library(qkvm_engine STATIC
    include/qkvm/engine/about.h
    include/qkvm/engine/gpu.h
    include/qkvm/engine/scene.h
    include/qkvm/engine/types.h
    src/about.cpp
    src/d3d.h
    src/decode_pool.cpp
    src/decode_pool.h
    src/decoder.cpp
    src/decoder.h
    src/ffmpeg.h
    src/gpu_core.cpp
    src/gpu_core.h
    src/scene_core.cpp
    src/stream_core.cpp
    src/stream_core.h
    src/surface.cpp
    ${QKVM_SHADER_HEADERS}
)
set_target_properties(qkvm_engine PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(qkvm_engine PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_link_libraries(qkvm_engine PUBLIC qkvm_base PRIVATE qkvm_build qkvm_ffmpeg d3d11 dxgi dxguid)

qkvm_add_test(test_engine_about SOURCES tests/test_about.cpp LIBRARIES qkvm_engine)
# The pool of decoding threads by itself, with work that blocks (it is the engine's own, in src).
qkvm_add_test(test_engine_decode_pool SOURCES tests/test_decode_pool.cpp LIBRARIES qkvm_engine)
target_include_directories(test_engine_decode_pool PRIVATE src)
qkvm_add_test(test_engine_scene SOURCES tests/test_scene.cpp LIBRARIES qkvm_engine qkvm_clips dxgi)
target_compile_definitions(test_engine_scene PRIVATE QKVM_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")
