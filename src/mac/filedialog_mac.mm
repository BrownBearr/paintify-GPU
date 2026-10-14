// filedialog.h on macOS: NSOpenPanel / NSSavePanel, shown as the system
// presents them (sidebar, tags, Quick Look, recents), and Finder for reveal.

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "filedialog.h"

#include <string>
#include <vector>

namespace filedialog {
namespace {

// "*.png;*.jpg" -> [png, jpg] as content types. A custom extension (.sbr)
// becomes a dynamic type, which the panels filter on just the same.
NSArray<UTType*>* contentTypes(const std::vector<Filter>& filters) {
    NSMutableArray<UTType*>* out = [NSMutableArray array];
    for (const Filter& f : filters) {
        const std::string pats = f.patterns ? f.patterns : "";
        size_t pos = 0;
        while (pos <= pats.size()) {
            size_t end = pats.find(';', pos);
            if (end == std::string::npos) end = pats.size();
            std::string p = pats.substr(pos, end - pos);
            if (p.rfind("*.", 0) == 0) p = p.substr(2);
            if (!p.empty() && p != "*") {
                UTType* t = [UTType typeWithFilenameExtension:@(p.c_str())];
                if (t) [out addObject:t];
            }
            pos = end + 1;
        }
    }
    return out;
}

std::string pathOf(NSURL* url) {
    return url.fileSystemRepresentation ? std::string(url.fileSystemRepresentation) : std::string();
}

// Panels are modal sheets of nothing in particular: bring the app forward
// first, or a panel opened from a menu can come up behind another app.
void activate() {
    [NSApp activateIgnoringOtherApps:YES];
}

// The window that was key before the panel; it gets focus back afterwards so
// keyboard shortcuts work without an extra click.
void restoreKey(NSWindow* w) {
    if (w) [w makeKeyAndOrderFront:nil];
}

} // namespace

void init() {}

std::string openFile(const char* title, const std::vector<Filter>& filters) {
    @autoreleasepool {
        NSWindow* key = NSApp.keyWindow;
        activate();
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.message = title ? @(title) : nil;
        p.canChooseFiles = YES;
        p.canChooseDirectories = NO;
        p.allowsMultipleSelection = NO;
        NSArray<UTType*>* types = contentTypes(filters);
        if (types.count) p.allowedContentTypes = types;
        const NSModalResponse r = [p runModal];
        restoreKey(key);
        if (r != NSModalResponseOK || p.URLs.count == 0) return {};
        return pathOf(p.URLs.firstObject);
    }
}

std::vector<std::string> openFiles(const char* title, const std::vector<Filter>& filters) {
    @autoreleasepool {
        NSWindow* key = NSApp.keyWindow;
        activate();
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.message = title ? @(title) : nil;
        p.canChooseFiles = YES;
        p.canChooseDirectories = NO;
        p.allowsMultipleSelection = YES;
        NSArray<UTType*>* types = contentTypes(filters);
        if (types.count) p.allowedContentTypes = types;
        const NSModalResponse r = [p runModal];
        restoreKey(key);
        std::vector<std::string> out;
        if (r != NSModalResponseOK) return out;
        for (NSURL* u in p.URLs) out.push_back(pathOf(u));
        return out;
    }
}

std::string pickFolder(const char* title) {
    @autoreleasepool {
        NSWindow* key = NSApp.keyWindow;
        activate();
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.message = title ? @(title) : nil;
        p.canChooseFiles = NO;
        p.canChooseDirectories = YES;
        p.canCreateDirectories = YES;
        p.allowsMultipleSelection = NO;
        p.prompt = @"Choose";
        const NSModalResponse r = [p runModal];
        restoreKey(key);
        if (r != NSModalResponseOK || p.URLs.count == 0) return {};
        return pathOf(p.URLs.firstObject);
    }
}

std::string saveFile(const char* title, const std::vector<Filter>& filters,
                     const char* defaultName, const char* defaultExt) {
    @autoreleasepool {
        NSWindow* key = NSApp.keyWindow;
        activate();
        NSSavePanel* p = [NSSavePanel savePanel];
        p.message = title ? @(title) : nil;
        p.canCreateDirectories = YES;
        NSArray<UTType*>* types = contentTypes(filters);
        if (types.count) p.allowedContentTypes = types;
        if (defaultName && *defaultName) p.nameFieldStringValue = @(defaultName);
        const NSModalResponse r = [p runModal];
        restoreKey(key);
        if (r != NSModalResponseOK || !p.URL) return {};
        std::string path = pathOf(p.URL);
        // allowedContentTypes already appends the extension; this covers an
        // empty filter list.
        if (defaultExt && *defaultExt && p.URL.pathExtension.length == 0)
            path += std::string(".") + defaultExt;
        return path;
    }
}

bool reveal(const std::string& path) {
    @autoreleasepool {
        if (path.empty()) return false;
        NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
        BOOL isDir = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:url.path isDirectory:&isDir])
            return false;
        if (isDir) return [[NSWorkspace sharedWorkspace] openURL:url];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
        return true;
    }
}

} // namespace filedialog
