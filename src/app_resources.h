#pragma once
#include <vector>

// Bytes of a resource compiled into the executable (assets/icon/brushkit.rc).
// Elsewhere the same images are read from the app bundle (or the source tree).
// Returns false when the resource is missing.
bool loadEmbeddedResource(const char* name, std::vector<unsigned char>& out);
