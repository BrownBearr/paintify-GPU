#pragma once

#include "params.h"
#include "pipeline.h"

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

struct LiveSpoutConfig {
    std::string inputName = "Brushkit Input";
    std::string outputName = "Brushkit Output";
    std::string stopFile; // optional per-process shutdown signal
    uint32_t parentPid = 0; // exit if the owning TouchDesigner process exits
    double fps = 30.0;
    // Called when the incoming resolution is first known or changes, so a
    // brushkit style can rescale its strokes to the live frame.
    std::function<void(int w, int h, TuningParams&, RenderConfig&)> onResize;
};

// Run the live bridge on the GL thread until the window closes.
int runLiveSpout(GLFWwindow* window, Pipeline& pipe, TuningParams params,
                 const RenderConfig& render, const LiveSpoutConfig& live);
