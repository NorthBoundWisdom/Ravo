#include "photo_registration.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

namespace ravo::photo_merge_internal
{
Matrix multiply(const Matrix &a, const Matrix &b)
{
    Matrix out{};
    for (std::size_t y = 0; y < 3; ++y)
        for (std::size_t x = 0; x < 3; ++x)
            for (std::size_t k = 0; k < 3; ++k)
                out[y * 3 + x] += a[y * 3 + k] * b[k * 3 + x];
    return out;
}

Result<Matrix> inverse(const Matrix &a)
{
    Matrix b{a[4] * a[8] - a[5] * a[7], a[2] * a[7] - a[1] * a[8], a[1] * a[5] - a[2] * a[4],
             a[5] * a[6] - a[3] * a[8], a[0] * a[8] - a[2] * a[6], a[2] * a[3] - a[0] * a[5],
             a[3] * a[7] - a[4] * a[6], a[1] * a[6] - a[0] * a[7], a[0] * a[4] - a[1] * a[3]};
    const double det = a[0] * b[0] + a[1] * b[3] + a[2] * b[6];
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        return make_error(ErrorCode::kValidation, "Singular photo registration",
                          {{"reason", "merge_singular_transform"}});
    for (auto &v : b)
        v /= det;
    return b;
}

Point project(const Matrix &m, const Point p)
{
    const double z = m[6] * p.x + m[7] * p.y + m[8];
    if (!std::isfinite(z) || std::abs(z) < 1e-10)
        return {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    return {(m[0] * p.x + m[1] * p.y + m[2]) / z, (m[3] * p.x + m[4] * p.y + m[5]) / z};
}

bool sample(const LinearWorkingBuffer &image, const Point p, std::array<float, 3> &rgb)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0 || p.y < 0 ||
        p.x > image.width - 1.0 || p.y > image.height - 1.0)
        return false;
    const auto x = static_cast<std::uint32_t>(p.x), y = static_cast<std::uint32_t>(p.y);
    const auto x1 = std::min(x + 1, image.width - 1), y1 = std::min(y + 1, image.height - 1);
    const float dx = static_cast<float>(p.x - x), dy = static_cast<float>(p.y - y);
    for (unsigned c = 0; c < 3; ++c)
        rgb[c] = (1 - dy) * ((1 - dx) * image.rgb[(std::size_t(y) * image.width + x) * 3 + c] +
                             dx * image.rgb[(std::size_t(y) * image.width + x1) * 3 + c]) +
                 dy * ((1 - dx) * image.rgb[(std::size_t(y1) * image.width + x) * 3 + c] +
                       dx * image.rgb[(std::size_t(y1) * image.width + x1) * 3 + c]);
    return true;
}

Result<FeatureSet> find_features(const LinearWorkingBuffer &image,
                                 const CancellationToken &cancellation)
{
    // FAST-9 + deterministic BRIEF. Spatial suppression bounds descriptor work
    // and keeps correspondences distributed, rather than clustered in texture.
    const double scale = std::min(1.0, 1200.0 / std::max(image.width, image.height));
    const int w = std::max(1, int(image.width * scale)), h = std::max(1, int(image.height * scale));
    FeatureSet result;
    result.scale_x = double(image.width) / w;
    result.scale_y = double(image.height) / h;
    std::vector<float> gray(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    // Sampling loops keep both coordinates within the positive image extent,
    // including the FAST and BRIEF offsets protected by the 18-pixel margin.
    const auto gray_index = [w](const int x, const int y)
    {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
               static_cast<std::size_t>(x);
    };
    for (int y = 0; y < h; ++y)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        for (int x = 0; x < w; ++x)
        {
            std::array<float, 3> rgb{};
            (void)sample(image, {x * result.scale_x, y * result.scale_y}, rgb);
            gray[gray_index(x, y)] = std::log1p(
                std::max(0.F, .2126F * rgb[0] + .7152F * rgb[1] + .0722F * rgb[2]) * 32.F);
        }
    }
    std::vector<float> sorted = gray;
    std::sort(sorted.begin(), sorted.end());
    const float low = sorted[sorted.size() / 100], high = sorted[sorted.size() * 99 / 100];
    if (high - low < 1e-5F)
        return result;
    for (auto &v : gray)
        v = std::clamp((v - low) / (high - low), 0.F, 1.F);
    struct Corner
    {
        int x;
        int y;
        float score;
    };
    std::vector<Corner> corners;
    constexpr std::array<int, 16> cx{0, 1, 2, 3, 3, 3, 2, 1, 0, -1, -2, -3, -3, -3, -2, -1};
    constexpr std::array<int, 16> cy{-3, -3, -2, -1, 0, 1, 2, 3, 3, 3, 2, 1, 0, -1, -2, -3};
    for (int y = 18; y < h - 18; ++y)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        for (int x = 18; x < w - 18; ++x)
        {
            const float center = gray[gray_index(x, y)];
            float best = 0;
            for (std::size_t start = 0; start < 16; ++start)
            {
                float bright = 1, dark = 1;
                for (std::size_t k = 0; k < 9; ++k)
                {
                    const auto q = (start + k) % 16;
                    const float diff = gray[gray_index(x + cx[q], y + cy[q])] - center;
                    bright = std::min(bright, diff);
                    dark = std::min(dark, -diff);
                }
                best = std::max({best, bright, dark});
            }
            if (best > .035F)
                corners.push_back({x, y, best});
        }
    }
    std::stable_sort(corners.begin(), corners.end(),
                     [](auto a, auto b) { return a.score > b.score; });
    std::mt19937 random(0x5241564fU);
    std::array<std::array<int, 4>, 256> pairs{};
    for (auto &pair : pairs)
        for (auto &v : pair)
            v = int(random() % 25U) - 12;
    for (const auto &corner : corners)
    {
        if (result.features.size() >= 1200)
            break;
        bool close = false;
        for (const auto &f : result.features)
            if (std::hypot(f.point.x - corner.x, f.point.y - corner.y) < 8)
            {
                close = true;
                break;
            }
        if (close)
            continue;
        Feature feature;
        feature.point = {double(corner.x), double(corner.y)};
        // BRIEF comparisons tolerate monotone exposure changes; a fixed pattern
        // makes reopening and repeated CLI requests exactly reproducible.
        for (std::size_t bit = 0; bit < pairs.size(); ++bit)
        {
            const auto &p = pairs[bit];
            if (gray[gray_index(corner.x + p[0], corner.y + p[1])] <
                gray[gray_index(corner.x + p[2], corner.y + p[3])])
                feature.descriptor[bit / 64] |= std::uint64_t(1) << (bit % 64);
        }
        result.features.push_back(feature);
    }
    return result;
}

namespace
{
struct Correspondence
{
    Point a;
    Point b;
};
std::optional<Matrix> fit(const std::vector<Correspondence> &points,
                          const std::vector<std::size_t> &indices)
{
    // Coordinates scaled before normal equations avoid full-resolution conditioning.
    double system[8][9]{};
    for (const auto i : indices)
    {
        const auto &p = points[i];
        const double x = p.a.x / 1200, y = p.a.y / 1200, u = p.b.x / 1200, v = p.b.y / 1200;
        const double rows[2][9]{{x, y, 1, 0, 0, 0, -u * x, -u * y, u},
                                {0, 0, 0, x, y, 1, -v * x, -v * y, v}};
        for (const auto &row : rows)
            for (int a = 0; a < 8; ++a)
                for (int b = 0; b < 9; ++b)
                    system[a][b] += row[a] * row[b];
    }
    for (int column = 0; column < 8; ++column)
    {
        int pivot = column;
        for (int y = column + 1; y < 8; ++y)
            if (std::abs(system[y][column]) > std::abs(system[pivot][column]))
                pivot = y;
        if (std::abs(system[pivot][column]) < 1e-12)
            return {};
        for (int x = column; x < 9; ++x)
            std::swap(system[column][x], system[pivot][x]);
        const double divisor = system[column][column];
        for (int x = column; x < 9; ++x)
            system[column][x] /= divisor;
        for (int y = 0; y < 8; ++y)
            if (y != column)
            {
                const double factor = system[y][column];
                for (int x = column; x < 9; ++x)
                    system[y][x] -= factor * system[column][x];
            }
    }
    Matrix m{};
    for (std::size_t i = 0; i < 8; ++i)
        m[i] = system[i][8];
    m[2] *= 1200;
    m[5] *= 1200;
    m[6] /= 1200;
    m[7] /= 1200;
    m[8] = 1;
    if (!inverse(m))
        return {};
    return m;
}
int distance(const Feature &a, const Feature &b)
{
    int d = 0;
    for (std::size_t i = 0; i < 4; ++i)
        d += std::popcount(a.descriptor[i] ^ b.descriptor[i]);
    return d;
}
} // namespace

Result<PhotoMergeAlignment> register_pair(const FeatureSet &source, const FeatureSet &reference,
                                          const CancellationToken &cancellation)
{
    const auto failure = []
    {
        return make_error(
            ErrorCode::kValidation,
            "Photos could not be reliably aligned; use overlapping, textured photographs",
            {{"reason", "merge_alignment_failed"}});
    };
    if (source.features.size() < 12 || reference.features.size() < 12)
        return failure();
    std::vector<Correspondence> points;
    for (const auto &a : source.features)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        int best = 257, second = 257;
        std::size_t index = 0;
        for (std::size_t j = 0; j < reference.features.size(); ++j)
        {
            const int d = distance(a, reference.features[j]);
            if (d < best)
            {
                second = best;
                best = d;
                index = j;
            }
            else
                second = std::min(second, d);
        }
        if (best > 80 || best * 5 >= second * 4)
            continue;
        const auto &b = reference.features[index];
        int reverse = 257;
        const Feature *match = nullptr;
        for (const auto &candidate : source.features)
        {
            const int d = distance(b, candidate);
            if (d < reverse)
            {
                reverse = d;
                match = &candidate;
            }
        }
        if (match == &a)
            points.push_back({a.point, b.point});
    }
    if (points.size() < 12)
        return failure();
    std::mt19937 random(0x485244U);
    std::vector<std::size_t> best;
    for (int iteration = 0; iteration < 2000; ++iteration)
    {
        if (iteration % 32 == 0)
            if (auto active = cancellation.check(); !active)
                return active.error();
        std::vector<std::size_t> selected;
        while (selected.size() < 4)
        {
            const auto i = std::size_t(random()) % points.size();
            if (std::find(selected.begin(), selected.end(), i) == selected.end())
                selected.push_back(i);
        }
        auto m = fit(points, selected);
        if (!m)
            continue;
        std::vector<std::size_t> inliers;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const auto p = project(*m, points[i].a);
            if (std::hypot(p.x - points[i].b.x, p.y - points[i].b.y) < 3)
                inliers.push_back(i);
        }
        if (inliers.size() > best.size())
            best = std::move(inliers);
        if (best.size() == points.size())
            break;
    }
    if (best.size() < 12 || best.size() * 3 < points.size())
        return failure();
    auto m = fit(points, best);
    if (!m)
        return failure();
    double minx = 1e9, miny = 1e9, maxx = -1, maxy = -1, residual = 0;
    for (const auto i : best)
    {
        const auto p = project(*m, points[i].a);
        residual += std::hypot(p.x - points[i].b.x, p.y - points[i].b.y);
        minx = std::min(minx, points[i].a.x);
        miny = std::min(miny, points[i].a.y);
        maxx = std::max(maxx, points[i].a.x);
        maxy = std::max(maxy, points[i].a.y);
    }
    const double mean_residual = residual / static_cast<double>(best.size());
    if (maxx - minx < 24 || maxy - miny < 24 || mean_residual > 2.5)
        return failure();
    const Matrix to_small{1 / source.scale_x, 0, 0, 0, 1 / source.scale_y, 0, 0, 0, 1};
    const Matrix to_full{reference.scale_x, 0, 0, 0, reference.scale_y, 0, 0, 0, 1};
    return PhotoMergeAlignment{multiply(to_full, multiply(*m, to_small)),
                               static_cast<std::uint32_t>(best.size()),
                               mean_residual * std::max(reference.scale_x, reference.scale_y)};
}
} // namespace ravo::photo_merge_internal
