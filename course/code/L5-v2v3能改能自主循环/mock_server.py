# ---------------------------------------------------------------------------
#  L5 · mock_server.py —— 会**按历史推进多步脚本**的假服务端
#
#  L4 的那份只会"一次工具调用"。而 L5 的核心是**环** ——
#  要验证环，假服务端就必须能扮演"一个会分多步做事的模型"：
#
#      第 1 步  读文件
#      第 2 步  改文件
#      第 3 步  跑命令验证
#      第 4 步  宣布完成
#
#  于是它按"请求历史里有几条 tool 消息"来决定这一步该发什么 ——
#  **这就等价于"模型看到了之前所有工具结果之后，决定下一步做什么"。**
#  如果客户端没有把工具结果正确回喂、没有把历史组装对，
#  这里的 n_tool 就不会增长，脚本会卡在第 1 步**反复重发同一个调用** ——
#  而这恰好就是真实世界里"模型重复调用同一个工具"的样子。
#
#  四种模式（由任务文本决定，任务文本在每一轮里都是同一条，所以模式稳定）：
#    含 "改名" / "改成"   → rename   脚本：Read → Edit → Bash → 完成
#    含 "危险"            → danger   发一条要删库的 Bash（验证权限门 Deny）
#    含 "重复"            → repeat   永远发同一条 Read（验证"没有闸门会怎样"）
#    其余                 → chat     直接文本回答
#
#  刁难客户端的手法与 L4 一致：参数切 4 段、finish_reason 同帧、每帧带 usage:null、
#  开头插 keepalive、起头帧 arguments 是空串。
#
#  运行：
#      python mock_server.py [port] [logfile]
# ---------------------------------------------------------------------------
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8790
LOG = sys.argv[2] if len(sys.argv) > 2 else "mock_requests.log"

CHUNK_STEP = 37                      # 任意字节边界切开再发
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


def pick_mode(user_text):
    if ("危险" in user_text):
        return "danger"
    if ("重复" in user_text):
        return "repeat"
    if ("改名" in user_text) or ("改成" in user_text):
        return "rename"
    return "chat"


def build_response(req):
    msgs = req.get("messages", [])
    tool_names = [t.get("function", {}).get("name", "?") for t in req.get("tools", [])]

    user_text = ""
    for m in reversed(msgs):
        if m.get("role") == "user":
            user_text = str(m.get("content") or "")
            break

    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    n_tool = len(tool_msgs)
    mode = pick_mode(user_text)

    def last_excerpt(n=200):
        last = str(tool_msgs[-1].get("content") or "") if tool_msgs else ""
        return last.replace("\r", "").replace("\n", " ⏎ ")[:n]

    # ================= repeat：永远发同一条（用来撞上限）=================
    if mode == "repeat":
        log("[MODE] repeat（第 %d 次，永远发同一条 Read）" % (n_tool + 1))
        return tool_call_stream("call_rep_%d" % n_tool, "Read",
                                {"path": "work_target.txt"})

    # ================= danger：先发一条要删库的 Bash =====================
    if mode == "danger":
        if n_tool == 0:
            log("[MODE] danger（发一条要删库的 Bash）")
            return tool_call_stream("call_danger", "Bash", {"command": "rm -rf /"})
        log("[MODE] danger 收工（第 %d 步）" % (n_tool + 1))
        return text_stream("命令没有执行成功，我换个安全的方式。工具结果： " + last_excerpt())

    # ================= rename：三步脚本 Read → Edit → Bash → 收工 ========
    #  ★ 注意顺序：这一段必须**在**通用"收工"分支之前判断，
    #    否则第 1 步拿到结果后就会被通用分支截走（= 脚本永远走不到第 2 步）。
    if mode == "rename":
        log("[MODE] rename 第 %d 步" % (n_tool + 1))
        if n_tool == 0:
            return tool_call_stream("call_read", "Read", {"path": "work_target.txt"})
        if n_tool == 1:
            return tool_call_stream("call_edit", "Edit",
                                    {"path": "work_target.txt",
                                     "old_str": "int add(",
                                     "new_str": "int sum("})
        if n_tool == 2:
            # ★ 这是一条**真的会失败**的命令（若 Edit 没生效）：
            #   findstr 找不到就退出码 1 → 工具返回 error → 回喂给模型。
            #   验证"改完必须验证"这条纪律是否真的闭环。
            return tool_call_stream("call_bash", "Bash",
                                    {"command": "findstr /C:sum work_target.txt"})
        log("[MODE] rename 收工（第 %d 步）" % (n_tool + 1))
        return text_stream(
            "已完成：已把 add 改名为 sum，并跑命令验证过。最后一条工具结果： "
            + last_excerpt() + " —— 以上来自工具结果。")

    # ================= chat：不做任何工具调用 ============================
    if n_tool > 0:
        log("[MODE] chat（收工）")
        return text_stream("已完成。最后一条工具结果： " + last_excerpt())
    log("[MODE] chat")
    return text_stream("这是一个不调用任何工具的直接回答（工具表里有 "
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
