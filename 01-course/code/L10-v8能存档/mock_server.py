# ---------------------------------------------------------------------------
#  L7 · mock_server.py —— 假服务端（坏模型发生器 + 第二协议 + 压缩摘要器）
#
#  L5 的版本会按历史推进多步脚本，用来证明"环转起来了"。
#  L6 的版本加了"各种坏掉的模型"，用来**逐道闸门**证明每种死法都被拦住。
#  L7 的版本加了三件事，每一件都是被本讲的代码**逼出来的**：
#
#    ① 非流式响应（stream=false）
#       L7 的压缩器要调一次 `chat()`（非流式）来要交接摘要。
#       旧 mock 不管 `stream` 字段，一律回 SSE → 客户端把 SSE 当 JSON parse
#       → 直接抛异常。**"客户端引入第一条非流式请求"这件事，
#       只有把链路真跑起来才会发现。**
#
#    ② Anthropic 形状（路径 /messages）
#       第二家厂商的事件名、消息结构、tools 形状全都不同。
#       一个只会说 OpenAI 的假服务端，验证不了第二个适配器。
#
#    ③ 压缩摘要 + grow 模式
#       `grow` 模式会连续读 N 个真文件（每次不同 → 绕开闸门 5），
#       把上下文顶到窗口之上，逼出压缩。压缩请求由一个"摘要桩"应答 ——
#       它会**从收到的报文里抽出原始需求和文件路径**再回填。
#       于是"H5：连续读 50 个文件后还记不记得最初的需求"就变成了
#       一条**端到端可断言**的事实：摘要里出现过的路径，
#       在最后一条请求里必须还能找到。
#
#  模式表（沿用 L6，新增最后几个）：
#    repeat      永远原样重发同一个调用       闸门 5
#    spin        每次换参数、毫无进展         闸门 1
#    sleepy      每次都慢                     闸门 3
#    fail        一直报错                     闸门 4
#    slow        长篇慢慢吐                   闸门 6
#    slowcmd     要跑很久的命令               沙箱超时
#    netcmd      要联网下载                   沙箱禁网
#    danger      要删库                       权限门 Deny
#    rename      正经干活（回归）             ——
#    grow        ★ L7：连续读 N(50) 个真文件  压缩级联
#    bigout      ★ L7：一条命令产出超大输出  L0 落盘
#    chat        什么都不调用                 ——
#
#  运行：
#      python mock_server.py [port] [logfile]
# ---------------------------------------------------------------------------
import json
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
LOG = sys.argv[2] if len(sys.argv) > 2 else "mock_requests.log"

CHUNK_STEP = 37                      # 任意字节边界切开再发
USAGE = {"prompt_tokens": 96, "completion_tokens": 18, "total_tokens": 114}

# grow 模式要读的文件。**必须是真实存在的文件**（Read 会去磁盘上取），
# 而且加起来要足够大 —— 不然顶不到窗口之上，压缩永远不会触发。
#
# ★ H5 的验收词是"连续读 **50** 个文件"，所以这里必须凑够 50 个**互不相同**的
#   真实文件（参数一样就会被闸门 5 判成重复调用）。
#   本讲目录只有 24 个源文件，因此后 26 个走相对路径去上一讲的目录借
#   —— 顺带也更接近真实仓库的形态（一个仓库里的文件当然散落在不同子目录）。
GROW_FILES = [
    # ---- 本讲目录的 24 个文件 ----
    "compaction.hpp", "compaction.cpp", "context_budget.hpp", "context_budget.cpp",
    "agent_loop.hpp", "agent_loop.cpp", "main.cpp", "providers.cpp",
    "tool_registry.hpp", "tool_registry.cpp", "plugin.hpp", "plugin.cpp",
    "sandbox.hpp", "sandbox_win.cpp", "http_winhttp.cpp", "http_transport.hpp",
    "sse_parser.hpp", "sse_parser.cpp", "model_provider.hpp", "types.hpp",
    "mock_server.py", "verify.ps1", "build.ps1", "mock_target.txt",
    # ---- 上一讲目录的 26 个文件（相对路径，凑满 50）----
    "../L6-v4能刹车/code/agent_loop.cpp", "../L6-v4能刹车/code/agent_loop.hpp",
    "../L6-v4能刹车/code/build.ps1", "../L6-v4能刹车/code/context_budget.cpp",
    "../L6-v4能刹车/code/context_budget.hpp", "../L6-v4能刹车/code/http_transport.hpp",
    "../L6-v4能刹车/code/http_winhttp.cpp", "../L6-v4能刹车/code/main.cpp",
    "../L6-v4能刹车/code/mock_server.py", "../L6-v4能刹车/code/mock_target.txt",
    "../L6-v4能刹车/code/model_provider.hpp", "../L6-v4能刹车/code/plugin.cpp",
    "../L6-v4能刹车/code/plugin.hpp", "../L6-v4能刹车/code/providers.cpp",
    "../L6-v4能刹车/code/sandbox_win.cpp", "../L6-v4能刹车/code/sandbox.hpp",
    "../L6-v4能刹车/code/sse_parser.cpp", "../L6-v4能刹车/code/sse_parser.hpp",
    "../L6-v4能刹车/code/tool_registry.cpp", "../L6-v4能刹车/code/tool_registry.hpp",
    "../L6-v4能刹车/code/types.hpp", "../L6-v4能刹车/code/verify.ps1",
    "../L6-v4能刹车/code/work_target.txt",
    "../L6-v4能刹车/code/_c1.txt", "../L6-v4能刹车/code/_c3.txt",
    "../L6-v4能刹车/code/_c4.txt",
]

# ★ 脚手架必须自己记状态（K-20，与 L5 的 K-10 同一族）
#
#   第一版把 grow 的步数写成"数一数请求里有几条 tool 消息"。看起来天经地义 ——
#   直到 L7 引入压缩：**压缩会把老消息删掉**，于是 tool 消息的条数不再单调增长，
#   mock 于是反复发同一个文件 → 被闸门 5 判成死循环 →
#   整个用例失败，而**被测代码其实是对的**。
#
#   这是"验证脚手架依赖了被测对象"的典型形态：
#   脚手架拿被测系统的状态来驱动自己，那么被测系统一改变行为，
#   脚手架就会给出**错误结论**。正确做法是让脚手架自己数自己的步数。
#
#   （排查顺序也值得记住：先看服务端日志的 [MODE] 行，确认"模型到底说了什么"，
#     再去怀疑被测代码 —— 否则会去改一个本来正确的产品，把 bug 制造出来。）
#
# ★ 再进一步（K-22）：计数器必须**按会话分桶**，不能只有一个全局计数器。
#   本讲的验证脚本要跑**两遍** grow（一遍压缩开、一遍压缩关，做 A/B 对比）。
#   一个全局计数器会让第二遍从"第 51 步"开始 —— 直接跳到收工分支，
#   一个文件都不读。于是 A/B 里 B 组什么都没测到，而日志上看起来"跑完了"。
#   分桶的键用**最后一条 user 消息**（= 本次任务，整个用例期间不变）。
#   这里有一个反直觉的点：看起来更自然的分桶键是"第一条 user 消息"（锚点），
#   但压缩会把交接摘要插成 history 的头 —— 摘要也是 user，
#   于是锚点会在压缩那一刻**变成摘要文本**，计数器立刻从头开始，
#   mock 又去读第 1 个文件 → 撞闸门 5。**分桶键必须是压缩动不了的东西。**
GROW_STEP = {}


def grow_step(anchor):
    GROW_STEP[anchor] = GROW_STEP.get(anchor, 0) + 1
    return GROW_STEP[anchor]


# ★ 摘要桩必须**跨轮累积**路径（K-23）。
#
#   压缩是**反复发生**的：第 1 轮把 20 条老消息压成一份摘要，第 2 轮又把
#   "之前那份摘要 + 新的一批老消息"再压一遍。如果摘要桩只从**当前**报文里
#   抽路径，那么第 2 轮里"早就被压掉的那些消息"已经不在报文里了 →
#   它们携带的路径**凭空消失** → H5 的"还记得读过哪些文件"就会在
#   某一轮之后悄悄失效。而这正是 kCompactionPrompt 第 3 条要防的失败模式：
#   **压缩丢掉路径，省下来的钱是假的。**
#
#   真实实现（ExtractiveSummarizer）是靠"继承上一轮的 previousSummary"来防它的；
#   摘要桩没有那个字段可用，于是用一个模块级的累积表达到同样效果。
SUMMARY_PATHS = []


def log(line):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


# ---------------------------------------------------------------------------
#  OpenAI 形状
# ---------------------------------------------------------------------------
def frame(obj):
    return "data: " + json.dumps(obj, ensure_ascii=False) + "\n\n"


def chunk(delta, finish=None, usage=None):
    ch = {"index": 0, "delta": delta}
    if finish is not None:
        ch["finish_reason"] = finish
    out = {"id": "chatcmpl-mock", "object": "chat.completion.chunk",
           "created": 0, "model": "mock-1", "choices": [ch]}
    out["usage"] = usage                # ★ 多数帧是 null —— 客户端必须判 is_null
    return frame(out)


def usage_frame():
    return frame({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                  "created": 0, "model": "mock-1", "choices": [], "usage": USAGE})


def split4(s):
    L = len(s)
    cuts = [0, L // 4, L // 2, (3 * L) // 4, L]
    return [s[cuts[i]:cuts[i + 1]] for i in range(4)]


def tool_call_stream(call_id, name, args_obj):
    args_json = json.dumps(args_obj, ensure_ascii=False)
    stream = [": keepalive\n\n"]

    # 起头帧：name 出现，arguments 是**空串**（真实服务端就这么发）
    stream.append(chunk({"role": "assistant", "content": None,
                         "tool_calls": [{"index": 0, "id": call_id, "type": "function",
                                         "function": {"name": name, "arguments": ""}}]},
                        usage=None))
    pieces = split4(args_json)
    for i, p in enumerate(pieces):
        last = (i == len(pieces) - 1)
        stream.append(chunk({"tool_calls": [{"index": 0,
                                             "function": {"arguments": p}}]},
                            # ★ 最后一段参数与 finish_reason 同帧
                            finish="tool_calls" if last else None,
                            usage=None))
    stream.append(usage_frame())
    stream.append("data: [DONE]\n\n")
    return stream


def text_stream(text):
    stream = [
        ": keepalive\n\n",
        chunk({"content": ""}, usage=None),                    # 首帧空串
        chunk({"role": "assistant", "content": None}, usage=None),
    ]
    for i in range(0, len(text), 7):
        stream.append(chunk({"content": text[i:i + 7]}, usage=None))
    stream.append(chunk({"content": ""}, finish="stop"))       # finish 同帧
    stream.append(usage_frame())
    stream.append("data: [DONE]\n\n")
    return stream


def slow_text_stream(paragraphs=400):
    """很长、很多帧的正文 —— 给"取消"和"墙钟"当靶子。

    拉长的办法是**帧多**而不是帧大：do_POST 每 37 字节 flush 一次并 sleep，
    所以总时长 ≈ 总字节 / 37 * 0.01s。400 段大约 8~9 秒。
    """
    stream = [
        ": keepalive\n\n",
        chunk({"content": ""}, usage=None),
        chunk({"role": "assistant", "content": None}, usage=None),
    ]
    for i in range(paragraphs):
        stream.append(chunk({"content": "第%04d段：这一句用来把流拉长，便于验证取消与墙钟。\n" % i},
                            usage=None))
    stream.append(chunk({"content": ""}, finish="stop"))
    stream.append(usage_frame())
    stream.append("data: [DONE]\n\n")
    return stream


def openai_nonstream(text):
    return {"id": "chatcmpl-mock", "object": "chat.completion", "created": 0,
            "model": "mock-1",
            "choices": [{"index": 0,
                         "message": {"role": "assistant", "content": text},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 120, "completion_tokens": 40,
                      "total_tokens": 160}}


# ---------------------------------------------------------------------------
#  ★ L7：Anthropic 形状
#
#  三处与 OpenAI 的结构性差异（对应 providers.cpp 里 AnthropicProvider 做的三件事）：
#    · 事件**带名字**（event: xxx），而 OpenAI 只有 data:
#    · 正文在 content_block_delta / text_delta，工具参数在 input_json_delta
#    · 用量被拆到两处：input 在 message_start，output 在 message_delta
# ---------------------------------------------------------------------------
def aframe(etype, obj):
    return "event: " + etype + "\ndata: " + json.dumps(obj, ensure_ascii=False) + "\n\n"


def a_start(in_tokens=25):
    return aframe("message_start", {
        "type": "message_start",
        "message": {"id": "msg_mock", "type": "message", "role": "assistant",
                    "model": "mock-1", "content": [],
                    "usage": {"input_tokens": in_tokens, "output_tokens": 1}}})


def anthropic_text_stream(text, paragraphs=1):
    s = [a_start()]
    s.append(aframe("content_block_start", {
        "type": "content_block_start", "index": 0,
        "content_block": {"type": "text", "text": ""}}))
    for i in range(0, len(text), 7):
        s.append(aframe("content_block_delta", {
            "type": "content_block_delta", "index": 0,
            "delta": {"type": "text_delta", "text": text[i:i + 7]}}))
    s.append(aframe("content_block_stop", {"type": "content_block_stop", "index": 0}))
    s.append(aframe("message_delta", {
        "type": "message_delta", "delta": {"stop_reason": "end_turn"},
        "usage": {"output_tokens": 18}}))
    s.append(aframe("message_stop", {"type": "message_stop"}))
    return s


def anthropic_slow_text_stream(paragraphs=400):
    s = [a_start()]
    s.append(aframe("content_block_start", {
        "type": "content_block_start", "index": 0,
        "content_block": {"type": "text", "text": ""}}))
    for i in range(paragraphs):
        s.append(aframe("content_block_delta", {
            "type": "content_block_delta", "index": 0,
            "delta": {"type": "text_delta",
                      "text": "第%04d段：这一句用来把流拉长，便于验证取消与墙钟。\n" % i}}))
    s.append(aframe("content_block_stop", {"type": "content_block_stop", "index": 0}))
    s.append(aframe("message_delta", {"type": "message_delta",
                                      "delta": {"stop_reason": "end_turn"},
                                      "usage": {"output_tokens": 18}}))
    s.append(aframe("message_stop", {"type": "message_stop"}))
    return s


def anthropic_tool_stream(call_id, name, args_obj):
    args_json = json.dumps(args_obj, ensure_ascii=False)
    s = [a_start()]
    # ★ 工具调用的名字只在 content_block_start 出现一次
    s.append(aframe("content_block_start", {
        "type": "content_block_start", "index": 0,
        "content_block": {"type": "tool_use", "id": call_id, "name": name, "input": {}}}))
    for p in split4(args_json):
        s.append(aframe("content_block_delta", {
            "type": "content_block_delta", "index": 0,
            "delta": {"type": "input_json_delta", "partial_json": p}}))
    s.append(aframe("content_block_stop", {"type": "content_block_stop", "index": 0}))
    s.append(aframe("message_delta", {"type": "message_delta",
                                      "delta": {"stop_reason": "tool_use"},
                                      "usage": {"output_tokens": 20}}))
    s.append(aframe("message_stop", {"type": "message_stop"}))
    return s


def anthropic_nonstream(text):
    return {"id": "msg_mock", "type": "message", "role": "assistant", "model": "mock-1",
            "content": [{"type": "text", "text": text}],
            "stop_reason": "end_turn",
            "usage": {"input_tokens": 120, "output_tokens": 40}}


def anthropic_tool_nonstream(call_id, name, args_obj):
    return {"id": "msg_mock", "type": "message", "role": "assistant", "model": "mock-1",
            "content": [{"type": "tool_use", "id": call_id, "name": name,
                         "input": args_obj}],
            "stop_reason": "tool_use",
            "usage": {"input_tokens": 120, "output_tokens": 40}}


# ---------------------------------------------------------------------------
#  ★ L7：压缩摘要桩
#
#  它做的是"抽取式摘要"该做的事：从收到的报文里挑出**交接最不能丢的两样东西** ——
#  原始需求 与 涉及的文件路径 —— 然后原样回填。
#  为什么这样设计：本讲的验收要求是"连续读 50 个文件后还记不记得最初的需求"。
#  只有让摘要**把需求带回去**，这个验收才能变成可断言的事实。
# ---------------------------------------------------------------------------
def msg_text(m):
    c = m.get("content")
    if isinstance(c, list):
        parts = []
        for b in c:
            if isinstance(b, dict):
                if b.get("type") == "text":
                    parts.append(str(b.get("text", "")))
                elif b.get("type") == "tool_result":
                    parts.append(str(b.get("content", "")))
        return " ".join(parts)
    return str(c or "")


def system_text(req):
    for m in req.get("messages", []):
        if m.get("role") == "system":
            return msg_text(m)
    s = req.get("system")
    if s is None:
        return ""
    if isinstance(s, list):
        return " ".join(str(b.get("text", "")) for b in s if isinstance(b, dict))
    return str(s)


def is_compaction_request(req):
    return "CHECKPOINT COMPACTION" in system_text(req)


def build_summary(req):
    body = ""
    for m in req.get("messages", []):
        if m.get("role") == "user" and len(msg_text(m)) > len(body):
            body = msg_text(m)

    goal = ""
    mm = re.search(r"# 本任务的原始需求[^\n]*\n(.+)", body)
    if mm:
        goal = mm.group(1).strip()

    # ★ 路径要**跨轮累积**（见 SUMMARY_PATHS 的说明）：本轮新出现的补进去，
    #   已有的绝不删除 —— 摘要桩模拟的是"一份不会丢路径的好摘要"。
    for p in re.findall(r'"path"\s*:\s*"([^"]+)"', body):
        if p not in SUMMARY_PATHS:
            SUMMARY_PATHS.append(p)
    paths = list(SUMMARY_PATHS)

    lines = ["【mock 交接摘要】"]
    if goal:
        lines.append("- 原始需求：" + goal[:200])
    if paths:
        lines.append("- 涉及的文件/路径（累计 %d 个）：" % len(paths))
        for p in paths[:40]:
            lines.append("  · " + p)
    lines.append("- 说明：更早的工具结果已被压缩；请以上面的需求与路径为准，"
                 "不要重复探索已经读过的东西。")
    return "\n".join(lines)


def is_summary_message(m):
    """判定"这条消息是不是压缩器插入的交接摘要"。

    ★ 判据必须**排除工具结果**（K-25，本讲第三个脚手架坑）。

      为什么：grow 模式要读 50 个真文件，其中就有 `mock_server.py` ——
      而那个文件里**含有**字符串 `【mock 交接摘要】`（它就是生成这个标记的地方）。
      于是"在上下文里找摘要"悄悄变成了"在上下文里找 mock_server.py 的源码"：

        · C19（压缩关掉）本该看到"没有摘要"，却因为在读源码而**声称**看到了摘要
          → 用例失败，而去查产品代码只会一无所获（产品是对的）
        · 更糟的是断言方向反过来的时候：它会**替 bug 背书** ——
          "摘要还在！"在一个根本没有摘要的会话里被"证明"了

      一句话：**验证脚本的证据不能由被测内容自己伪造。**
      凡是"从上下文里认出某个东西"的判据，都要先问一句：
      "如果被读进来的文件里恰好有这段字，会发生什么？"

      摘要消息的特征：role == user 且 content 是**字符串**。
      工具结果两条路都被排除：
        · OpenAI  → role == "tool"
        · Anthropic → 折进 user，但 content 是**数组**（含 tool_result block）
    """
    if m.get("role") != "user":
        return False
    return isinstance(m.get("content"), str)


def summary_in_request(req):
    """在请求里找"压缩摘要"这条消息，并取出它列出的第一个路径。

    ★ 这就是"记忆还在不在"的**证据**：那条摘要消息是压缩器插进去的，
      它列的路径来自**已经被压掉**的那些消息。它还在，说明记忆还在。

    ★ 正则必须锚定"路径条目的行首"（`^  · `），不能只找 `· `：
      摘要消息的标题行是 `[上下文压缩 · 交接摘要] ...` —— 里面也有一个 `· `。
      只找 `· ` 会把标题中的"交接摘要]"当成路径取出来，
      于是断言"路径还在"**通过**了，而实际上一条路径都没保住。
      这类"证据本身就是错的"最危险：它让验证脚本替 bug 背书。
    """
    for m in req.get("messages", []):
        if not is_summary_message(m):
            continue
        c = msg_text(m)
        if "【mock 交接摘要】" in c:
            mm = re.search(r"^  · (\S+)", c, re.M)
            if mm:
                return mm.group(1)
            return "(摘要中无路径)"
    return "(未找到摘要)"


def summary_goal_in_request(req):
    """★ 从**交接摘要**里读出原始需求。

    为什么要专门从摘要里读，而不是直接用最后一条 user 消息：
    最后一条 user 消息是"本次输入"，它一定在（组装器的 L6 保证）。
    真正需要证明的是 —— **摘要这份"交接文档"自己记不记得任务是什么**。
    记不得，说明摘要不自包含，换一个模型接手就得从零开始。
    """
    for m in req.get("messages", []):
        if not is_summary_message(m):
            continue
        c = msg_text(m)
        if "【mock 交接摘要】" in c:
            mm = re.search(r"- 原始需求：(.+)", c)
            if mm:
                return mm.group(1).strip()[:60]
            return "(摘要中无需求)"
    return "(未找到摘要)"


# ---------------------------------------------------------------------------
#  模式选择
# ---------------------------------------------------------------------------
def pick_mode(user_text):
    if "危险" in user_text:
        return "danger"
    if "重复" in user_text:
        return "repeat"
    if "超大" in user_text:
        return "bigout"
    if ("汇总" in user_text) or ("逐个读" in user_text) or ("很多文件" in user_text):
        return "grow"
    if ("改名" in user_text) or ("改成" in user_text):
        return "rename"
    if ("不同" in user_text) or ("毫无进展" in user_text):
        return "spin"
    if ("慢速" in user_text) or ("很慢" in user_text):
        return "sleepy"
    if ("报错" in user_text) or ("服务不可用" in user_text):
        return "fail"
    if ("慢慢" in user_text) or ("长篇" in user_text):
        return "slow"
    if ("很久" in user_text) or ("长时间" in user_text):
        return "slowcmd"
    if ("联网" in user_text) or ("下载" in user_text):
        return "netcmd"
    return "chat"


def count_tool_results(msgs):
    """数"已经回填的工具结果" —— **两种协议的消息结构不一样**。

    ★ K-24（脚手架坑，与 K-20 同族）：
      OpenAI：工具结果是一条独立的 `role="tool"` 消息。
      Anthropic：工具结果被**折进** user 消息里，形如
                 {"role":"user","content":[{"type":"tool_result",...}]}
                 —— **没有** role="tool" 的消息。
      所以只数 `role="tool"` 的版本，在 Anthropic 形状下永远数出 0：
      脚本(rename/grow)会以为自己还在第 0 步 → 永远发同一个调用 →
      被闸门 5 判成死循环。症状是"换成 Anthropic 之后模型疯了"，
      而根因在**假服务端**。

      这正是"第二家厂商"这件事的真实代价：它不是换一个 URL，
      而是**每一处依赖消息结构的逻辑都要重新过一遍**。
    """
    n = 0
    for m in msgs:
        if m.get("role") == "tool":
            n += 1
            continue
        c = m.get("content")
        if isinstance(c, list):
            for b in c:
                if isinstance(b, dict) and b.get("type") == "tool_result":
                    n += 1
    return n


def last_tool_excerpt(msgs, n=200):
    """最后一条工具结果的一小段 —— 两种协议都要能取到。"""
    last = ""
    for m in msgs:
        if m.get("role") == "tool":
            last = msg_text(m)
            continue
        c = m.get("content")
        if isinstance(c, list) and any(
                isinstance(b, dict) and b.get("type") == "tool_result" for b in c):
            last = msg_text(m)
    return last.replace("\r", "").replace("\n", " ⏎ ")[:n]


def build_response(req, shape="openai"):
    msgs = req.get("messages", [])
    tool_names = [t.get("function", {}).get("name", t.get("name", "?"))
                  for t in req.get("tools", [])]

    user_text = ""
    for m in reversed(msgs):
        if m.get("role") == "user":
            user_text = msg_text(m)
            break

    n_tool = count_tool_results(msgs)
    mode = pick_mode(user_text)

    anth = (shape == "anthropic")

    def mk_tool(call_id, name, args_obj):
        return (anthropic_tool_stream(call_id, name, args_obj) if anth
                else tool_call_stream(call_id, name, args_obj))

    def mk_text(text):
        return anthropic_text_stream(text) if anth else text_stream(text)

    def last_excerpt(n=200):
        return last_tool_excerpt(msgs, n)

    # ================= repeat：永远原样重发同一条（撞闸门 5）=============
    if mode == "repeat":
        log("[MODE] repeat（第 %d 次，永远发同一条 Read）" % (n_tool + 1))
        return mk_tool("call_rep_0", "Read", {"path": "work_target.txt"})

    # ================= spin：每次参数都不同（撞闸门 1）===================
    #  ★ 参数里多带一个 _spin 字段，让每次调用的指纹都不同 ——
    #    这样既绕开闸门 5，又保持"模型一直在干活但毫无进展"的形态。
    if mode == "spin":
        log("[MODE] spin 第 %d 步（参数每次都不同）" % (n_tool + 1))
        return mk_tool("call_spin_%d" % n_tool, "Read",
                       {"path": "work_target.txt", "_spin": n_tool})

    # ================= sleepy：每次都慢（撞闸门 3 墙钟）=================
    if mode == "sleepy":
        log("[MODE] sleepy 第 %d 步（先睡 1.5s）" % (n_tool + 1))
        time.sleep(1.5)
        return mk_tool("call_sleep_%d" % n_tool, "Read",
                       {"path": "work_target.txt", "_spin": n_tool})

    # ================= slow：长篇慢慢吐（撞闸门 6 取消）=================
    if mode == "slow":
        log("[MODE] slow（分很多帧慢慢吐，给取消/墙钟当靶子）")
        return (anthropic_slow_text_stream() if anth else slow_text_stream())

    # ================= slowcmd：要跑很久的命令（撞沙箱超时）==============
    if mode == "slowcmd":
        if n_tool == 0:
            log("[MODE] slowcmd（发一条要跑很久的命令）")
            return mk_tool("call_slowcmd", "Bash",
                           {"command": "ping -n 30 127.0.0.1"})
        log("[MODE] slowcmd 收工（第 %d 步）" % (n_tool + 1))
        return mk_text("那条命令没能跑完。工具结果： " + last_excerpt())

    # ================= netcmd：要联网下载（撞沙箱禁网）==================
    if mode == "netcmd":
        if n_tool == 0:
            log("[MODE] netcmd（发一条要联网的命令）")
            return mk_tool("call_netcmd", "Bash",
                           {"command": "curl http://example.com/payload"})
        log("[MODE] netcmd 收工（第 %d 步）" % (n_tool + 1))
        return mk_text("联网没成功，我换个离线做法。工具结果： " + last_excerpt())

    # ================= danger：先发一条要删库的 Bash =====================
    if mode == "danger":
        if n_tool == 0:
            log("[MODE] danger（发一条要删库的 Bash）")
            return mk_tool("call_danger", "Bash", {"command": "rm -rf /"})
        log("[MODE] danger 收工（第 %d 步）" % (n_tool + 1))
        return mk_text("命令没有执行成功，我换个安全的方式。工具结果： " + last_excerpt())

    # ================= rename：三步脚本 Read → Edit → Bash → 收工 ========
    #  ★ 注意顺序：这一段必须**在**通用"收工"分支之前判断，
    #    否则第 1 步拿到结果后就会被通用分支截走（= 脚本永远走不到第 2 步）。
    #    这就是 L5 的 K-10。
    if mode == "rename":
        log("[MODE] rename 第 %d 步" % (n_tool + 1))
        if n_tool == 0:
            return mk_tool("call_read", "Read", {"path": "work_target.txt"})
        if n_tool == 1:
            return mk_tool("call_edit", "Edit",
                           {"path": "work_target.txt",
                            "old_str": "int add(",
                            "new_str": "int sum("})
        if n_tool == 2:
            # ★ 这是一条**真的会失败**的命令（若 Edit 没生效）：
            #   findstr 找不到就退出码 1 → 工具返回 error → 回喂给模型。
            return mk_tool("call_bash", "Bash",
                           {"command": "findstr /C:sum work_target.txt"})
        log("[MODE] rename 收工（第 %d 步）" % (n_tool + 1))
        return mk_text(
            "已完成：已把 add 改名为 sum，并跑命令验证过。最后一条工具结果： "
            + last_excerpt() + " —— 以上来自工具结果。")

    # ================= grow：★ L7 连续读 N 个真文件（逼出压缩）===========
    #  每次读**不同**的文件 → 指纹不同 → 不会撞闸门 5。
    #  文件都是真源码，加起来几十万字符 → 一定会顶破小窗口。
    if mode == "grow":
        # ★ 分桶的键必须是**稳定**的那个 user 消息。
        #   不能用"第一条 user 消息"（看起来更自然）：压缩会把交接摘要
        #   插成 history 的头 —— 摘要也是 role=user，于是"第一条 user 消息"
        #   会在压缩发生的那一刻**变成摘要文本**，计数器随之从头开始 →
        #   mock 又去读第 1 个文件 → 撞闸门 5 → 用例失败。
        #   用"最后一条 user 消息"才对：它由组装器追加（L6 保证），
        #   内容始终是本次任务，整个用例期间不变。
        step = grow_step(user_text)
        if step <= len(GROW_FILES):
            f = GROW_FILES[step - 1]
            log("[MODE] grow 第 %d 步：Read %s" % (step, f))
            return mk_tool("call_grow_%d" % (step - 1), "Read", {"path": f})
        ev = summary_in_request(req)
        gv = summary_goal_in_request(req)
        log("[MODE] grow 收工（共发 %d 次 Read；摘要路径=%s；摘要需求=%s）"
            % (step - 1, ev, gv))
        # ★ 收工回答刻意只回显**摘要里的两样东西**（而不是"最后一条 user 消息"）：
        #   最后一条 user 消息是本次输入，它**一定在**（组装器 L6 保证），
        #   拿它当证据等于什么都没证明。
        #   摘要里还有没有需求、还有没有路径，才是"压缩有没有失忆"的真凭据。
        return mk_text(
            "汇总完成：本任务共读取 %d 个文件。"
            "★ 交接摘要里记着的原始需求：%s"
            "★ 交接摘要里保住的路径：%s"
            % (step - 1, gv, ev))

    # ================= bigout：★ L7 一条命令产出超大输出（测 L0 落盘）=====
    #  `type` 是 cmd 内建命令，沙箱用 `cmd.exe /c` 起，所以能用。
    #  两个文件拼起来远超 Bash 的落盘阈值 50'000 字节 → 触发 L0。
    #  ★ 这里刻意**不写死大小**：输出 = `type main.cpp mock_server.py`，
    #    它会随这两个文件的体积变化（本讲实测约 73K 字符 / 90K 字节），
    #    所以写死一个数字迟早对不上账 —— 断言只认"落盘文件 >60K 字节"。
    if mode == "bigout":
        if n_tool == 0:
            log("[MODE] bigout（一条命令产出超大输出，触发 L0 落盘）")
            return mk_tool("call_bigout", "Bash",
                           {"command": "type main.cpp mock_server.py"})
        log("[MODE] bigout 收工（第 %d 步）" % (n_tool + 1))
        return mk_text("输出确实很大，我已按需取用。工具结果开头： " + last_excerpt())

    # ================= chat：不做任何工具调用 ============================
    if n_tool > 0:
        log("[MODE] chat（收工）")
        return mk_text("已完成。最后一条工具结果： " + last_excerpt())
    log("[MODE] chat")
    return mk_text("这是一个不调用任何工具的直接回答（工具表里有 "
                   + str(len(tool_names)) + " 个工具）。")


# ---------------------------------------------------------------------------
#  非流式响应（★ L7 新增）
#
#  压缩器走的是 `chat()` —— 一条**非流式**请求。
#  旧 mock 不看 `stream` 字段，一律回 SSE，于是客户端把 SSE 当 JSON parse 直接炸。
#  这正好说明："加了一条新的调用路径"这件事，只有真跑才会暴露。
# ---------------------------------------------------------------------------
def build_nonstream(req, shape="openai"):
    msgs = req.get("messages", [])
    user_text = ""
    for m in reversed(msgs):
        if m.get("role") == "user":
            user_text = msg_text(m)
            break
    n_tool = count_tool_results(msgs)
    mode = pick_mode(user_text)

    # 压缩请求优先：它带的是"要压缩的对话"，不该被当成普通任务
    if is_compaction_request(req):
        text = build_summary(req)
        log("[MODE] compaction-summary（非流式，返回 %d 字符的交接摘要）" % len(text))
        return (anthropic_nonstream(text) if shape == "anthropic"
                else openai_nonstream(text))

    # 非流式的普通请求：给一个"要不要调工具"的确定答案，
    # 让别的用例也能顺带覆盖 chat() 路径。
    if mode == "rename" and n_tool == 0:
        args = {"path": "work_target.txt"}
        return (anthropic_tool_nonstream("call_read", "Read", args)
                if shape == "anthropic"
                else {"id": "chatcmpl-mock", "object": "chat.completion", "created": 0,
                      "model": "mock-1",
                      "choices": [{"index": 0,
                                   "message": {"role": "assistant", "content": None,
                                               "tool_calls": [{"id": "call_read",
                                                               "type": "function",
                                                               "function": {
                                                                   "name": "Read",
                                                                   "arguments": json.dumps(args)}}]},
                                   "finish_reason": "tool_calls"}],
                      "usage": {"prompt_tokens": 120, "completion_tokens": 20,
                                "total_tokens": 140}})

    text = "（非流式应答）模式=%s，已收到 %d 个工具结果。" % (mode, n_tool)
    return (anthropic_nonstream(text) if shape == "anthropic"
            else openai_nonstream(text))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, ctype, data):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        # 分片发送，顺便把"服务端持续在吐字节"这件事做实
        for i in range(0, len(data), CHUNK_STEP):
            self.wfile.write(data[i:i + CHUNK_STEP])
            self.wfile.flush()
            time.sleep(0.01)
        self.close_connection = True

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n)
        try:
            req = json.loads(raw.decode("utf-8"))
        except Exception as e:
            log("[REQ] BAD JSON (encoding?): %s" % e)
            req = {}

        # ★ L7：按**路径**区分协议 —— 这就是"两个端点"在服务端的对应物
        shape = "anthropic" if "/messages" in self.path else "openai"

        msgs = req.get("messages", [])
        tools = req.get("tools", [])
        names = [t.get("function", {}).get("name", t.get("name", "?")) for t in tools]
        log("[REQ] shape=%s model=%s stream=%s tools=%d %s msgs=%d"
            % (shape, req.get("model"), req.get("stream"), len(tools), names, len(msgs)))
        for m in msgs:
            role = m.get("role")
            c = msg_text(m)
            extra = ""
            if role == "assistant" and m.get("tool_calls"):
                extra = " tool_calls=" + json.dumps(
                    [tc.get("function", {}).get("name") for tc in m["tool_calls"]],
                    ensure_ascii=False)
            if role == "assistant":
                for b in (m.get("content") or []):
                    if isinstance(b, dict) and b.get("type") == "tool_use":
                        extra += " tool_use=%s" % b.get("name")
            if role == "tool":
                extra = " tool_call_id=%s" % m.get("tool_call_id")
            # ★ Anthropic 形状下，工具结果**折进 user 消息**（没有 role=tool）。
            #   不打这个标记的话，日志上看起来"工具结果凭空消失了"，
            #   会让你往错误的方向查（去怀疑客户端没回填结果）。
            if role == "user" and isinstance(m.get("content"), list):
                ntr = sum(1 for b in m["content"]
                          if isinstance(b, dict) and b.get("type") == "tool_result")
                if ntr:
                    extra = " tool_result×%d（折进 user 消息）" % ntr
            # ★ 把**这条消息进上下文的实际长度**打进日志。
            #   为什么值得单独记：L0 落盘、L1/L2 折叠、L3 摘要，这三件事
            #   的验收词都是"**上下文里只剩多少**"。只记前 60 字符的话，
            #   你能看到内容变了，却看不出"从 90K 压到了 2K"这个**量的变化**，
            #   而量才是这几级要证明的东西。
            log("[REQ]   %s: %s%s  [len=%d]" % (role, c[:60], extra, len(c)))
            # ★ 压缩摘要进上下文这件事必须看得见 —— 它就是"记忆还在"的证据。
            #   ★ 判据必须排除工具结果（K-25）：`mock_server.py` 自己就含这个标记，
            #     而 grow 模式会把它读进来。不排除的话，日志会**谎报**"摘要在场"。
            if is_summary_message(m) and "【mock 交接摘要】" in c:
                log("[REQ]   ! 压缩摘要消息在场（%d 字符）：%s"
                    % (len(c), c.replace("\n", " | ")[:200]))

        # ★ 角色必须严格交替（Anthropic 的硬要求）。
        #    OpenAI 允许连续同角色，Anthropic 会直接 400 —— 所以这条检查
        #    只对 anthropic 形状报错，避免把合法请求也标红。
        prev_role = None
        for m in msgs:
            r = m.get("role")
            if r == "system":
                continue
            if r == prev_role and shape == "anthropic":
                log("[REQ]   !!! 连续同角色 %s —— Anthropic 会 400（适配器没合并）" % r)
            prev_role = r

        # ★ L8：上面每条消息只打了**前 60 字符**，而仓库地图是**追加在 system 末尾**的 ——
        #   光看头部永远看不见它，日志会谎报"地图没进请求"。
        #   所以这里补一行**服务端实际收到的 system 全长清单**：
        #     长度 + 命中了哪些段落标题 + 末尾 80 字符。
        #   ★ 判据要的是"字节真的上了线路"，所以首选证据是**长度差**，
        #     段落标题只是让你一眼看出多出来的那一截是什么。
        sys_text = system_text(req)
        if sys_text:
            secs = [s for s in ("# 项目知识", "# 仓库地图") if s in sys_text]
            log("[REQ]   system[full=%d chars] sections=%s tail=%s"
                % (len(sys_text), secs, sys_text[-80:].replace("\n", " | ")))

        user_text = ""
        for m in reversed(msgs):
            if m.get("role") == "user":
                user_text = msg_text(m)
                break

        # ---- fail 模式：直接抛错误码，模拟"服务端一直报错"（撞闸门 4）------
        if not is_compaction_request(req) and pick_mode(user_text) == "fail":
            log("[MODE] fail（返回 401，撞闸门 4）")
            payload = json.dumps(
                {"error": {"message": "mock: 401 unauthorized（本模式用来撞闸门 4）"}},
                ensure_ascii=False).encode("utf-8")
            # 401 不在 RetryPolicy::retryOn 里 → 客户端不重试，直接抛
            self._send(401, "application/json", payload)
            return

        # ---- ★ L7：非流式请求走 JSON 通道 ---------------------------------
        if not req.get("stream"):
            obj = build_nonstream(req, shape)
            self._send(200, "application/json",
                       json.dumps(obj, ensure_ascii=False).encode("utf-8"))
            return

        body = "".join(build_response(req, shape))
        self._send(200, "text/event-stream", body.encode("utf-8"))


if __name__ == "__main__":
    open(LOG, "w", encoding="utf-8").close()
    print("mock server on http://127.0.0.1:%d  (log: %s)" % (PORT, LOG), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
