#ifndef LOCAL_BRISS_CROP_H_
#define LOCAL_BRISS_CROP_H_

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <limits>

// Header-only grayscale white-margin detection. Pixels are 0=black, 255=white.
// Rectangle coordinates are half-open: [left,right) x [top,bottom).
// Deliberately free of exceptions so it can be used from translation units that
// are built with exceptions disabled.
namespace briss_crop {

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

namespace detail {

inline bool Band(const double* p, int n, int margin, int* begin, int* end) {
    if (n < 3) {
        return false;
    }
    double peak = p[0];
    double low = p[0];
    for (int i = 1; i < n; i++) {
        if (p[i] > peak) {
            peak = p[i];
        }
        if (p[i] < low) {
            low = p[i];
        }
    }
    if (peak < 1.0 || peak - low < 0.5) {
        return false;
    }
    int window = n / 100 + 2;
    if (window < 2) {
        window = 2;
    }
    if (window > 16) {
        window = 16;
    }
    double threshold = peak * 0.015;
    if (threshold < 0.5) {
        threshold = 0.5;
    }
    // prefix sums of value, of successive differences, and of their squares
    double* sums = (double*)calloc((std::size_t)n + 1, sizeof(double));
    double* diffs = (double*)calloc((std::size_t)n + 1, sizeof(double));
    double* squares = (double*)calloc((std::size_t)n + 1, sizeof(double));
    if (!sums || !diffs || !squares) {
        free(sums);
        free(diffs);
        free(squares);
        return false;
    }
    for (int i = 0; i < n; i++) {
        double d = i ? p[i] - p[i - 1] : 0.0;
        sums[i + 1] = sums[i] + p[i];
        diffs[i + 1] = diffs[i] + d;
        squares[i + 1] = squares[i] + d * d;
    }
    int first = n;
    int last = 0;
    for (int a = 0; a < n; a++) {
        int span = window < (n - a) ? window : (n - a);
        int b = a + span;
        double count = (double)(b - a);
        double mean = (sums[b] - sums[a]) / count;
        double dm = (diffs[b] - diffs[a]) / count;
        double variance = (squares[b] - squares[a]) / count - dm * dm;
        if (variance < 0) {
            variance = 0;
        }
        if (mean >= threshold && (mean >= threshold * 2.0 || std::sqrt(variance) >= threshold)) {
            if (a < first) {
                first = a;
            }
            last = b;
        }
    }
    free(sums);
    free(diffs);
    free(squares);
    if (first >= last) {
        return false;
    }
    *begin = margin >= first ? 0 : first - margin;
    *end = margin >= n - last ? n : last + margin;
    return true;
}

// Scanned paper is neither white nor even: a gray book page has paper around
// 200, an uneven scan drifts by tens of levels across the page, and a scan has
// faint noise everywhere. So "darker than 245" is the wrong rule - on a gray
// page it calls every pixel ink and nothing is cropped. Instead each tile of
// the image gets its own paper level (its 75th percentile, so a tile that also
// holds text still knows where its paper is) and ink is what is clearly darker
// than that level. A tile that is dark all over (a picture) is clamped upwards,
// otherwise its content would look like background and be cropped away.
struct BackgroundMap {
    int tileSize = 1;
    int nx = 0;
    int ny = 0;
    float* values = nullptr;

    ~BackgroundMap() {
        free(values);
    }

    bool Build(const std::uint8_t* data, int width, int height, std::ptrdiff_t stride, int hi, int floorLevel) {
        tileSize = 24;
        if (tileSize > width) {
            tileSize = width;
        }
        if (tileSize > height) {
            tileSize = height;
        }
        if (tileSize < 4) {
            tileSize = 4;
        }
        nx = (width + tileSize - 1) / tileSize;
        ny = (height + tileSize - 1) / tileSize;
        values = (float*)calloc((std::size_t)nx * (std::size_t)ny, sizeof(float));
        if (!values) {
            return false;
        }
        int hist[256];
        for (int ty = 0; ty < ny; ty++) {
            for (int tx = 0; tx < nx; tx++) {
                std::memset(hist, 0, sizeof(hist));
                int x0 = tx * tileSize;
                int x1 = x0 + tileSize;
                if (x1 > width) {
                    x1 = width;
                }
                int y0 = ty * tileSize;
                int y1 = y0 + tileSize;
                if (y1 > height) {
                    y1 = height;
                }
                for (int y = y0; y < y1; y++) {
                    const std::uint8_t* row = data + (std::size_t)y * stride;
                    for (int x = x0; x < x1; x++) {
                        hist[row[x]]++;
                    }
                }
                long long count = (long long)(x1 - x0) * (long long)(y1 - y0);
                long long acc = 0;
                int paper = hi;
                for (int v = 0; v < 256; v++) {
                    acc += hist[v];
                    if (count > 0 && acc * 4 >= count * 3) {
                        paper = v;
                        break;
                    }
                }
                if (paper < floorLevel) {
                    paper = floorLevel;
                }
                if (paper > hi) {
                    paper = hi;
                }
                values[(std::size_t)ty * (std::size_t)nx + (std::size_t)tx] = (float)paper;
            }
        }
        return true;
    }

    // Paper level at a pixel, interpolated between tile centres so the estimate
    // is continuous instead of stepping at tile borders.
    float At(int x, int y) const {
        float fx = ((float)x + 0.5f) / (float)tileSize - 0.5f;
        float fy = ((float)y + 0.5f) / (float)tileSize - 0.5f;
        int x0 = (int)fx;
        if (fx < 0) {
            x0 = 0;
            fx = 0;
        }
        int y0 = (int)fy;
        if (fy < 0) {
            y0 = 0;
            fy = 0;
        }
        if (x0 > nx - 1) {
            x0 = nx - 1;
        }
        if (y0 > ny - 1) {
            y0 = ny - 1;
        }
        float tx = fx - (float)x0;
        if (tx > 1) {
            tx = 1;
        }
        float ty = fy - (float)y0;
        if (ty > 1) {
            ty = 1;
        }
        int x1 = x0 + 1 < nx ? x0 + 1 : nx - 1;
        int y1 = y0 + 1 < ny ? y0 + 1 : ny - 1;
        float top = values[(std::size_t)y0 * (std::size_t)nx + (std::size_t)x0] * (1 - tx) +
                    values[(std::size_t)y0 * (std::size_t)nx + (std::size_t)x1] * tx;
        float bottom = values[(std::size_t)y1 * (std::size_t)nx + (std::size_t)x0] * (1 - tx) +
                       values[(std::size_t)y1 * (std::size_t)nx + (std::size_t)x1] * tx;
        return top * (1 - ty) + bottom * ty;
    }
};

}  // namespace detail

// data points to the first row; stride is its positive size in bytes and must
// be >= width. The caller owns at least (height-1)*stride+width readable bytes.
// margin is extra pixels on each side, clamped to the image. Returns false for
// invalid, blank, low-contrast, or otherwise undetectable images; on failure
// *out is unchanged. A successful result is a heuristic, not a guarantee that
// faint marks or isolated noise are preserved.
inline bool Detect(const std::uint8_t* data, int width, int height, std::ptrdiff_t stride, Rect* out,
                   int margin = 4) {
    if (!data || !out || width < 3 || height < 3 || stride < width || margin < 0) {
        return false;
    }
    if (margin > 100000) {
        margin = 100000;
    }
    std::size_t w = (std::size_t)width;
    std::size_t h = (std::size_t)height;
    std::size_t s = (std::size_t)stride;
    if (h > ((std::size_t)PTRDIFF_MAX - w) / s) {
        return false;
    }

    // 1. How much contrast the page has at all: the darkest level present, and
    //    the 90th percentile, which is the paper level.
    int hist[256];
    std::memset(hist, 0, sizeof(hist));
    for (int y = 0; y < height; y++) {
        const std::uint8_t* row = data + (std::size_t)y * s;
        for (int x = 0; x < width; x++) {
            hist[row[x]]++;
        }
    }
    long long total = (long long)width * (long long)height;
    int dark = 0;
    while (dark < 255 && !hist[dark]) {
        dark++;
    }
    long long acc = 0;
    int paper = 255;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc * 10 >= total * 9) {
            paper = v;
            break;
        }
    }
    int contrast = paper - dark;
    if (contrast < 10) {
        // a uniform page has no content to find: leave it alone
        return false;
    }
    int minInk = (int)((double)contrast * 0.15);
    if (minInk < 10) {
        minInk = 10;
    }
    int floorLevel = dark + (int)((double)contrast * 0.35);

    // 2. Paper level per tile, so a gray or unevenly lit page reads as paper
    //    instead of as one big black splash.
    detail::BackgroundMap background;
    if (!background.Build(data, width, height, stride, paper, floorLevel)) {
        return false;
    }

    double* columns = (double*)calloc(w, sizeof(double));
    double* rows = (double*)calloc(h, sizeof(double));
    if (!columns || !rows) {
        free(columns);
        free(rows);
        return false;
    }
    // 3. Ink = clearly darker than the paper level of its own tile.
    for (int y = 0; y < height; y++) {
        const std::uint8_t* row = data + (std::size_t)y * s;
        for (int x = 0; x < width; x++) {
            int limit = (int)background.At(x, y) - minInk;
            if (row[x] < limit) {
                double ink = (double)(limit - row[x]);
                rows[y] += ink;
                columns[x] += ink;
            }
        }
    }
    for (int y = 0; y < height; y++) {
        rows[y] /= (double)width;
    }
    for (int x = 0; x < width; x++) {
        columns[x] /= (double)height;
    }

    Rect result;
    bool ok = detail::Band(columns, width, margin, &result.left, &result.right) &&
              detail::Band(rows, height, margin, &result.top, &result.bottom);
    if (ok) {
        // 4. The band detector looks a window ahead, so its edges land a few
        //    pixels outside the content. Pull each edge in while it holds no ink
        //    at all: only edges that are really empty move, so no ink is cut.
        auto tighten = [](const double* p, int n, int* begin, int* end) {
            int b = *begin;
            int e = *end;
            while (b < e - 3 && p[b] < 1.0) {
                b++;
            }
            while (e > b + 3 && p[e - 1] < 1.0) {
                e--;
            }
            *begin = b;
            *end = e;
        };
        tighten(columns, width, &result.left, &result.right);
        tighten(rows, height, &result.top, &result.bottom);
    }
    free(columns);
    free(rows);
    if (!ok || result.right <= result.left || result.bottom <= result.top) {
        return false;
    }
    *out = result;
    return true;
}

}  // namespace briss_crop

// Convenience interface with a four-pixel safety margin. Outputs change only on
// success; right and bottom are exclusive.
inline bool BrissCropDetect(const unsigned char* gray, int width, int height, int stride, int& left, int& top,
                            int& right, int& bottom) {
    briss_crop::Rect rect;
    if (!briss_crop::Detect(gray, width, height, stride, &rect)) {
        return false;
    }
    left = rect.left;
    top = rect.top;
    right = rect.right;
    bottom = rect.bottom;
    return true;
}

#endif  // LOCAL_BRISS_CROP_H_
