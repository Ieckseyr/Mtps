#pragma once
// NpcSkinSteve.h - 内置默认皮肤的生成（纯 std + zlib, 不依赖框架, 便于单独验证）。
// 为什么需要自己画: 库不预置任何皮肤, 而 NPC 创建时皮肤未注册直接返回 -3 —— 不生成一份,
// "没配皮肤"的点位根本创建不出来。画的是史蒂夫配色（非原图）, 64x64 标准展开, 未用区域透明。
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

namespace mtps {
namespace skins {

struct Rgb {
    unsigned char r{}, g{}, b{};
};

// 史蒂夫配色（近似）
inline constexpr Rgb kHair{60, 42, 26};
inline constexpr Rgb kSkin{189, 142, 114};
inline constexpr Rgb kShirt{0, 170, 170};
inline constexpr Rgb kPants{63, 63, 165};
inline constexpr Rgb kShoe{76, 76, 76};
inline constexpr Rgb kEye{56, 72, 120};
inline constexpr Rgb kWhite{255, 255, 255};
inline constexpr Rgb kMouth{110, 70, 55};

struct Canvas {
    int                        w{64}, h{64};
    std::vector<unsigned char> px;   // RGBA, 初始全透明

    Canvas() : px((std::size_t)w * h * 4, 0) {}

    void rect(int x0, int y0, int rw, int rh, Rgb c) {
        for (int y = y0; y < y0 + rh; ++y) {
            if (y < 0 || y >= h) continue;
            for (int x = x0; x < x0 + rw; ++x) {
                if (x < 0 || x >= w) continue;
                std::size_t o = ((std::size_t)y * w + x) * 4;
                px[o] = c.r, px[o + 1] = c.g, px[o + 2] = c.b, px[o + 3] = 255;
            }
        }
    }

    Rgb at(int x, int y) const {
        std::size_t o = ((std::size_t)y * w + x) * 4;
        return Rgb{px[o], px[o + 1], px[o + 2]};
    }
    bool opaque(int x, int y) const { return px[((std::size_t)y * w + x) * 4 + 3] != 0; }

    // 手臂/腿: 4 个侧面按行分上下两色（上 = 袖子/裤腰, 下 = 皮肤/鞋）, 顶底各自一色
    void limb(int x, int y, Rgb upper, int upperRows, Rgb lower, Rgb bottom) {
        rect(x + 4, y, 4, 4, upper);                   // 顶
        rect(x + 8, y, 4, 4, bottom);                  // 底
        int const sx[4] = {x, x + 4, x + 8, x + 12};   // 右 / 前 / 左 / 后
        for (int i = 0; i < 4; ++i) {
            rect(sx[i], y + 4, 4, upperRows, upper);
            rect(sx[i], y + 4 + upperRows, 4, 12 - upperRows, lower);
        }
    }
};

inline void makeSteveSkin(Canvas& c) {
    // 头（8x8x8）: 六面头发, 正面留出脸
    c.rect(8, 0, 8, 8, kHair);      // 顶
    c.rect(16, 0, 8, 8, kSkin);     // 底
    c.rect(0, 8, 8, 8, kHair);      // 右
    c.rect(16, 8, 8, 8, kHair);     // 左
    c.rect(24, 8, 8, 8, kHair);     // 后
    c.rect(8, 8, 8, 8, kSkin);      // 正面: 脸
    c.rect(8, 8, 8, 2, kHair);      // 刘海
    c.rect(9, 12, 1, 1, kWhite), c.rect(10, 12, 1, 1, kEye);    // 左眼
    c.rect(13, 12, 1, 1, kEye), c.rect(14, 12, 1, 1, kWhite);   // 右眼
    c.rect(10, 14, 4, 1, kMouth);   // 嘴

    // 躯干（8x12x4）: 上衣
    c.rect(20, 16, 8, 4, kShirt);   // 顶
    c.rect(28, 16, 8, 4, kShirt);   // 底
    c.rect(16, 20, 4, 12, kShirt);  // 右
    c.rect(20, 20, 8, 12, kShirt);  // 前
    c.rect(28, 20, 4, 12, kShirt);  // 左
    c.rect(32, 20, 8, 12, kShirt);  // 后

    // 四肢: 上段 4 行是短袖/裤腰, 其余皮肤; 腿再往下是鞋
    c.limb(40, 16, kShirt, 4, kSkin, kSkin);    // 右臂
    c.limb(32, 48, kShirt, 4, kSkin, kSkin);    // 左臂
    c.limb(0, 16, kPants, 8, kShoe, kShoe);     // 右腿
    c.limb(16, 48, kPants, 8, kShoe, kShoe);    // 左腿
}

namespace detail {
inline void putBE32(std::vector<unsigned char>& v, std::uint32_t x) {
    v.push_back((unsigned char)(x >> 24));
    v.push_back((unsigned char)(x >> 16));
    v.push_back((unsigned char)(x >> 8));
    v.push_back((unsigned char)x);
}
} // namespace detail

// 最小 PNG 编码（8 位 RGBA, filter 全 0）; 只为写内置默认皮肤, 不追求通用
inline bool writePng(std::filesystem::path const& path, Canvas const& c) {
    std::vector<unsigned char> raw;
    raw.reserve((std::size_t)c.h * (1 + (std::size_t)c.w * 4));
    for (int y = 0; y < c.h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), c.px.begin() + (std::size_t)y * c.w * 4, c.px.begin() + (std::size_t)(y + 1) * c.w * 4);
    }
    uLongf                    compLen = compressBound((uLong)raw.size());
    std::vector<unsigned char> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
    comp.resize(compLen);

    std::vector<unsigned char> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&out](char const* type, std::vector<unsigned char> const& data) {
        detail::putBE32(out, (std::uint32_t)data.size());
        std::vector<unsigned char> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        out.insert(out.end(), body.begin(), body.end());
        detail::putBE32(out, (std::uint32_t)crc32(0, body.data(), (uInt)body.size()));
    };
    std::vector<unsigned char> ihdr;
    detail::putBE32(ihdr, (std::uint32_t)c.w);
    detail::putBE32(ihdr, (std::uint32_t)c.h);
    ihdr.push_back(8);            // bit depth
    ihdr.push_back(6);            // color type: RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk("IHDR", ihdr);
    chunk("IDAT", comp);
    chunk("IEND", {});

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write((char const*)out.data(), (std::streamsize)out.size());
    return f.good();
}

} // namespace skins
} // namespace mtps
