set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

get_filename_component(MFG_LINUX_TOOLS "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
if(DEFINED ENV{MFG_XWIN})
    set(MFG_XWIN "$ENV{MFG_XWIN}")
else()
    set(MFG_XWIN "$ENV{HOME}/.xwin")
endif()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_ASM_MASM_COMPILER "${MFG_LINUX_TOOLS}/ml64.sh")

set(MFG_SYSTEM_INCLUDES
    "${MFG_XWIN}/crt/include" "${MFG_XWIN}/sdk/include/ucrt" "${MFG_XWIN}/sdk/include/um"
    "${MFG_XWIN}/sdk/include/shared" "${MFG_XWIN}/sdk/include/winrt" "${MFG_LINUX_TOOLS}/include")
set(MFG_COMPILER_INCLUDES "")
foreach(directory IN LISTS MFG_SYSTEM_INCLUDES)
    string(APPEND MFG_COMPILER_INCLUDES " /imsvc ${directory}")
endforeach()
set(CMAKE_C_FLAGS_INIT "${MFG_COMPILER_INCLUDES} /FIintrin.h")
set(CMAKE_CXX_FLAGS_INIT "${MFG_COMPILER_INCLUDES}")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "/EHa")
set(CMAKE_RC_FLAGS_INIT "-I ${MFG_XWIN}/sdk/include/um -I ${MFG_XWIN}/sdk/include/shared")

set(MFG_LIBRARY_PATHS
    "/libpath:${MFG_XWIN}/crt/lib/x86_64 /libpath:${MFG_XWIN}/sdk/lib/um/x86_64 /libpath:${MFG_XWIN}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MFG_LIBRARY_PATHS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${MFG_LIBRARY_PATHS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${MFG_LIBRARY_PATHS}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_CROSSCOMPILING_EMULATOR "${MFG_LINUX_TOOLS}/wine.sh")
