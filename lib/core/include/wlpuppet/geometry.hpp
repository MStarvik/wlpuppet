#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace wlp {

struct PointF {
    double x = 0;
    double y = 0;

    friend constexpr PointF operator+(PointF a, PointF b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr PointF operator-(PointF a, PointF b) { return {a.x - b.x, a.y - b.y}; }
};

struct Size {
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct SizeF {
    double width = 0;
    double height = 0;
};

struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] constexpr bool contains(PointF p) const {
        return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height;
    }
};

struct RectF {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;

    [[nodiscard]] constexpr bool contains(PointF p) const {
        return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height;
    }
    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr RectF united(const RectF& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const double x0 = std::min(x, o.x), y0 = std::min(y, o.y);
        const double x1 = std::max(x + width, o.x + o.width), y1 = std::max(y + height, o.y + o.height);
        return {x0, y0, x1 - x0, y1 - y0};
    }
};

/// wl_region: the result of a sequence of rectangle additions and subtractions.
class Region {
public:
    static Region infinite() {
        Region r;
        r.infinite_ = true;
        return r;
    }

    void add(Rect rect) { ops_.emplace_back(true, rect); }
    void subtract(Rect rect) { ops_.emplace_back(false, rect); }

    [[nodiscard]] bool contains(PointF p) const {
        bool inside = infinite_;
        for (const auto& [add, rect] : ops_) {
            if (rect.contains(p)) {
                inside = add;
            }
        }
        return inside;
    }

private:
    bool infinite_ = false;
    std::vector<std::pair<bool, Rect>> ops_;
};

} // namespace wlp
