// ---------------------------------------------------------------------------
//  L2 · contract_check.cpp —— 契约自检
//
//  ★ 这一讲唯一的可执行文件。它**不调用任何模块方法**（那些还没有实现），
//    只做两件事：
//      ① 编译期检查：类型关系对不对（static_assert）
//      ② 运行期检查：预算分配算得对不对
//
//  为什么需要一个这样的文件：
//    L2 的验收标准是"types.hpp 编译通过"。但"能编译"本身很弱 ——
//    它不能证明契约是**自洽**的。contract_check 把"自洽"变成可执行判据。
//
//  用法：
//      .\build.ps1 -Out contract_check.exe
//      .\build\contract_check.exe
// ---------------------------------------------------------------------------

#include <cstdio>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "agent_loop.hpp"
#include "context_budget.hpp"
#include "http_transport.hpp"
#include "model_provider.hpp"
#include "sandbox.hpp"
#include "tool_registry.hpp"
#include "types.hpp"

// ===========================================================================
//  一、编译期契约
//
//  这些 static_assert 是"设计意图的可执行版本"。
//  谁要是哪天改坏了契约，编译就会在这里失败 —— 而且报错信息直接说明白
//  "为什么这条约束存在"，而不是让你去猜。
// ===========================================================================

// ---- 1. 流式事件必须用变体，且事件类型数量锁定 ----
static_assert(std::variant_size_v<af::ChatChunk> == 8,
    "ChatChunk 的事件类型数量变了。"
    "每加一种事件，所有 std::visit 点都必须重新检查是否穷尽处理 —— "
    "漏掉 DoneChunk 的后果是工具调用永远不落地。");

// ---- 2. 四个端口 + 权限策略必须是抽象类 ----
static_assert(std::is_abstract_v<af::IModelProvider>,  "IModelProvider 必须是端口（有纯虚函数）");
static_assert(std::is_abstract_v<af::IHttpTransport>,  "IHttpTransport 必须是端口");
static_assert(std::is_abstract_v<af::IUIPort>,         "IUIPort 必须是端口");
static_assert(std::is_abstract_v<af::ISandbox>,        "ISandbox 必须是端口");
static_assert(std::is_abstract_v<af::IPermissionGate>, "IPermissionGate 必须是抽象策略");

// ---- 3. Message 不能有虚函数 ----
static_assert(!std::is_polymorphic_v<af::Message>,
    "Message 不该有虚函数。它要被大量拷贝（每轮请求、每次压缩都会复制历史），"
    "vtable 会让拷贝变贵，而且值语义的 Message 才能安全地放进 vector。");

// ---- 4. 具体类必须可实例化（L3 起要用） ----
static_assert(!std::is_abstract_v<af::ToolRegistry>,
              "ToolRegistry 应该是可实例化的具体类");
static_assert(!std::is_abstract_v<af::DefaultPermissionGate>,
              "DefaultPermissionGate 应该覆盖全部纯虚函数");
static_assert(!std::is_abstract_v<af::NoUI>,
              "NoUI 应该覆盖全部纯虚函数");

// ---- 5. Turn 必须是可默认构造、可移动的普通聚合 ----
static_assert(std::is_default_constructible_v<af::Turn>,
              "Loop 需要调用方直接 `Turn turn;` 构造一个");
static_assert(std::is_move_constructible_v<af::Turn>,
              "Turn 要能被移动（放进容器、被压缩重排）");

// ---- 6. 枚举必须是作用域枚举（enum class） ----
// 否则 `if (role == 1)` 这种隐式转换会编译通过，语义全丢
static_assert(!std::is_convertible_v<af::Role,         int>, "Role 必须用 enum class");
static_assert(!std::is_convertible_v<af::RiskLevel,    int>, "RiskLevel 必须用 enum class");
static_assert(!std::is_convertible_v<af::FinishReason, int>, "FinishReason 必须用 enum class");
static_assert(!std::is_convertible_v<af::LoopOutcome,  int>, "LoopOutcome 必须用 enum class");
static_assert(!std::is_convertible_v<af::Layer,        int>, "Layer 必须用 enum class");

// ---- 7. JSON 类型必须是 nlohmann::json（跨模块一致） ----
static_assert(std::is_same_v<af::json, nlohmann::json>,
              "json 别名必须指向 nlohmann::json，否则跨模块传参会编译不过");

// ===========================================================================
//  二、运行期检查
// ===========================================================================

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail.empty() ? "" : "  —— ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

// ---------------------------------------------------------------------------
//  检查 1：各层配额之和不能超过可用量
//
//  ★ 这是本讲最值得自动化的一个检查。
//    sysFrac + projectFrac + repoMapFrac + retrievedFrac + activeFrac > 1.0 时，
//    历史层（L5）会拿到**负预算** —— 症状是"历史莫名其妙地消失"，
//    完全不会报错，只会表现为"模型开始忘记前面做的事"。
//    这种 bug 在 L7 调参时极易发生，所以在这里就把它钉住。
// ---------------------------------------------------------------------------
void checkBudget() {
    std::printf("\n[预算分配检查]  (contextWindow=%d)\n", af::BudgetConfig{}.contextWindow);

    af::BudgetConfig cfg;
    const int avail = cfg.available();

    struct Row { const char* name; af::Layer layer; int budget; };
    const std::vector<Row> rows = {
        {"L0 系统提示  ", af::Layer::L0_System,      cfg.forLayer(af::Layer::L0_System)},
        {"L1 项目知识  ", af::Layer::L1_Project,     cfg.forLayer(af::Layer::L1_Project)},
        {"L2 仓库地图  ", af::Layer::L2_RepoMap,     cfg.forLayer(af::Layer::L2_RepoMap)},
        {"L3 检索命中  ", af::Layer::L3_Retrieved,   cfg.forLayer(af::Layer::L3_Retrieved)},
        {"L4 活动文件  ", af::Layer::L4_ActiveFiles, cfg.forLayer(af::Layer::L4_ActiveFiles)},
    };

    int sum = 0;
    for (const auto& r : rows) {
        std::printf("    %s %7d token  (%4.1f%%)\n", r.name, r.budget,
                    100.0 * double(r.budget) / double(avail));
        sum += r.budget;
    }
    const int history = avail - sum;
    std::printf("    %s %7d token  (%4.1f%%)   ← 剩余全部归历史\n",
                "L5 对话历史  ", history, 100.0 * double(history) / double(avail));
    std::printf("    %s %7d token  (gated, 不计入预算)\n", "L6 用户输入  ", 0);

    check(sum + history == avail, "各层配额之和 == 可用量",
          "sum=" + std::to_string(sum) + " + history=" + std::to_string(history) +
          " , available=" + std::to_string(avail));

    check(history > 0, "历史层预算为正数",
          history > 0 ? "" :
          "各层 fraction 之和已超过 1.0 —— 历史层拿到负预算，"
          "症状是历史被静默清空，且不会报任何错");
}

// ---------------------------------------------------------------------------
//  检查 2：RiskLevel 的数值必须是有序的
//
//  权限门和沙箱都靠这个顺序做"至少需要 X 级权限"的判断。
//  顺序一旦被改乱，安全策略会静默失效。
// ---------------------------------------------------------------------------
void checkRiskOrder() {
    std::printf("\n[RiskLevel 顺序检查]\n");
    const int rd = static_cast<int>(af::RiskLevel::Read);
    const int wr = static_cast<int>(af::RiskLevel::Write);
    const int ex = static_cast<int>(af::RiskLevel::Exec);
    const int nw = static_cast<int>(af::RiskLevel::Network);

    std::printf("    Read=%d < Write=%d < Exec=%d < Network=%d\n", rd, wr, ex, nw);
    check(rd < wr && wr < ex && ex < nw, "Read < Write < Exec < Network",
          "权限判定依赖这个单调顺序（'至少 Write 级'之类）");
}

// ---------------------------------------------------------------------------
//  检查 3：文本卫生函数必须真的能挡住非法 UTF-8
//
//  这是 C-4 约束的守卫。GBK 乱码在最前面出现的就是 0xB6 0xC1 这类字节。
// ---------------------------------------------------------------------------
void checkUtf8() {
    std::printf("\n[文本卫生检查]\n");

    check(af::isValidUtf8("正常的中文 UTF-8"), "合法 UTF-8 被识别为合法");

    const std::string gbk = "\xB6\xC1";            // GBK 的「读」
    check(!af::isValidUtf8(gbk), "GBK 字节被识别为非法");

    const std::string fixed = af::sanitizeUtf8(gbk);
    check(af::isValidUtf8(fixed), "消毒后一定是合法 UTF-8",
          "输入 " + std::to_string(gbk.size()) + " 字节 → 输出 " +
          std::to_string(fixed.size()) + " 字节（非法字节被替换为 U+FFFD）");

    // 消毒必须是幂等的：已经是合法的内容一字不改
    const std::string clean = "count = 42;";
    check(af::sanitizeUtf8(clean) == clean, "合法内容不被改动（幂等）");
}

// ---------------------------------------------------------------------------
//  检查 4：看一眼主要类型的体积
//
//  不是为了"越小越好"，而是让"值语义会带来多少拷贝成本"变得可见。
//  Message 在压缩、组装、回填时会被反复拷贝（或移动），
//  体积直接影响长会话的 CPU 占用。
// ---------------------------------------------------------------------------
void reportSizes() {
    std::printf("\n[主要类型体积]\n");
    std::printf("    %-18s %5zu B\n", "Message",      sizeof(af::Message));
    std::printf("    %-18s %5zu B\n", "ToolCall",     sizeof(af::ToolCall));
    std::printf("    %-18s %5zu B\n", "ToolResult",   sizeof(af::ToolResult));
    std::printf("    %-18s %5zu B\n", "ToolDef",      sizeof(af::ToolDef));
    std::printf("    %-18s %5zu B\n", "ChatChunk",    sizeof(af::ChatChunk));
    std::printf("    %-18s %5zu B\n", "ChatRequest",  sizeof(af::ChatRequest));
    std::printf("    %-18s %5zu B\n", "Turn",         sizeof(af::Turn));
    std::printf("    %-18s %5zu B\n", "LoopLimits",   sizeof(af::LoopLimits));
    std::printf("    %-18s %5zu B\n", "BudgetConfig", sizeof(af::BudgetConfig));
}

}  // namespace

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::printf("========================================\n");
    std::printf("  L2 契约自检\n");
    std::printf("  （编译期 static_assert 已在编译时通过，\n");
    std::printf("    下面是运行期能算出来的部分）\n");
    std::printf("========================================\n");

    checkBudget();
    checkRiskOrder();
    checkUtf8();
    reportSizes();

    std::printf("\n========================================\n");
    std::printf("  结果：%d 通过 / %d 失败\n", g_pass, g_fail);
    std::printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
