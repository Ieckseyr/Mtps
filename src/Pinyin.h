#pragma once
// Pinyin.h - 拼音匹配（数据驱动: 运行时读 Meowdata/Mtps/pinyin.txt, 每行 "<汉字> <拼音>"）。
// 表缺失时拼音匹配自动跳过, 其余搜索方式不受影响。
#include <string>

namespace mtps {

// 启动时调用一次; 表不存在/读失败都不算错误
void loadPinyinTable();
bool pinyinTableLoaded();

// 逐字转换, 未收录的字符直接跳过（返回的串里不含它）
std::string pinyinFull(std::string const& utf8);      // "北京" -> "beijing"
std::string pinyinInitials(std::string const& utf8); // "北京" -> "bj"

} // namespace mtps
