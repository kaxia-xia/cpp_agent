#include "json.hpp"
#include "git.hpp"
#include <cassert>
#include <cstdio>

int main() {
    // 1) 代理对合并：\uD83D\uDE00 -> 😀 (U+1F600 = F0 9F 98 80)
    {
        json::Value v = json::parse("\"\\uD83D\\uDE00\"");
        const std::string& s = v.as_string();
        auto* p = reinterpret_cast<const unsigned char*>(s.data());
        assert(s.size() == 4);
        assert(p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x98 && p[3] == 0x80);
        std::printf("1) surrogate pair -> ok (%02X %02X %02X %02X)\n",
                    p[0], p[1], p[2], p[3]);
    }

    // 2) 孤立代理 -> U+FFFD (EF BF BD)
    {
        json::Value v = json::parse("\"\\uD800\"");
        const std::string& s = v.as_string();
        auto* p = reinterpret_cast<const unsigned char*>(s.data());
        assert(s.size() == 3);
        assert(p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBD);
        std::printf("2) lone surrogate -> ok (U+FFFD)\n");
    }

    // 3) UTF-8 安全截断：不把中文字符拦腰截断
    {
        std::string s = "你好世界abc";  // 每个汉字 3 字节
        // 截断到 7 字节：第 7 字节落在"世"中间，应回退到"你好"(6 字节) + "..."
        std::string t = git::utf8_truncate(s, 7);
        assert(t == "你好...");
        std::printf("3) utf8_truncate(7) -> \"%s\" (ok, no mojibake)\n", t.c_str());

        // 恰好落在字符边界
        std::string t2 = git::utf8_truncate(s, 6);
        assert(t2 == "你好...");
        std::printf("4) utf8_truncate(6) -> \"%s\" (ok, boundary)\n", t2.c_str());
    }

    std::printf("all fixes verified ✓\n");
    return 0;
}
