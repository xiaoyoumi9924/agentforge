// ---------------------------------------------------------------------------
//  L1 工程骨架 —— main.cpp
//
//  这一讲的 main 故意极其简单：它唯一的职责是证明「工具链是通的」。
//  不要在这里加业务逻辑，那是 L3 的事。
//
//  有意为之的三个「不做」：
//    1. 不建 namespace、不建 class —— 接口要等 L2 想清楚再定
//    2. 不提前建抽象（没有 IStrategy / Factory / Manager）
//    3. 不写任何"以后会用上"的代码
// ---------------------------------------------------------------------------

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
// NOMINMAX 必须在 windows.h 之前：否则 min/max 宏会污染 std::min/std::max，
// 报错位置完全指不到这里。
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

int main(int /*argc*/, char** /*argv*/) {
#ifdef _WIN32
    // Windows 控制台默认是 GBK 代码页，不设置的话中文输出全是乱码。
    // 平台差异从第一天就要处理，不要等到"以后统一处理"。
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::cout << "AgentForge v0.1 —— 骨架就绪\n\n";

    // 这里只是把 L1 设计文档里的三层结构打印出来，
    // 让学生第一次运行时就能看见「设计 → 代码」的对应关系。
    // 模块编号沿用 骨架.md 的 13 模块全景。
    const std::vector<std::pair<std::string, std::vector<std::string>>> layers = {
        {"交互执行层", {"⑥ 工具调用与执行", "⑪ UI 与交互", "⑬ 测试与评估"}},
        {"核心智能层", {"⑦ Agent 循环", "② 多模型适配", "④ 上下文组装",
                        "③ 仓库索引", "⑤ 增量状态"}},
        {"基础设施层", {"① 插件管理", "⑧ 安全与权限", "⑨ 可观测性",
                        "⑩ 配置", "⑫ 持久化与记忆"}},
    };

    for (const auto& [layer, mods] : layers) {
        std::cout << layer << "：";
        for (std::size_t i = 0; i < mods.size(); ++i) {
            std::cout << (i ? " · " : "") << mods[i];
        }
        std::cout << "\n";
    }

    std::cout << "\n依赖方向：交互执行 → 核心智能 → 基础设施（单向）\n";
    std::cout << "本讲的验收标准：能编译、能运行、能说清 5 个 ADR 的理由\n";
    return 0;
}
