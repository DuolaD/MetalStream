#pragma once

// metalstream.ini ([MetalStream] section, next to the DLL): every key, its default and valid range
// live in config.cpp. Missing keys (or no file) use the defaults; invalid values are corrected and
// reported by LogSummary().

namespace metalstream {

struct Config {
    int enableLogging;   // EnableLogging: 1 = write metalstream.log
    int debug;           // Debug: 1 = include verbose per-frame logs (with EnableLogging=1)
    int frameGeneration; // FrameGeneration: 0 = force off, 1 = follow the game's DLSS-G options
    int debugView;       // FGDebugView: 1 = present only the generated frames
    int pacing;          // FGPacing: 1 = paced present thread without vsync, 0 = back-to-back presents
    int maxGameFps;      // FGMaxGameFps: 0 = no cap, -1 = half the display refresh rate, N = N fps
    int cameraData;      // FGCameraData: 1 = forward camera vectors / view-to-clip matrix
    int capture;         // FGCapture: 1 = F10 frame capture, 2 = also one automatic burst

    // Read once, on first use. Never logs (the logger itself reads its settings from here).
    static const Config& Get();
    // Logs every effective value and where it came from, plus any corrections (once per process)
    static void LogSummary();
};

} // namespace metalstream
