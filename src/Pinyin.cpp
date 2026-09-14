#include "Pinyin.h"

#include "Config.h"

#include <fstream>
#include <unordered_map>

namespace mtps {

namespace {

std::unordered_map<std::string, std::string>& table() {
    static std::unordered_map<std::string, std::string> t;
    return t;
}

bool& loadedFlag() {
    static bool v = false;
    return v;
}

// 取 UTF-8 串的下一个码元（返回其字节长度与内容）; 非法字节按 1 字节跳过
size_t nextCodepoint(std::string const& s, size_t pos, std::string& out) {
    if (pos >= s.size()) return 0;
    unsigned char c = (unsigned char)s[pos];
    size_t        n = 1;
    if (c >= 0xF0)      n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    if (pos + n > s.size()) n = 1;
    out.assign(s, pos, n);
    return n;
}

} // namespace

void loadPinyinTable() {
    if (loadedFlag()) return;
    loadedFlag() = true;

    auto path = Config::dataDir() / "pinyin.txt";
    std::ifstream f(path);
    if (!f.is_open()) {
        return;   // 没有表 → 拼音匹配跳过
    }

    std::string line;
    size_t      n = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // 取第一个空白前的汉字（可能是多字节）与之后的拼音
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string ch = line.substr(0, sp);
        std::string py;
        for (size_t i = sp; i < line.size(); i++) {
            char c = line[i];
            if (c == ' ' || c == '\t') continue;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            py += c;
        }
        if (!ch.empty() && !py.empty()) {
            table()[ch] = py;
            n++;
        }
    }
    (void)n;   // 条目数由调用方经 pinyinTableLoaded() 判断, 这里不打日志
}

bool pinyinTableLoaded() { return !table().empty(); }

std::string pinyinFull(std::string const& utf8) {
    if (table().empty()) return {};
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        std::string cp;
        size_t      n = nextCodepoint(utf8, i, cp);
        if (n == 0) break;
        i += n;
        auto it = table().find(cp);
        if (it != table().end()) out += it->second;
    }
    return out;
}

std::string pinyinInitials(std::string const& utf8) {
    if (table().empty()) return {};
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        std::string cp;
        size_t      n = nextCodepoint(utf8, i, cp);
        if (n == 0) break;
        i += n;
        auto it = table().find(cp);
        if (it != table().end() && !it->second.empty()) out += it->second[0];
    }
    return out;
}

} // namespace mtps
