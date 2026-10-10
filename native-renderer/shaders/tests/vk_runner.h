// Runs a vertex / pixel SPIR-V pair on a Vulkan device (Mesa's lavapipe in CI) with the kkshaders
// binding model, drawing one full-screen triangle into four 1x1 RGBA32F targets.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VkRunner {
public:
    VkRunner();
    ~VkRunner();
    bool init(std::string* error);
    std::string deviceName() const;

    struct Draw {
        std::vector<uint8_t> vs, ps;                     // SPIR-V
        std::vector<uint8_t> vertexConstants;            // b0 (4096 bytes)
        std::vector<uint8_t> pixelConstants;             // b1 (4096 bytes)
        std::vector<uint8_t> drawConstants;              // b2 (abi.h DrawConstants)
        std::vector<std::vector<uint8_t>> buffers;       // storage buffers, descriptor indices 0..7
        // 2D textures (RGBA32F, descriptor indices 0..7) and samplers (0: point clamp, 1: linear clamp).
        struct Texture {
            uint32_t width = 1, height = 1;
            std::vector<float> texels;                   // RGBA
        };
        std::vector<Texture> textures;
    };
    struct Result {
        bool ok = false;
        std::string error;
        std::array<std::array<float, 4>, 4> targets{};
    };
    Result run(const Draw& draw);

    static constexpr float kClear = -12345.0f;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
