#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "mac_app.h"

#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>

namespace {

std::deque<macapp::Command> g_commands;
std::vector<std::string> g_opened;
std::mutex g_openedLock;
macapp::MenuState g_state;
float g_magnify = 0.f;
bool g_trackpadScroll = false;
id g_target = nil;
NSMenu* g_recentMenu = nil;

NSString* const kRecentKey = @"PaintifyRecentPaths";
const NSUInteger kRecentMax = 12;

void queueOpened(NSArray<NSURL*>* urls) {
    std::lock_guard<std::mutex> lock(g_openedLock);
    for (NSURL* u in urls)
        if (u.isFileURL && u.fileSystemRepresentation) g_opened.emplace_back(u.fileSystemRepresentation);
}

// -application:openURLs: added to GLFW's application delegate. GLFW does not
// implement it, and without it a file opened from Finder, dropped on the Dock
// icon or picked from the Dock's recent items is silently ignored.
void openURLs(id, SEL, NSApplication*, NSArray<NSURL*>* urls) { queueOpened(urls); }
BOOL secureRestorable(id, SEL, NSApplication*) { return YES; }

void addDelegateMethods(Class cls) {
    if (!cls) return;
    class_addMethod(cls, @selector(application:openURLs:), (IMP)openURLs, "v@:@@");
    class_addMethod(cls, @selector(applicationSupportsSecureRestorableState:),
                    (IMP)secureRestorable, "c@:@");
}

NSArray<NSString*>* recentPaths() {
    NSArray* a = [[NSUserDefaults standardUserDefaults] arrayForKey:kRecentKey];
    NSMutableArray<NSString*>* out = [NSMutableArray array];
    for (id x in a)
        if ([x isKindOfClass:[NSString class]] &&
            [[NSFileManager defaultManager] fileExistsAtPath:x])
            [out addObject:x];
    return out;
}

} // namespace

// ── menu target ─────────────────────────────────────────────────────────────
@interface BKMenuTarget : NSObject <NSMenuDelegate, NSMenuItemValidation>
@end

@implementation BKMenuTarget
- (void)perform:(NSMenuItem*)item {
    g_commands.push_back(macapp::Command(item.tag));
}
- (void)openRecent:(NSMenuItem*)item {
    NSString* path = item.representedObject;
    if (path) queueOpened(@[ [NSURL fileURLWithPath:path] ]);
}
- (void)clearRecent:(id)sender {
    (void)sender;
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kRecentKey];
    [[NSDocumentController sharedDocumentController] clearRecentDocuments:nil];
}
- (void)about:(id)sender {
    (void)sender;
    NSString* text = @"Painterly rendering on the GPU, with editable historical brush textures.\n\n"
                      "Interface type: Schibsted Grotesk and Geist Mono (SIL OFL 1.1).";
    NSDictionary* opts = @{
        NSAboutPanelOptionCredits : [[NSAttributedString alloc]
            initWithString:text
                attributes:@{NSFontAttributeName : [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName : NSColor.secondaryLabelColor}],
    };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    using C = macapp::Command;
    if (item.action == @selector(openRecent:) || item.action == @selector(clearRecent:))
        return !g_state.busy;
    if (item.action != @selector(perform:)) return YES;
    const C c = C(item.tag);
    if (c == C::ViewPainted || c == C::ViewSplit || c == C::ViewSource) {
        const int v = c == C::ViewPainted ? 0 : c == C::ViewSplit ? 1 : 2;
        item.state = g_state.view == v ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (c == C::PlayPause) {
        item.title = g_state.playing ? @"Pause" : @"Play";
        return g_state.video && !g_state.busy;
    }
    return !g_state.busy;
}
// Open Recent is rebuilt each time it opens, so it never lists a moved file.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu != g_recentMenu) return;
    [menu removeAllItems];
    NSArray<NSString*>* paths = recentPaths();
    for (NSString* p in paths) {
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:p.lastPathComponent
                                                    action:@selector(openRecent:)
                                             keyEquivalent:@""];
        it.target = self;
        it.representedObject = p;
        it.toolTip = p;
        NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFile:p];
        icon.size = NSMakeSize(16, 16);
        it.image = icon;
        [menu addItem:it];
    }
    if (paths.count) [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* clear = [[NSMenuItem alloc] initWithTitle:@"Clear Menu"
                                                   action:@selector(clearRecent:)
                                            keyEquivalent:@""];
    clear.target = self;
    clear.enabled = paths.count > 0;
    [menu addItem:clear];
}
@end

namespace {

NSMenuItem* item(NSMenu* menu, NSString* title, macapp::Command c, NSString* key = @"",
                 NSEventModifierFlags mods = NSEventModifierFlagCommand) {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:@selector(perform:) keyEquivalent:key];
    it.target = g_target;
    it.tag = NSInteger(c);
    it.keyEquivalentModifierMask = mods;
    [menu addItem:it];
    return it;
}

NSMenuItem* sys(NSMenu* menu, NSString* title, SEL action, NSString* key = @"",
                NSEventModifierFlags mods = NSEventModifierFlagCommand) {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    it.keyEquivalentModifierMask = mods;
    [menu addItem:it];
    return it;
}

NSMenu* submenu(NSMenu* bar, NSString* title) {
    NSMenuItem* top = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    NSMenu* m = [[NSMenu alloc] initWithTitle:title];
    top.submenu = m;
    [bar addItem:top];
    return m;
}

void buildMenuBar() {
    using C = macapp::Command;
    g_target = [BKMenuTarget new];
    NSMenu* bar = [NSMenu new];
    NSString* app = @"Paintify";

    NSMenu* appMenu = submenu(bar, app);
    NSMenuItem* about = [[NSMenuItem alloc] initWithTitle:@"About Paintify"
                                                   action:@selector(about:) keyEquivalent:@""];
    about.target = g_target;
    [appMenu addItem:about];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenu* services = [NSMenu new];
    sys(appMenu, @"Services", nil).submenu = services;
    NSApp.servicesMenu = services;
    [appMenu addItem:[NSMenuItem separatorItem]];
    sys(appMenu, @"Hide Paintify", @selector(hide:), @"h");
    sys(appMenu, @"Hide Others", @selector(hideOtherApplications:), @"h",
        NSEventModifierFlagCommand | NSEventModifierFlagOption);
    sys(appMenu, @"Show All", @selector(unhideAllApplications:));
    [appMenu addItem:[NSMenuItem separatorItem]];
    sys(appMenu, @"Quit Paintify", @selector(terminate:), @"q");

    NSMenu* file = submenu(bar, @"File");
    item(file, @"Open Image…", C::OpenImage, @"o");
    item(file, @"Open Video…", C::OpenVideo);
    item(file, @"Open Images as Batch…", C::OpenImages);
    item(file, @"Open Folder as Batch…", C::OpenFolder);
    NSMenuItem* recent = sys(file, @"Open Recent", nil);
    g_recentMenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    g_recentMenu.delegate = g_target;
    recent.submenu = g_recentMenu;
    [file addItem:[NSMenuItem separatorItem]];
    sys(file, @"Close Window", @selector(performClose:), @"w");
    [file addItem:[NSMenuItem separatorItem]];
    item(file, @"Save Look…", C::SaveLook, @"s");
    item(file, @"Open Look…", C::OpenLook, @"o",
         NSEventModifierFlagCommand | NSEventModifierFlagShift);
    [file addItem:[NSMenuItem separatorItem]];
    item(file, @"Export…", C::Export, @"e");
    item(file, @"Save Snapshot as PNG", C::Snapshot, @"e",
         NSEventModifierFlagCommand | NSEventModifierFlagShift);

    // Standard edit actions, aimed down the responder chain. Nothing there
    // answers them, so they stay disabled and the keystrokes fall through to
    // the text fields, which handle copy, paste and undo themselves.
    NSMenu* edit = submenu(bar, @"Edit");
    sys(edit, @"Undo", @selector(undo:), @"z");
    sys(edit, @"Redo", @selector(redo:), @"z", NSEventModifierFlagCommand | NSEventModifierFlagShift);
    [edit addItem:[NSMenuItem separatorItem]];
    sys(edit, @"Cut", @selector(cut:), @"x");
    sys(edit, @"Copy", @selector(copy:), @"c");
    sys(edit, @"Paste", @selector(paste:), @"v");
    sys(edit, @"Select All", @selector(selectAll:), @"a");

    NSMenu* view = submenu(bar, @"View");
    item(view, @"Painted", C::ViewPainted, @"1");
    item(view, @"Split", C::ViewSplit, @"2");
    item(view, @"Source", C::ViewSource, @"3");
    [view addItem:[NSMenuItem separatorItem]];
    item(view, @"Actual Size", C::ZoomActual, @"0");
    item(view, @"Zoom to Fit", C::ZoomFit, @"9");
    item(view, @"Zoom In", C::ZoomIn, @"+");
    item(view, @"Zoom Out", C::ZoomOut, @"-");
    [view addItem:[NSMenuItem separatorItem]];
    item(view, @"Play", C::PlayPause);
    [view addItem:[NSMenuItem separatorItem]];
    item(view, @"Reload Shaders", C::ReloadShaders, @"r");
    [view addItem:[NSMenuItem separatorItem]];
    sys(view, @"Enter Full Screen", @selector(toggleFullScreen:), @"f",
        NSEventModifierFlagCommand | NSEventModifierFlagControl);

    NSMenu* window = submenu(bar, @"Window");
    sys(window, @"Minimize", @selector(performMiniaturize:), @"m");
    sys(window, @"Zoom", @selector(performZoom:));
    [window addItem:[NSMenuItem separatorItem]];
    sys(window, @"Bring All to Front", @selector(arrangeInFront:));
    NSApp.windowsMenu = window;

    NSMenu* help = submenu(bar, @"Help");
    item(help, @"Keyboard Shortcuts", C::ShowShortcuts, @"/");
    NSApp.helpMenu = help;

    NSApp.mainMenu = bar;
}

// GUI apps start with launchd's PATH (/usr/bin:/bin:/usr/sbin:/sbin), which
// never contains Homebrew's ffmpeg. A copy bundled in Contents/MacOS wins.
void extendPath() {
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";
    std::string extra;
    NSString* exeDir = [NSBundle mainBundle].executablePath.stringByDeletingLastPathComponent;
    if (exeDir) extra += std::string(exeDir.fileSystemRepresentation) + ":";
    for (const char* d : {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"})
        if (path.find(d) == std::string::npos) extra += std::string(d) + ":";
    path = extra + path;
    setenv("PATH", path.c_str(), 1);
}

// Launched from Finder the working directory is "/", which is read-only, and
// relative defaults (the snapshot file, frames_out) would fail. Work in
// ~/Pictures/Paintify as the Windows installer's shortcut does. Started from a
// shell the directory is left alone, so relative command-line paths still work.
void chooseWorkingFolder() {
    char cwd[4096] = {};
    const bool rootCwd = !getcwd(cwd, sizeof(cwd)) || std::string(cwd) == "/";
    if (!rootCwd) return;
    NSURL* pictures = [[NSFileManager defaultManager] URLsForDirectory:NSPicturesDirectory
                                                             inDomains:NSUserDomainMask].firstObject;
    if (!pictures) return;
    NSURL* dir = [pictures URLByAppendingPathComponent:@"Paintify" isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:dir withIntermediateDirectories:YES
                                              attributes:nil error:nil];
    if (chdir(dir.fileSystemRepresentation) != 0) perror("chdir ~/Pictures/Paintify");
}

} // namespace

namespace macapp {

void preInit() {
    // A pipe to ffmpeg that closes early must fail the write, not kill the app.
    std::signal(SIGPIPE, SIG_IGN);
    extendPath();
    chooseWorkingFolder();
    // GLFW would chdir into Contents/Resources and build its own minimal
    // menu; both are handled here instead.
    glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
    glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_FALSE);
    // GLFW's delegate class exists from load time; giving it openURLs now
    // means even the file that launched the app is not missed.
    addDelegateMethods(objc_getClass("GLFWApplicationDelegate"));
    // No "Show Tab Bar" in a single-window app.
    NSWindow.allowsAutomaticWindowTabbing = NO;
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        @"NSFullScreenMenuItemEverywhere" : @NO,
        @"ApplePressAndHoldEnabled" : @NO,   // held keys repeat (B, arrows) instead of accents
    }];
}

void setupWindow(GLFWwindow* win, float r, float g, float b) {
    @autoreleasepool {
        if (NSApp.delegate) addDelegateMethods([NSApp.delegate class]);
        // A Dock icon and a menu bar even when run straight from the build
        // tree rather than as Paintify.app.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        buildMenuBar();

        NSWindow* w = glfwGetCocoaWindow(win);
        // The title bar takes the chrome colour, so it reads as one surface
        // with the app's own top bar instead of a grey strip above it.
        w.titlebarAppearsTransparent = YES;
        w.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
        w.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        w.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
        w.tabbingMode = NSWindowTabbingModeDisallowed;

        // Pinch zooms like Preview; a two-finger scroll pans, a mouse wheel
        // still zooms. GLFW forwards neither distinction, so watch for them.
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify | NSEventMaskScrollWheel
                                              handler:^NSEvent*(NSEvent* e) {
            if (e.type == NSEventTypeMagnify) g_magnify += float(e.magnification);
            else g_trackpadScroll = e.hasPreciseScrollingDeltas;
            return e;
        }];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

Command nextCommand() {
    if (g_commands.empty()) return Command::None;
    const Command c = g_commands.front();
    g_commands.pop_front();
    return c;
}

std::vector<std::string> takeOpenedPaths() {
    std::lock_guard<std::mutex> lock(g_openedLock);
    std::vector<std::string> out;
    out.swap(g_opened);
    return out;
}

void noteRecent(const std::string& path) {
    @autoreleasepool {
        std::error_code ec;
        const std::string abs = std::filesystem::absolute(path, ec).string();
        NSString* p = @(abs.c_str());
        NSMutableArray* list = [recentPaths() mutableCopy];
        [list removeObject:p];
        [list insertObject:p atIndex:0];
        while (list.count > kRecentMax) [list removeLastObject];
        [[NSUserDefaults standardUserDefaults] setObject:list forKey:kRecentKey];
        [[NSDocumentController sharedDocumentController]
            noteNewRecentDocumentURL:[NSURL fileURLWithPath:p]];
    }
}

float takeMagnification() {
    const float m = g_magnify;
    g_magnify = 0.f;
    return m;
}

bool scrollIsTrackpad() { return g_trackpadScroll; }

void setMenuState(const MenuState& s) { g_state = s; }

void setRepresentedFile(GLFWwindow* win, const std::string& path) {
    @autoreleasepool {
        NSWindow* w = glfwGetCocoaWindow(win);
        NSString* p = path.empty() ? @"" : @(path.c_str());
        if (![w.representedFilename isEqualToString:p]) w.representedFilename = p;
    }
}

void setDocumentEdited(GLFWwindow* win, bool edited) {
    NSWindow* w = glfwGetCocoaWindow(win);
    if (w.documentEdited != edited) w.documentEdited = edited;
}

float backingScale(GLFWwindow* win) {
    NSWindow* w = glfwGetCocoaWindow(win);
    return w ? float(w.backingScaleFactor) : 1.f;
}

} // namespace macapp
