cmake_minimum_required(VERSION 3.24)
project(FsrTests LANGUAGES C CXX)
set(NATIVE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
set(MFG_GENERATED_DIR "" CACHE PATH "The DLL build's generated folder (tools/linux/build.sh)")
set(FSR_GENERATED "${MFG_GENERATED_DIR}/fsr")
if(NOT EXISTS "${FSR_GENERATED}/stamp.sha256")
    message(FATAL_ERROR "Set MFG_GENERATED_DIR to a DLL build's generated folder: ${FSR_GENERATED}/stamp.sha256 is missing")
endif()
set(FSR_SDK "${FSR_GENERATED}/sdk")
set(FSR_SDK_SOURCES
    "${FSR_SDK}/src/components/frameinterpolation/ffx_frameinterpolation.cpp"
    "${FSR_SDK}/src/components/opticalflow/ffx_opticalflow.cpp"
    "${FSR_SDK}/src/backends/dx12/ffx_dx12.cpp"
    "${FSR_SDK}/src/backends/shared/ffx_shader_blobs.cpp"
    "${FSR_SDK}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp"
    "${FSR_SDK}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp"
    "${FSR_SDK}/src/shared/ffx_assert.cpp"
    "${FSR_SDK}/src/shared/ffx_message.cpp"
    "${FSR_SDK}/src/shared/ffx_breadcrumbs_list.cpp"
    "${FSR_SDK}/src/shared/ffx_object_management.cpp")
add_executable(FsrModelTests fsr_model_tests.cpp "${NATIVE}/fsr/fsr_model.cpp" ${FSR_SDK_SOURCES})
set_source_files_properties(${FSR_SDK_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/FIbit"
    COMPILE_DEFINITIONS "FFX_FI;FFX_OF;_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
target_compile_features(FsrModelTests PRIVATE cxx_std_20)
target_include_directories(FsrModelTests PRIVATE "${NATIVE}" "${FSR_SDK}/include" "${FSR_SDK}/src/components"
    "${FSR_SDK}/src/shared" "${FSR_SDK}/src/backends/shared" "${FSR_SDK}/libs" "${FSR_GENERATED}/shaders")
target_compile_definitions(FsrModelTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(FsrModelTests PRIVATE d3d12 dxgi)
set_target_properties(FsrModelTests PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
enable_testing()
