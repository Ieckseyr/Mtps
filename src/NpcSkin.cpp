#include "NpcSkin.h"
#include "NpcSkinSteve.h"
#include "Config.h"

#include <hologramlib/HologramLib.h>

#include <ll/api/mod/NativeMod.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace mtps {
namespace skins {

namespace {

ll::io::Logger& log() { return ll::mod::NativeMod::current()->getLogger(); }

hologramlib::IPlayerNpc& npcs() { return hologramlib::IHologramLib::getInstance().playerNpcs(); }

// 默认皮肤文件不存在就生成一份（存在则完全不动它, 用户放的皮肤说了算）
void ensureDefaultFile() {
    auto const png = Config::npcSkinsDir() / (std::string(kDefaultId) + ".png");
    std::error_code ec;
    if (fs::exists(png, ec) || fs::is_directory(Config::npcSkinsDir() / kDefaultId, ec)) return;
    Canvas c;
    makeSteveSkin(c);
    if (writePng(png, c)) {
        log().info("[皮肤] 内置默认皮肤已生成: {}（覆盖它即可换成你自己的史蒂夫皮肤）", png.string());
    } else {
        log().warn("[皮肤] 内置默认皮肤写入失败: {}（请手动放一张 default.png 到该目录）", png.string());
    }
}

// ── 目录扫描

bool readFileInto(fs::path const& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

// <id>.bin = HologramLib 皮肤快照（skinId 在 blob 里, 不用猜文件名）
bool registerBlobFile(fs::path const& p) {
    std::string blob;
    if (!readFileInto(p, blob)) return false;
    return npcs().registerSkinFromBlob(blob);
}

// 快照文件名是 skinId 转义来的（非 [A-Za-z0-9_.-] 的字节写成 %XX, 与 MHR 的存法一致）。
// 反解出来只是为了"这个皮肤已经注册过就跳过" —— 否则后扫的目录会把先扫的覆盖掉。
std::string blobIdFromFileName(std::string const& stem) {
    std::string out;
    auto        hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < stem.size(); ++i) {
        if (stem[i] == '%' && i + 2 < stem.size()) {
            int const h = hex(stem[i + 1]);
            int const l = hex(stem[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back((char)(h * 16 + l));
                i += 2;
                continue;
            }
        }
        out.push_back(stem[i]);
    }
    return out;
}

bool registerPngFile(fs::path const& png, std::string const& skinId, std::string geometryData = {}) {
    if (skinId.empty()) return false;
    hologramlib::PlayerNpcSkin s;
    s.pngPath      = png.string();
    s.skinId       = skinId;
    s.geometryData = std::move(geometryData);   // 空 = 标准玩家模型
    return npcs().registerSkin(s);
}

// 子文件夹 = 一套皮肤: 里面第一张 *.png 当贴图, 第一个 *.json 当几何
// （与库 importSkins 的取法一致; 自己实现是为了能"已注册就跳过"地做优先级）
bool registerFolder(fs::path const& sub) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto const& f : fs::directory_iterator(sub, ec)) {
        if (ec) break;
        if (f.is_regular_file(ec) && !ec) files.push_back(f.path());
    }
    std::sort(files.begin(), files.end());
    std::string png, geom;
    for (auto const& p : files) {
        std::string ext = p.extension().string();
        for (char& ch : ext) ch = (char)::tolower((unsigned char)ch);
        if (ext == ".png" && png.empty()) {
            png = p.string();
        } else if (ext == ".json" && geom.empty()) {
            readFileInto(p, geom);
        }
    }
    if (png.empty()) return false;
    return registerPngFile(png, sub.filename().string(), std::move(geom));
}

struct ScanStat {
    int blobs = 0, folders = 0, pngs = 0, skipped = 0, failed = 0;
};

// 扫一个目录: 快照 / 子文件夹 / 散落 PNG 三种形式都认
// 已经注册过的 skinId 一律跳过 —— 于是"先扫的目录优先"（自己的目录 > 额外目录）
void scanDir(fs::path const& dir, ScanStat& st) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return;

    for (auto const& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (char& ch : ext) ch = (char)::tolower((unsigned char)ch);
        if (ext != ".bin") continue;
        if (npcs().hasSkin(blobIdFromFileName(e.path().stem().string()))) { st.skipped++; continue; }
        if (registerBlobFile(e.path())) st.blobs++; else st.failed++;
    }

    std::vector<fs::path> subs;
    std::vector<fs::path> pngs;
    for (auto const& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (e.is_directory()) subs.push_back(e.path());
        else if (e.is_regular_file() && e.path().extension() == ".png") pngs.push_back(e.path());
    }
    std::sort(subs.begin(), subs.end());
    std::sort(pngs.begin(), pngs.end());

    for (auto const& sub : subs) {
        if (npcs().hasSkin(sub.filename().string())) { st.skipped++; continue; }
        if (registerFolder(sub)) st.folders++; else st.failed++;
    }
    for (auto const& png : pngs) {
        std::string const id = png.stem().string();
        if (npcs().hasSkin(id)) { st.skipped++; continue; }
        if (registerPngFile(png, id)) st.pngs++; else st.failed++;
    }
}

std::vector<fs::path> allDirs() {
    std::vector<fs::path> dirs;
    dirs.push_back(Config::npcSkinsDir());
    for (auto& d : Config::getInstance().skinExtraDirs()) {
        if (!d.empty()) dirs.emplace_back(d);
    }
    return dirs;
}

} // namespace

int refresh(bool verbose) {
    ensureDefaultFile();

    ScanStat total;
    int      scanned = 0;
    for (auto const& dir : allDirs()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) continue;
        ScanStat st;
        scanDir(dir, st);
        scanned++;
        total.blobs += st.blobs;
        total.folders += st.folders;
        total.pngs += st.pngs;
        total.failed += st.failed;
        if (verbose && (st.blobs || st.folders || st.pngs || st.failed || st.skipped)) {
            log().info("[皮肤] {}: 快照 {} / 文件夹 {} / PNG {} / 失败 {} / 跳过(已注册) {}",
                       dir.string(), st.blobs, st.folders, st.pngs, st.failed, st.skipped);
        }
    }
    int const usable = (int)npcs().getSkinIds().size();
    if (verbose) {
        if (total.failed) {
            log().warn("[皮肤] 有 {} 个皮肤注册失败（PNG 需为 64x64 或 128x128）", total.failed);
        }
        log().info("[皮肤] 可用皮肤 {} 个（扫描目录 {} 个; 含 MHR 等其他插件注册的）", usable, scanned);
    }
    return usable;
}

bool ensure(std::string const& skinId) {
    if (skinId.empty()) return false;
    if (npcs().hasSkin(skinId)) return true;

    std::error_code ec;
    for (auto const& dir : allDirs()) {
        if (!fs::is_directory(dir, ec) || ec) continue;
        // <id>.png
        auto png = dir / (skinId + ".png");
        if (fs::is_regular_file(png, ec) && !ec && registerPngFile(png, skinId)) return true;
        // <id>/ 子文件夹
        auto sub = dir / skinId;
        if (fs::is_directory(sub, ec) && !ec && registerFolder(sub)) return true;
        // <safeId>.bin —— 文件名是转义的 skinId, 逐个试到命中为止
        // （不能试第一个就 break: 目录里可能有多个快照, 想要的那个未必排在最前）
        for (auto const& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".bin") continue;
            if (registerBlobFile(e.path()) && npcs().hasSkin(skinId)) return true;
        }
    }
    return npcs().hasSkin(skinId);
}

std::string resolve(std::string const& skinId) {
    if (!skinId.empty() && skinId != kDefaultId) {
        if (ensure(skinId)) return skinId;
        log().warn("[皮肤] 皮肤 '{}' 不可用（npc_skins 里没有?）, 已回落到默认皮肤", skinId);
    }
    if (ensure(kDefaultId)) return kDefaultId;
    log().error("[皮肤] 默认皮肤也不可用, NPC 无法创建: 请放一张 default.png 到 {}",
                Config::npcSkinsDir().string());
    return {};
}

std::vector<std::string> available() {
    auto ids = npcs().getSkinIds();
    std::sort(ids.begin(), ids.end());
    // 默认皮肤排最前（下拉框默认项）
    auto it = std::find(ids.begin(), ids.end(), kDefaultId);
    if (it != ids.end()) std::rotate(ids.begin(), it, it + 1);
    return ids;
}

} // namespace skins
} // namespace mtps
