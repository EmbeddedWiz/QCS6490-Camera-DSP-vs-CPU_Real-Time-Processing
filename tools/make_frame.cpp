/*
 * make_frame.cpp - writes a synthetic RAW8 frame for --replay.
 *
 * The parity check compares the DSP and the CPU render of the same frame, so it
 * needs a frame in the sensor's layout and nothing more: a smooth ramp with a
 * Bayer checker on top, clipped to 8 bits. A real frame is better if you want
 * to look at the picture; this one is enough for the numbers.
 *
 *   ./build/make_frame frame.raw 640 480
 *   ./build/pipeline --replay frame.raw --width 640 --height 480 --raw8
 * --parity
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

std::uint8_t sample(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                    std::uint32_t height)
{
    /* A ramp across and down, plus a green-dominant checker where the Bayer
       pattern has a green site, so the demosaic sees real edges. */
    int value = 24 + static_cast<int>((x * 190U) / width) +
                static_cast<int>((y * 50U) / height);
    if (((x ^ y) & 1U) != 0U) {
        value += 30;
    }
    if (value > 255) {
        value = 255;
    }
    return static_cast<std::uint8_t>(value);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s PATH [WIDTH HEIGHT]\n", argv[0]);
        return 1;
    }
    const std::uint32_t width =
        argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 640U;
    const std::uint32_t height =
        argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 480U;
    if (width == 0U || height == 0U) {
        std::fprintf(stderr, "make_frame: width and height must be positive\n");
        return 1;
    }

    std::vector<std::uint8_t> row(width);
    std::FILE* file = std::fopen(argv[1], "wb");
    if (file == nullptr) {
        std::perror("make_frame: fopen");
        return 1;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            row[x] = sample(x, y, width, height);
        }
        if (std::fwrite(row.data(), 1U, row.size(), file) != row.size()) {
            std::perror("make_frame: fwrite");
            std::fclose(file);
            return 1;
        }
    }
    std::fclose(file);
    std::printf("[make_frame] %s %ux%u RAW8\n", argv[1], width, height);
    return 0;
}
