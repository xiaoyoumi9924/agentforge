# ---------------------------------------------------------------------------
#  L6 · mock_server.py —— 假服务端（能扮演"各种坏掉的模型"）
#
#  L5 的版本会按历史推进多步脚本，用来证明"环转起来了"。
#  L6 的验证目标变了：要**逐道闸门**证明它能拦住对应的失败模式。
#
#  于是这个假服务端多了几种"坏模型"模式 —— 每一种精确对应一道闸门：
#
#    模式        它模仿的失败模式              应对的闸门
#    --------    --------------------------    --------------------------
#    repeat      模型永远原样重发同一个调用   闸门 5 重复调用
#    spin        模型每次换参数、但毫无进展   闸门 1 步数上限
#    sleepy      模型每次都很慢               闸门 3 墙钟
#    fail        服务端一直报错               闸门 4 连续错误
#    slow        长篇大论、慢慢吐             闸门 6 用户中断
#    slowcmd     要求跑一条很久的命令         沙箱：超时强杀
#    netcmd      要求联网下载                 沙箱：禁网策略
#    danger      要求删库                     权限门 Deny
#    rename      正经干活（回归用例）          ——
#    chat        什么都不调用                 ——
#
#  ★ 为什么 spin 模式必须"每次参数都不同"：
#    否则它会先被**闸门 5** 抓住，就永远测不到闸门 1 了。
#    想隔离一道闸门，就要先把其他闸门绕开 —— 这是写验证用例的常见功课。
#    （本文件里同样的分支顺序问题在 L5 踩过一次，见 K-10：
#      具体模式的判断必须排在通用"收工"分支**之前**。）
#
#  刁难客户端的手法与 L4/L5 一致：参数切 4 段、finish_reason 同帧、
#  每帧带 usage:null、开头插 keepalive、起头帧 arguments 是空串。
#
#  运行：
#      python mock_server.py [port] [logfile]
# ---------------------------------------------------------------------------
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
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


def pick_mode(user_text):
    if ("危险" in user_text):
        return "danger"
    if ("重复" in user_text):
        return "repeat"
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

    # ================= repeat：永远原样重发同一条（撞闸门 5）=============
    if mode == "repeat":
        log("[MODE] repeat（第 %d 次，永远发同一条 Read）" % (n_tool + 1))
        return tool_call_stream("call_rep_0", "Read",
                                {"path": "work_target.txt"})

    # ================= spin：每次参数都不同（撞闸门 1）===================
    #  ★ 参数里多带一个 _spin 字段，让每次调用的指纹都不同 ——
    #    这样既绕开闸门 5，又保持"模型一直在干活但毫无进展"的形态。
    if mode == "spin":
        log("[MODE] spin 第 %d 步（参数每次都不同）" % (n_tool + 1))
        return tool_call_stream("call_spin_%d" % n_tool, "Read",
                                {"path": "work_target.txt", "_spin": n_tool})

    # ================= sleepy：每次都慢（撞闸门 3 墙钟）=================
    if mode == "sleepy":
        log("[MODE] sleepy 第 %d 步（先睡 1.5s）" % (n_tool + 1))
        time.sleep(1.5)
        return tool_call_stream("call_sleep_%d" % n_tool, "Read",
                                {"path": "work_target.txt", "_spin": n_tool})

    # ================= slow：长篇慢慢吐（撞闸门 6 取消）=================
    if mode == "slow":
        log("[MODE] slow（分很多帧慢慢吐，给取消/墙钟当靶子）")
        return slow_text_stream()

    # ================= slowcmd：要跑很久的命令（撞沙箱超时）==============
    if mode == "slowcmd":
        if n_tool == 0:
            log("[MODE] slowcmd（发一条要跑很久的命令）")
            return tool_call_stream("call_slowcmd", "Bash",
                                    {"command": "ping -n 30 127.0.0.1"})
        log("[MODE] slowcmd 收工（第 %d 步）" % (n_tool + 1))
        return text_stream("那条命令没能跑完。工具结果： " + last_excerpt())

    # ================= netcmd：要联网下载（撞沙箱禁网）==================
    if mode == "netcmd":
        if n_tool == 0:
            log("[MODE] netcmd（发一条要联网的命令）")
            return tool_call_stream("call_netcmd", "Bash",
                                    {"command": "curl http://example.com/payload"})
        log("[MODE] netcmd 收工（第 %d 步）" % (n_tool + 1))
        return text_stream("联网没成功，我换个离线做法。工具结果： " + last_excerpt())

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
    #    这就是 L5 的 K-10。
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

        # ---- fail 模式：直接抛错误码，模拟"服务端一直报错"（撞闸门 4）------
        user_text = ""
        for m in reversed(msgs):
            if m.get("role") == "user":
                user_text = str(m.get("content") or "")
                break
        if pick_mode(user_text) == "fail":
            log("[MODE] fail（返回 401，撞闸门 4）")
            payload = json.dumps(
                {"error": {"message": "mock: 401 unauthorized（本模式用来撞闸门 4）"}},
                ensure_ascii=False).encode("utf-8")
            # 401 不在 RetryPolicy::retryOn 里 → 客户端不重试，直接抛
            self._send(401, "application/json", payload)
            return

        body = "".join(build_response(req))
        self._send(200, "text/event-stream", body.encode("utf-8"))


if __name__ == "__main__":
    open(LOG, "w", encoding="utf-8").close()
    print("mock server on http://127.0.0.1:%d  (log: %s)" % (PORT, LOG), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
