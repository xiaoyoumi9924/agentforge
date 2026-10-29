# -*- coding: utf-8 -*-
"""
make_fixture.py —— 造一个 200 文件的合成 C++ 仓库，供 L8 验收与实测取数。

★ 这个仓库是"专门为量出词法后端的短板"设计的：
  里面放了 6 类词法占位抽不到、而 tree-sitter 能抽到的符号
  （构造函数 / 析构函数 / 无常见返回类型的方法 / 模板函数 / operator 重载 /
   跨行签名）。如果两者抽出来的东西一样多，说明 fixture 没造对。

用法：
    python make_fixture.py [输出目录]
"""
import os
import sys
import shutil

MODULES = [
    ("core",    "Core"),
    ("net",     "Net"),
    ("store",   "Store"),
    ("crypto",  "Crypto"),
    ("render",  "Render"),
    ("audio",   "Audio"),
    ("sync",    "Sync"),
    ("metrics", "Metrics"),
]

TPL_HPP = '''// {rel} —— {mod} 模块（自动生成，仅用于仓库索引实测）
#pragma once

#include <string>
#include <vector>
#include <memory>
{km_inc}

namespace app {{
namespace {mod} {{

struct Config {{
    int         threads = 1;
    std::string label;
    bool        verbose = false;
}};

class Logger {{
public:
    virtual ~Logger() = default;
    virtual void write(const std::string& line) = 0;
}};

class {cls} {{
public:
    {cls}(const Config& cfg);
    ~{cls}();

    bool init();
    int  run(int argc, char** argv);
    std::string describe() const;
    void setLogger(Logger* l);
    std::vector<int> snapshot() const;

    bool operator==(const {cls}& other) const;

private:
    Config       cfg_;
    Logger*      logger_ = nullptr;
    int          state_ = 0;{km_member}
}};

template <typename T>
T clamp{Tag}(T v, T lo, T hi) {{
    return v < lo ? lo : (v > hi ? hi : v);
}}

std::string buildLabel{Tag}(const std::string& prefix,
                            int idx,
                            bool upper);

int compute{Tag}(int a, int b);

}}  // namespace {mod}
}}  // namespace app
'''

TPL_CPP = '''// {rel} —— {mod} 模块实现（自动生成，仅用于仓库索引实测）
#include "{stem}.hpp"

#include <algorithm>

namespace app {{
namespace {mod} {{

{cls}::{cls}(const Config& cfg) : cfg_(cfg) {{}}

{cls}::~{cls}() = default;

bool {cls}::init() {{
    state_ = 1;
    return true;
}}

int {cls}::run(int argc, char** argv) {{
    if (argc <= 0) return -1;
    return clamp{Tag}(argc, 0, 64);
}}

std::string {cls}::describe() const {{
    return "{cls}#" + std::to_string(state_);
}}

void {cls}::setLogger(Logger* l) {{ logger_ = l; }}

std::vector<int> {cls}::snapshot() const {{
    return std::vector<int>{{state_, (int)cfg_.threads}};
}}

bool {cls}::operator==(const {cls}& other) const {{
    return state_ == other.state_;
}}

std::string buildLabel{Tag}(const std::string& prefix,
                            int idx,
                            bool upper) {{
    std::string s = prefix + std::to_string(idx);
    if (upper) {{
        for (auto& c : s) c = (char)::toupper((unsigned char)c);
    }}
    return s;
}}

int compute{Tag}(int a, int b) {{
    return clamp{Tag}(a + b, -1000, 1000);
}}

}}  // namespace {mod}
}}  // namespace app
'''

# ★ 答案文件：验收要求"一次定位到正确文件"，这个文件就是答案。
ANSWER_HPP = '''// key_manager.hpp —— 主密钥管理
#pragma once
#include <string>
#include <vector>

namespace app {
namespace crypto {

class KeyManager {
public:
    KeyManager();
    ~KeyManager();

    // ★ 验收要找的就是它
    bool rotateMasterKey(const std::string& newId, int ttlDays);

    std::string currentKeyId() const;
    std::vector<std::string> expiredKeyIds() const;

    template <typename Fn>
    void forEachKey(Fn&& fn) {
        for (const auto& k : keys_) fn(k);
    }

private:
    std::vector<std::string> keys_;
};

}  // namespace crypto
}  // namespace app
'''

ANSWER_CPP = '''// key_manager.cpp
#include "key_manager.hpp"

namespace app {
namespace crypto {

KeyManager::KeyManager() = default;
KeyManager::~KeyManager() = default;

bool KeyManager::rotateMasterKey(const std::string& newId, int ttlDays) {
    if (newId.empty() || ttlDays <= 0) return false;
    keys_.push_back(newId);
    return true;
}

std::string KeyManager::currentKeyId() const {
    return keys_.empty() ? std::string() : keys_.back();
}

std::vector<std::string> KeyManager::expiredKeyIds() const {
    return {};
}

}  // namespace crypto
}  // namespace app
'''


def w(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "_fixture/repo200"
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.makedirs(root, exist_ok=True)

    n = 0
    # 8 个模块 × 12 个组件 × (hpp + cpp) = 192，再加答案文件 2 个 = 194
    #
    # ★ 一半的文件 include 了 key_manager.hpp。这是**故意**的：
    #   PageRank 只有在"真的有入边"时才算得出中心文件。若所有文件都对称，
    #   rank 退化成均匀分布，地图就只是"按扫描顺序截一段"——
    #   那时无论用什么排序都量不出差别。
    for mod, _cap in MODULES:
        for i in range(12):
            stem = "%s_comp%02d" % (mod, i)
            cls = "".join(p.capitalize() for p in stem.split("_"))
            tag = cls
            rel_h = "src/%s/%s.hpp" % (mod, stem)
            rel_c = "src/%s/%s.cpp" % (mod, stem)
            if mod == "crypto":
                km_inc = '#include "key_manager.hpp"'
            elif i % 2 == 0:
                km_inc = '#include "../crypto/key_manager.hpp"'
            else:
                km_inc = ""
            km_member = ('\n    app::crypto::KeyManager keyManager_;'
                         if km_inc else "")
            w(os.path.join(root, rel_h),
              TPL_HPP.format(rel=rel_h, mod=mod, cls=cls, Tag=tag,
                             km_inc=km_inc, km_member=km_member))
            w(os.path.join(root, rel_c),
              TPL_CPP.format(rel=rel_c, mod=mod, cls=cls, Tag=tag, stem=stem))
            n += 2

    w(os.path.join(root, "src/crypto/key_manager.hpp"), ANSWER_HPP)
    w(os.path.join(root, "src/crypto/key_manager.cpp"), ANSWER_CPP)
    n += 2

    # 凑满 200：再加 6 个顶层文件
    for i in range(6):
        rel = "src/app_entry%d.cpp" % i
        w(os.path.join(root, rel),
          "// %s\n#include <string>\n\nnamespace app {\n\n"
          "std::string entryName%d() { return \"entry%d\"; }\n\n"
          "}  // namespace app\n" % (rel, i, i))
        n += 1

    # ---- 忽略规则要真的被用到 ----
    w(os.path.join(root, ".gitignore"), "# 自动生成的 fixture\nbuild\nvendor\n")
    for i in range(3):
        w(os.path.join(root, "build/gen%02d.cpp" % i),
          "// 构建产物，应被忽略\nint gen%02d() { return %d; }\n" % (i, i))
    for i in range(2):
        w(os.path.join(root, "vendor/vend%02d.cpp" % i),
          "// 第三方，应被 .gitignore 忽略\nint vend%02d() { return %d; }\n" % (i, i))

    # ---- 超大文件：必须被 maxBytes 拦下，不能拖垮索引 ----
    big = os.path.join(root, "generated/huge_table.cpp")
    os.makedirs(os.path.dirname(big), exist_ok=True)
    with open(big, "w", encoding="utf-8") as f:
        f.write("// 超大生成文件（约 2.5MB），索引必须跳过它\n")
        f.write("int huge_table[] = {\n")
        for i in range(700000):
            f.write("%d," % (i % 97))
        f.write("\n};\n")

    # ---- 边界：空文件 / 只有注释 / 非法 UTF-8 ----
    w(os.path.join(root, "src/edge_empty.cpp"), "")
    w(os.path.join(root, "src/edge_comment_only.cpp"), "// 只有一行注释\n")
    with open(os.path.join(root, "src/edge_bad_utf8.cpp"), "wb") as f:
        f.write(b"// bad utf8: \xff\xfe\x80\nint badUtf8(int x) { return x; }\n")

    print("fixture -> %s" % root)
    print("  代码文件(含边界): %d" % n)


if __name__ == "__main__":
    main()
