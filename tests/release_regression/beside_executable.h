#pragma once
#include <Windows.h>
#include <cwchar>

inline bool BesideExecutable(const wchar_t* leaf) {
    wchar_t module[MAX_PATH]{},executable[MAX_PATH]{};
    const HMODULE image=GetModuleHandleW(leaf);
    if(!image||!GetModuleFileNameW(image,module,MAX_PATH)||!GetModuleFileNameW(nullptr,executable,MAX_PATH))return false;
    wchar_t* const moduleLeaf=wcsrchr(module,L'\\');
    wchar_t* const executableLeaf=wcsrchr(executable,L'\\');
    if(!moduleLeaf||!executableLeaf)return false;
    *moduleLeaf=0;
    *executableLeaf=0;
    return !_wcsicmp(module,executable);
}
