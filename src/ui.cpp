#include "ui.h"
#include <algorithm>

UiLayout LayoutBar(float width, float height, float s) {
    const float button = 40, slider = 112, gap = 2, divider = 12, pad = 6, margin = 20;
    float barW = pad * 2 + button * 6 + slider + gap * 6 + divider;
    // Shrink the whole bar on tiny windows instead of clipping it.
    s = std::min(s, (width - 16) / barW);

    UiLayout l;
    const float bw = barW * s, bh = (button + pad * 2) * s;
    l.bar = {(width - bw) / 2, height - margin * s - bh, 0, 0};
    l.bar.r = l.bar.l + bw;
    l.bar.b = l.bar.t + bh;

    float x = l.bar.l + pad * s;
    const float y = l.bar.t + pad * s;
    auto add = [&](UiId id, float w) {
        l.items.push_back({id, {x, y, x + w * s, y + button * s}});
        x += (w + gap) * s;
    };
    add(UiId::Mute, button);
    add(UiId::Volume, slider);
    x += divider * s;
    add(UiId::Screenshot, button);
    add(UiId::Stats, button);
    add(UiId::Pin, button);
    add(UiId::Settings, button);
    add(UiId::Fullscreen, button);

    const RectF& v = l.items[1].rect;
    const float cy = (v.t + v.b) / 2;
    l.track = {v.l + 8 * s, cy - 2 * s, v.r - 12 * s, cy + 2 * s};
    return l;
}

UiId HitTest(const UiLayout& layout, float x, float y) {
    for (auto& item : layout.items)
        if (item.rect.Contains(x, y)) return item.id;
    return UiId::None;
}

float VolumeAt(const UiLayout& layout, float x) {
    return std::clamp((x - layout.track.l) / layout.track.W(), 0.0f, 1.0f);
}

std::wstring Tooltip(UiId id, const UiState& s) {
    switch (id) {
        case UiId::Mute: return s.muted ? L"Unmute  (M)" : L"Mute  (M)";
        case UiId::Volume: return L"Volume " + std::to_wstring(int(s.volume * 100 + 0.5f)) + L"%  (scroll)";
        case UiId::Screenshot: return L"Screenshot  (S)";
        case UiId::Stats: return L"Stats  (I)";
        case UiId::Pin: return s.onTop ? L"Stop keeping on top  (T)" : L"Keep on top  (T)";
        case UiId::Settings: return L"Settings";
        case UiId::Fullscreen: return s.fullscreen ? L"Exit fullscreen  (Esc)" : L"Fullscreen  (F)";
        default: return L"";
    }
}
