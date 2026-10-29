# ---------------------------------------------------------------------------
#  L4 · mock_server.py —— 会**真的发 tool_calls** 的假服务端
#
#  与 L3 那份的区别只有一条主线：
#      L3 只验证"文本流 + 结束原因"
#      L4 还要验证"**工具调用意图**能不能从协议里被完整取出来"
#
#  于是它多做了四件刁难客户端的事：
#    1. tool_calls 的 arguments 被**切成 4 段**发
#       —— 中途 parse 必然失败；只有攒到最后一次 parse 才对
#    2. `finish_reason: "tool_calls"` 与**最后一段 arguments 同帧**
#       —— 处理完 tool_calls 就 return 的实现会把结束原因吞掉
#    3. 起头那一帧的 arguments 是**空串**（不是 null）
#       —— 真实 OpenAI 就是这么发的；按"空串=没内容"跳过会丢掉 id
#    4. 第二轮请求里必须能看到上一轮的 assistant.tool_calls + tool 结果
#       —— 看不到就说明客户端没把工具字段存回历史，模型会重复调同一个工具
#
#  分支由"用户任务文本"决定，让验证脚本能命中确定的分支：
#    含 "读" / "mock_target"   → 发 Read(path=mock_target.txt)
#    含 "WordCount" / "统计"    → 发 WordCount(path=mock_target.txt)   （插件工具）
#    含 "非法参数" / "缺"       → 发 Read 但不给 path（触发参数校验）
#    含 "未知工具" / "不存在"    → 发一个不存在的工具名
#    其余                      → 直接文本回答
#
#  第二轮（历史里已经有 role=tool 消息）一律走"最终回答"分支，
#  并把**上一条工具结果的原文**摘出来回显 —— 这样验证脚本才能断言
#  "工具结果真的回到了模型手里"，而不只是"请求发出去了"。
#
#  运行：
#      python mock_server.py [port] [logfile]
# ---------------------------------------------------------------------------
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8789
LOG = sys.argv[2] if len(sys.argv) > 2 else "mock_requests.log"

# 故意按任意字节边界切开发送（TCP 不保证消息边界）
CHUNK_STEP = 37

USAGE = {"prompt_tokens": 96, "completion_tokens": 18, "total_tokens": 114}


def log(line):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def frame(obj):
    return "data: " + json.dumps(obj, ensure_ascii=False) + "\n\n"


def chunk(delta, finish=None, usage=None):
    ch = {"index": 0, "delta": delta}
    if finish is not None:
        ch["finish_reason"] = finish
    out = {"id": "chatcmpl-mock", "object": "chat.completion.chunk",
           "created": 0, "model": "mock-1", "choices": [ch]}
    # ★ 每帧都带 usage（多数是 null）—— 客户端必须判 is_null 而不是 contains
    out["usage"] = usage
    return frame(out)


def usage_frame():
    return frame({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                  "created": 0, "model": "mock-1", "choices": [], "usage": USAGE})


def split4(s):
    """把字符串切成 4 段（长度小时允许出现空段）。"""
    L = len(s)
    cuts = [0, L // 4, L // 2, (3 * L) // 4, L]
    return [s[cuts[i]:cuts[i + 1]] for i in range(4)]


def tool_call_stream(call_id, name, args_json):
    """构造一段"模型决定调用工具"的流。"""
    stream = [": keepalive\n\n"]                       # 注释帧，必须被忽略

    # ★ 起头帧：name 出现，arguments 是**空串**（真实服务端就这么发）
    stream.append(chunk({"role": "assistant", "content": None,
                         "tool_calls": [{"index": 0, "id": call_id, "type": "function",
                                         "function": {"name": name, "arguments": ""}}]},
                        usage=None))

    # ★ 参数切成 4 段逐段发 —— 中途 parse 必失败
    pieces = split4(args_json)
    for i, p in enumerate(pieces):
        last = (i == len(pieces) - 1)
        stream.append(chunk({"tool_calls": [{"index": 0,
                                             "function": {"arguments": p}}]},
                            # ★ 最后一段参数与 finish_reason 同帧 —— 最容易吞掉结束原因
                            finish="tool_calls" if last else None,
                            usage=None))

    stream.append(usage_frame())
    stream.append("data: [DONE]\n\n")
    return stream


def text_stream(text):
    """构造一段普通文本流（含 L3 已经踩过的那些坑）。"""
    stream = [
        ": keepalive\n\n",
        chunk({"content": ""}, usage=None),                       # 首帧空串
        chunk({"role": "assistant", "content": None}, usage=None),
    ]
    for i in range(0, len(text), 7):
        stream.append(chunk({"content": text[i:i + 7]}, usage=None))
    stream.append(chunk({"content": ""}, finish="stop"))           # finish 同帧
    stream.append(usage_frame())
    stream.append("data: [DONE]\n\n")
    return stream


def pick_mode(user_text):
    if ("非法参数" in user_text) or ("缺" in user_text):
        return "bad_args"
    if ("未知工具" in user_text) or ("不存在" in user_text):
        return "unknown_tool"
    if ("WordCount" in user_text) or ("统计" in user_text):
        return "wordcount"
    if ("读" in user_text) or ("mock_target" in user_text):
        return "read"
    return "chat"


def build_response(req):
    msgs = req.get("messages", [])
    tool_names = [t.get("function", {}).get("name", "?")
                  for t in req.get("tools", [])]

    user_text = ""
    for m in reversed(msgs):
        if m.get("role") == "user":
            user_text = str(m.get("content") or "")
            break

    # ---- 已经收到过工具结果 → 走"最终回答"分支 ----
    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    if tool_msgs:
        last = str(tool_msgs[-1].get("content") or "")
        excerpt = last.replace("\r", "").replace("\n", " ⏎ ")[:220]
        log("[MODE] final（第 2 轮，已看到工具结果）")
        return text_stream(
            "已收到工具返回。依据如下： " + excerpt +
            " —— 以上内容来自工具结果（不是凭空生成）。")

    mode = pick_mode(user_text)
    log("[MODE] %s" % mode)

    if mode == "read":
        return tool_call_stream("call_read_1", "Read",
                                json.dumps({"path": "mock_target.txt"},
                                           ensure_ascii=False))
    if mode == "wordcount":
        return tool_call_stream("call_wc_1", "WordCount",
                                json.dumps({"path": "mock_target.txt"},
                                           ensure_ascii=False))
    if mode == "bad_args":
        # ★ 故意不给 path —— 触发宿主的参数校验（模型幻觉出缺字段的调用）
        return tool_call_stream("call_bad_1", "Read", "{}")
    if mode == "unknown_tool":
        return tool_call_stream("call_unk_1", "NoSuchTool", "{}")

    # 默认：纯文本
    return text_stream("这是一个不调用任何工具的直接回答（tools 列表里共有 "
                       + str(len(tool_names)) + " 个工具）。")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n)
        try:
            req = json.loads(raw.decode("utf-8"))
        except Exception as e:
            # ★ 客户端若发的是 GBK 字节（Windows argv 的经典事故），这里直接报错
            log("[REQ] BAD JSON (encoding?): %s" % e)
            req = {}

        msgs = req.get("messages", [])
        tools = req.get("tools", [])
        names = [t.get("function", {}).get("name", "?") for t in tools]
        log("[REQ] model=%s stream=%s tools=%d %s msgs=%d"
            % (req.get("model"), req.get("stream"), len(tools), names, len(msgs)))
        for m in msgs:
            role = m.get("role")
            extra = ""
            if role == "assistant" and m.get("tool_calls"):
                extra = " tool_calls=" + json.dumps(
                    [tc.get("function", {}).get("name") for tc in m["tool_calls"]],
                    ensure_ascii=False)
            if role == "tool":
                extra = " tool_call_id=%s" % m.get("tool_call_id")
            log("[REQ]   %s: %s%s" % (role, str(m.get("content"))[:60], extra))

        body = "".join(build_response(req))
        data = body.encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()

        for i in range(0, len(data), CHUNK_STEP):
            self.wfile.write(data[i:i + CHUNK_STEP])
            self.wfile.flush()
            time.sleep(0.01)
        self.close_connection = True


if __name__ == "__main__":
    open(LOG, "w", encoding="utf-8").close()
    print("mock server on http://127.0.0.1:%d  (log: %s)" % (PORT, LOG), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
