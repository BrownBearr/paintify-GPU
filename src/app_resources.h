#pragma once
#include <vector>

// Bytes of a resource compiled into the executable (assets/icon/brushkit.rc).
// Returns false when the resource is missing or on non-Windows builds.
bool loadEmbeddedResource(const char* name, std::vector<unsigned char>& out);
