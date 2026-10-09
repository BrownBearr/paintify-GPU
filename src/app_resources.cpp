#include "app_resources.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

bool loadEmbeddedResource(const char* name, std::vector<unsigned char>& out) {
#ifdef _WIN32
    HRSRC res = FindResourceA(nullptr, name, MAKEINTRESOURCEA(10));   // RT_RCDATA
    if (!res) return false;
    HGLOBAL mem = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data || size == 0) return false;
    const auto* bytes = static_cast<const unsigned char*>(data);
    out.assign(bytes, bytes + size);
    return true;
#else
    (void)name;
    out.clear();
    return false;
#endif
}
