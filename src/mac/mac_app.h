#pragma once
// The macOS application shell around the GLFW window: the menu bar, Finder
// integration, the window's chrome, and trackpad gestures. main.cpp polls it
// once per frame; nothing here touches the painting.
#include <string>
#include <vector>

struct GLFWwindow;

namespace macapp {

// What a menu item asks the app to do. Menu key equivalents replace the
// Ctrl+key handling main.cpp does on Windows.
enum class Command {
    None,
    OpenImage, OpenVideo, OpenImages, OpenFolder,
    SaveLook, OpenLook, Export, Snapshot,
    ViewPainted, ViewSplit, ViewSource,
    ZoomFit, ZoomActual, ZoomIn, ZoomOut,
    PlayPause, ReloadShaders, ShowShortcuts,
};

// Before glfwInit: Finder "Open With" / Dock-drop handling, a sane PATH for
// ffmpeg (apps launched from Finder do not see the shell's), the working
// folder, and the GLFW init hints that would otherwise fight the above.
void preInit();

// After the window exists: native menu bar, title bar matching the chrome
// colour, full-screen behaviour, gesture monitors.
void setupWindow(GLFWwindow* win, float chromeR, float chromeG, float chromeB);

// Next queued menu command, or None.
Command nextCommand();

// Paths opened from Finder, the Dock icon or Open Recent since the last call.
std::vector<std::string> takeOpenedPaths();

// Remember a file in File > Open Recent and the Dock's recent items.
void noteRecent(const std::string& path);

// Trackpad state since the last call: pinch magnification (0.1 = 10%), and
// whether the last scroll came from a trackpad (pan) or a wheel (zoom).
float takeMagnification();
bool scrollIsTrackpad();

// What menu validation needs to know about the app this frame.
struct MenuState {
    bool busy = false;       // exporting: everything but quit is disabled
    bool video = false;      // Play is meaningful
    bool playing = false;
    int view = 0;            // 0 Painted, 1 Split, 2 Source
};
void setMenuState(const MenuState& s);

// Title-bar document affordances: the proxy icon for the open file (or none),
// and the dot in the close button while the look has unsaved changes.
void setRepresentedFile(GLFWwindow* win, const std::string& path);
void setDocumentEdited(GLFWwindow* win, bool edited);

// Points -> pixels for the window's current screen (2 on Retina).
float backingScale(GLFWwindow* win);

} // namespace macapp
