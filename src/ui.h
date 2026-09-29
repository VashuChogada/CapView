#pragma once
#include <string>
#include <vector>

// The auto-hiding control bar. Layout lives here so the window (hit-testing)
// and the renderer (drawing) always agree on where every button is.

enum class UiId { None, Mute, Volume, Screenshot, Stats, Pin, Settings, Fullscreen };

struct RectF {
    float l = 0, t = 0, r = 0, b = 0;
    bool Contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    float W() const { return r - l; }
    float H() const { return b - t; }
};

struct UiItem {
    UiId id;
    RectF rect;
};

struct UiLayout {
    RectF bar;
    std::vector<UiItem> items;
    RectF track;  // volume slider track, inside the Volume item
};

// What the renderer needs to draw the bar and stats panel.
struct UiState {
    bool barVisible = true;
    UiId hover = UiId::None, pressed = UiId::None;
    bool muted = false, fullscreen = false, onTop = false, stats = false;
    float volume = 1.0f;
    std::wstring statsText;
};

UiLayout LayoutBar(float width, float height, float scale);
UiId HitTest(const UiLayout& layout, float x, float y);
std::wstring Tooltip(UiId id, const UiState& s);
float VolumeAt(const UiLayout& layout, float x);
