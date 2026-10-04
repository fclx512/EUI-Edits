#define NANOSVG_IMPLEMENTATION
#include "3rd/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "3rd/nanosvgrast.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const int outputSize = std::atoi(argv[2]);
    if (outputSize < 1 || outputSize > 4096) return 2;
    NSVGimage* svg = nsvgParseFromFile(argv[1], "px", 96.0f);
    if (!svg) return 3;
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (!rasterizer) { nsvgDelete(svg); return 4; }

    const int sampleSize = outputSize * 8;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(sampleSize) * sampleSize * 4);
    const float scale = std::min(sampleSize / svg->width, sampleSize / svg->height);
    nsvgRasterize(rasterizer, svg, 0.0f, 0.0f, scale, pixels.data(),
                  sampleSize, sampleSize, sampleSize * 4);
    FILE* output = nullptr;
    if (fopen_s(&output, argv[3], "wb") != 0 || !output) {
        nsvgDeleteRasterizer(rasterizer); nsvgDelete(svg); return 5;
    }
    const bool ok = std::fwrite(pixels.data(), 1, pixels.size(), output) == pixels.size();
    std::fclose(output);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(svg);
    return ok ? 0 : 6;
}
