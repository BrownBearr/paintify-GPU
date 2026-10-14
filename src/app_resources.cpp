#include "app_resources.h"

#ifndef _WIN32
#include "gl_util.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#endif

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
    // No resource section outside Windows: the same files ship in the app
    // bundle's Resources folder, with the source checkout as a fallback.
    out.clear();
    if (std::strcmp(name, "PAINTIFY_LOGO") != 0) return false;
    std::string path = glu::resourceDir("paintify.png");
    if (path.empty()) path = std::string(SBR_ROOT_DIR) + "/assets/icon/paintify.png";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) out.insert(out.end(), chunk, chunk + n);
    std::fclose(f);
    return !out.empty();
#endif
}
