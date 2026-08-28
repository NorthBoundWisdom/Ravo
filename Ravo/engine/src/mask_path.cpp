#include "mask_path.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace ravo
{
namespace
{

constexpr float kInvalidCoordinate = -std::numeric_limits<float>::max();
constexpr double kMinParameterStep = 0.0001;

struct Sample2
{
    float x = 0.0F;
    float y = 0.0F;
};

struct Cubic
{
    float p0x = 0.0F;
    float p0y = 0.0F;
    float p1x = 0.0F;
    float p1y = 0.0F;
    float p2x = 0.0F;
    float p2y = 0.0F;
    float p3x = 0.0F;
    float p3y = 0.0F;
};

[[nodiscard]] TaskError path_error(const std::string_view message, const std::string_view reason)
{
    return make_error(ErrorCode::kValidation, std::string(message),
                      {{"reason", std::string(reason)}});
}

[[nodiscard]] float smoothstep(const float left, const float right, const float t) noexcept
{
    return left + (right - left) * t * t * (3.0F - 2.0F * t);
}

void cubic_point(const Cubic &cubic, const float t, float &x, float &y) noexcept
{
    const float ti = 1.0F - t;
    const float a = ti * ti * ti;
    const float b = 3.0F * t * ti * ti;
    const float c = 3.0F * t * t * ti;
    const float d = t * t * t;
    x = cubic.p0x * a + cubic.p1x * b + cubic.p2x * c + cubic.p3x * d;
    y = cubic.p0y * a + cubic.p1y * b + cubic.p2y * c + cubic.p3y * d;
}

void cubic_border(const Cubic &cubic, const float t, const float radius, float &x, float &y,
                  float &bx, float &by) noexcept
{
    cubic_point(cubic, t, x, y);
    const double ti = 1.0 - static_cast<double>(t);
    const double t_t = static_cast<double>(t) * t;
    const double ti_ti = ti * ti;
    const double t_ti = static_cast<double>(t) * ti;
    const double a = 3.0 * ti_ti;
    const double b = 3.0 * (ti_ti - 2.0 * t_ti);
    const double c = 3.0 * (2.0 * t_ti - t_t);
    const double d = 3.0 * t_t;
    const double dx = -cubic.p0x * a + cubic.p1x * b + cubic.p2x * c + cubic.p3x * d;
    const double dy = -cubic.p0y * a + cubic.p1y * b + cubic.p2y * c + cubic.p3y * d;
    if (dx == 0.0 && dy == 0.0)
    {
        bx = kInvalidCoordinate;
        by = kInvalidCoordinate;
        return;
    }
    const double length = 1.0 / std::sqrt(dx * dx + dy * dy);
    bx = x + radius * static_cast<float>(dy * length);
    by = y - radius * static_cast<float>(dx * length);
}

[[nodiscard]] Result<void> tessellate_segment(const Cubic &cubic, const double tmin,
                                              const double tmax, const float radius_min,
                                              const float radius_max, const bool with_border,
                                              std::vector<Sample2> &points,
                                              std::vector<Sample2> &borders)
{
    float path_min_x = 0.0F;
    float path_min_y = 0.0F;
    float path_max_x = 0.0F;
    float path_max_y = 0.0F;
    float border_min_x = kInvalidCoordinate;
    float border_min_y = kInvalidCoordinate;
    float border_max_x = kInvalidCoordinate;
    float border_max_y = kInvalidCoordinate;
    cubic_border(cubic, static_cast<float>(tmin), smoothstep(radius_min, radius_max, static_cast<float>(tmin)),
                 path_min_x, path_min_y, border_min_x, border_min_y);
    cubic_border(cubic, static_cast<float>(tmax), smoothstep(radius_min, radius_max, static_cast<float>(tmax)),
                 path_max_x, path_max_y, border_max_x, border_max_y);

    const auto recurse = [&](auto &&self, const double left, const double right, const float x0,
                             const float y0, const float x1, const float y1, const float bx0,
                             const float by0, const float bx1, const float by1) -> Result<void>
    {
        if (points.size() >= kCanonicalMaskMaxTessellatedSamples)
        {
            return path_error("Path or brush tessellation exceeded the sample limit",
                              "mask_path_tessellation_limit");
        }
        const bool close = (right - left < kMinParameterStep) ||
                           (static_cast<int>(x0) - static_cast<int>(x1) < 1 &&
                            static_cast<int>(x0) - static_cast<int>(x1) > -1 &&
                            static_cast<int>(y0) - static_cast<int>(y1) < 1 &&
                            static_cast<int>(y0) - static_cast<int>(y1) > -1 &&
                            (!with_border || (bx0 == kInvalidCoordinate || bx1 == kInvalidCoordinate ||
                                              (static_cast<int>(bx0) - static_cast<int>(bx1) < 1 &&
                                               static_cast<int>(bx0) - static_cast<int>(bx1) > -1 &&
                                               static_cast<int>(by0) - static_cast<int>(by1) < 1 &&
                                               static_cast<int>(by0) - static_cast<int>(by1) > -1))));
        if (close)
        {
            points.push_back({x1, y1});
            if (with_border)
            {
                borders.push_back({bx1, by1});
            }
            return {};
        }
        const double mid = (left + right) / 2.0;
        float mx = 0.0F;
        float my = 0.0F;
        float mbx = kInvalidCoordinate;
        float mby = kInvalidCoordinate;
        cubic_border(cubic, static_cast<float>(mid),
                     smoothstep(radius_min, radius_max, static_cast<float>(mid)), mx, my, mbx, mby);
        auto first = self(self, left, mid, x0, y0, mx, my, bx0, by0, mbx, mby);
        if (!first)
        {
            return first.error();
        }
        return self(self, mid, right, mx, my, x1, y1, mbx, mby, bx1, by1);
    };
    return recurse(recurse, tmin, tmax, path_min_x, path_min_y, path_max_x, path_max_y, border_min_x,
                   border_min_y, border_max_x, border_max_y);
}

void path_falloff_roi(std::vector<float> &buffer, const int x0, const int y0, const int x1,
                      const int y1, const std::uint32_t width, const std::uint32_t height) noexcept
{
    const float lx = static_cast<float>(x1 - x0);
    const float ly = static_cast<float>(y1 - y0);
    const int length =
        static_cast<int>(std::sqrt(lx * lx + ly * ly)) + 1;
    const int dx = lx < 0.0F ? -1 : 1;
    const int dy = ly < 0.0F ? -1 : 1;
    const int bw = static_cast<int>(width);
    const int bh = static_cast<int>(height);
    const int dpy = dy * bw;
    for (int i = 0; i < length; ++i)
    {
        const int x = static_cast<int>(static_cast<float>(i) * lx / static_cast<float>(length)) + x0;
        const int y = static_cast<int>(static_cast<float>(i) * ly / static_cast<float>(length)) + y0;
        const float opacity = 1.0F - static_cast<float>(i) / static_cast<float>(length);
        const auto stamp = [&](const int px, const int py)
        {
            if (px < 0 || py < 0 || px >= bw || py >= bh)
            {
                return;
            }
            float &sample = buffer[static_cast<std::size_t>(py) * width + static_cast<std::size_t>(px)];
            sample = std::max(sample, opacity);
        };
        stamp(x, y);
        stamp(x + dx, y);
        static_cast<void>(dpy);
        stamp(x, y + dy);
    }
}

void brush_falloff_roi(std::vector<float> &buffer, const int x0, const int y0, const int x1,
                       const int y1, const std::uint32_t width, const std::uint32_t height,
                       const float hardness, const float density) noexcept
{
    const float lx = static_cast<float>(x1 - x0);
    const float ly = static_cast<float>(y1 - y0);
    const int length = static_cast<int>(std::sqrt(lx * lx + ly * ly)) + 1;
    const int solid = static_cast<int>(hardness * static_cast<float>(length));
    const float step_x = lx / static_cast<float>(length);
    const float step_y = ly / static_cast<float>(length);
    const int dx = lx <= 0.0F ? -1 : 1;
    const int dy = ly <= 0.0F ? -1 : 1;
    const int bw = static_cast<int>(width);
    const int bh = static_cast<int>(height);
    float fx = static_cast<float>(x0);
    float fy = static_cast<float>(y0);
    float opacity = density;
    const float dop = length == solid ? 0.0F : density / static_cast<float>(length - solid);
    for (int i = 0; i < length; ++i)
    {
        const int x = static_cast<int>(fx);
        const int y = static_cast<int>(fy);
        fx += step_x;
        fy += step_y;
        if (i > solid)
        {
            opacity -= dop;
        }
        const auto stamp = [&](const int px, const int py)
        {
            if (px < 0 || py < 0 || px >= bw || py >= bh)
            {
                return;
            }
            float &sample = buffer[static_cast<std::size_t>(py) * width + static_cast<std::size_t>(px)];
            sample = std::max(sample, opacity);
        };
        stamp(x, y);
        stamp(x + dx, y);
        stamp(x, y + dy);
    }
}

void edge_flag_fill(std::vector<float> &buffer, const std::vector<Sample2> &points,
                    const std::uint32_t width, const std::uint32_t height) noexcept
{
    if (points.size() < 2U)
    {
        return;
    }
    float xlast = points.back().x;
    float ylast = points.back().y;
    for (const auto &point : points)
    {
        float xstart = xlast;
        float ystart = ylast;
        float xend = point.x;
        float yend = point.y;
        xlast = point.x;
        ylast = point.y;
        if (ystart > yend)
        {
            std::swap(ystart, yend);
            std::swap(xstart, xend);
        }
        if (ystart == yend)
        {
            continue;
        }
        const float slope = (xstart - xend) / (ystart - yend);
        for (int yy = static_cast<int>(std::ceil(ystart)); static_cast<float>(yy) < yend; ++yy)
        {
            const float xcross = xstart + slope * (static_cast<float>(yy) - ystart);
            int xx = static_cast<int>(std::floor(xcross));
            if (static_cast<float>(xx) + 0.5F <= xcross)
            {
                ++xx;
            }
            if (xx < 0 || yy < 0 || xx >= static_cast<int>(width) ||
                yy >= static_cast<int>(height))
            {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(yy) * width + static_cast<std::size_t>(xx);
            buffer[index] = 1.0F - buffer[index];
        }
    }
    for (std::uint32_t row = 0; row < height; ++row)
    {
        int state = 0;
        for (std::uint32_t column = 0; column < width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * width + column;
            if (buffer[index] > 0.5F)
            {
                state = !state;
            }
            if (state != 0)
            {
                buffer[index] = 1.0F;
            }
        }
    }
}

[[nodiscard]] bool path_encircles_roi(const std::vector<Sample2> &points,
                                      const std::uint32_t width, const std::uint32_t height) noexcept
{
    int crossings = 0;
    int last = -9999;
    const int x = static_cast<int>(width / 2U);
    const int y = static_cast<int>(height / 2U);
    for (const auto &point : points)
    {
        const int yy = static_cast<int>(point.y);
        if (yy != last && yy == y && point.x > static_cast<float>(x))
        {
            ++crossings;
        }
        last = yy;
    }
    return (crossings & 1) != 0;
}

} // namespace

Result<std::vector<float>> evaluate_path_mask_alpha(const PathMask &path,
                                                    const MaskEvaluationRequest &request)
{
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (path.points.size() < kCanonicalMaskMinPathPoints)
    {
        return path_error("Path mask has too few points", "invalid_path_mask");
    }
    const float full_width = static_cast<float>(request.full_width);
    const float full_height = static_cast<float>(request.full_height);
    const float min_dimension = std::min(full_width, full_height);
    const float radius = static_cast<float>(path.feather) * min_dimension;
    const float roi_x = static_cast<float>(request.roi_x);
    const float roi_y = static_cast<float>(request.roi_y);
    std::vector<Sample2> samples;
    std::vector<Sample2> borders;
    samples.reserve(256U);
    if (path.feather > 0.0)
    {
        borders.reserve(256U);
    }
    for (std::size_t index = 0; index < path.points.size(); ++index)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const auto &from = path.points[index];
        const auto &to = path.points[(index + 1U) % path.points.size()];
        const Cubic cubic{static_cast<float>(from.x) * full_width - roi_x,
                          static_cast<float>(from.y) * full_height - roi_y,
                          static_cast<float>(from.ctrl2_x) * full_width - roi_x,
                          static_cast<float>(from.ctrl2_y) * full_height - roi_y,
                          static_cast<float>(to.ctrl1_x) * full_width - roi_x,
                          static_cast<float>(to.ctrl1_y) * full_height - roi_y,
                          static_cast<float>(to.x) * full_width - roi_x,
                          static_cast<float>(to.y) * full_height - roi_y};
        auto tessellated =
            tessellate_segment(cubic, 0.0, 1.0, radius, radius, path.feather > 0.0, samples, borders);
        if (!tessellated)
        {
            return tessellated.error();
        }
    }
    std::vector<float> buffer(static_cast<std::size_t>(request.roi_width) * request.roi_height, 0.0F);
    const bool samples_in_roi = std::any_of(
        samples.begin(), samples.end(),
        [&](const Sample2 &sample)
        {
            return sample.x > 1.0F && sample.y > 1.0F &&
                   sample.x < static_cast<float>(request.roi_width) - 2.0F &&
                   sample.y < static_cast<float>(request.roi_height) - 2.0F;
        });
    if (!samples_in_roi && path_encircles_roi(samples, request.roi_width, request.roi_height))
    {
        std::fill(buffer.begin(), buffer.end(), 1.0F);
        return buffer;
    }
    edge_flag_fill(buffer, samples, request.roi_width, request.roi_height);
    if (path.feather > 0.0 && borders.size() == samples.size())
    {
        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            if (borders[index].x == kInvalidCoordinate)
            {
                continue;
            }
            path_falloff_roi(buffer, static_cast<int>(std::floor(samples[index].x + 0.5F)),
                             static_cast<int>(std::ceil(samples[index].y)),
                             static_cast<int>(borders[index].x), static_cast<int>(borders[index].y),
                             request.roi_width, request.roi_height);
        }
    }
    return buffer;
}

Result<std::vector<float>> evaluate_brush_mask_alpha(const BrushMask &brush,
                                                     const MaskEvaluationRequest &request)
{
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (brush.points.size() < kCanonicalMaskMinBrushPoints)
    {
        return path_error("Brush mask has too few points", "invalid_brush_mask");
    }
    const float full_width = static_cast<float>(request.full_width);
    const float full_height = static_cast<float>(request.full_height);
    const float min_dimension = std::min(full_width, full_height);
    const float roi_x = static_cast<float>(request.roi_x);
    const float roi_y = static_cast<float>(request.roi_y);
    std::vector<float> buffer(static_cast<std::size_t>(request.roi_width) * request.roi_height, 0.0F);
    for (std::size_t index = 0; index + 1U < brush.points.size(); ++index)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const auto &from = brush.points[index];
        const auto &to = brush.points[index + 1U];
        const Cubic cubic{static_cast<float>(from.x) * full_width - roi_x,
                          static_cast<float>(from.y) * full_height - roi_y,
                          static_cast<float>(from.ctrl2_x) * full_width - roi_x,
                          static_cast<float>(from.ctrl2_y) * full_height - roi_y,
                          static_cast<float>(to.ctrl1_x) * full_width - roi_x,
                          static_cast<float>(to.ctrl1_y) * full_height - roi_y,
                          static_cast<float>(to.x) * full_width - roi_x,
                          static_cast<float>(to.y) * full_height - roi_y};
        const float radius0 = static_cast<float>(from.radius) * min_dimension;
        const float radius1 = static_cast<float>(to.radius) * min_dimension;
        std::vector<Sample2> samples;
        std::vector<Sample2> borders;
        auto tessellated = tessellate_segment(cubic, 0.0, 1.0, radius0, radius1, true, samples, borders);
        if (!tessellated)
        {
            return tessellated.error();
        }
        if (samples.size() != borders.size())
        {
            return path_error("Brush tessellation did not produce paired falloff samples",
                              "invalid_brush_mask");
        }
        for (std::size_t sample = 0; sample < samples.size(); ++sample)
        {
            if (borders[sample].x == kInvalidCoordinate)
            {
                continue;
            }
            const float t = samples.size() == 1U ?
                                1.0F :
                                static_cast<float>(sample) / static_cast<float>(samples.size() - 1U);
            const float hardness = smoothstep(static_cast<float>(from.hardness),
                                              static_cast<float>(to.hardness), t);
            const float density =
                smoothstep(static_cast<float>(from.density), static_cast<float>(to.density), t);
            brush_falloff_roi(buffer, static_cast<int>(samples[sample].x),
                              static_cast<int>(samples[sample].y), static_cast<int>(borders[sample].x),
                              static_cast<int>(borders[sample].y), request.roi_width,
                              request.roi_height, hardness, density);
        }
    }
    return buffer;
}

} // namespace ravo
