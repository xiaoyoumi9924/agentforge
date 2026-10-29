# ---------------------------------------------------------------------------
#  L3 · mock_server.py —— OpenAI 兼容的假服务端（不需要真 API Key）
#
#  它存在的唯一理由：**协议层的正确性不该依赖"有没有密钥"**。
#  整个 L3 要验证的东西（分帧、跨块累积、结束原因）全都是协议层的事，
#  用一个假服务端就能全部覆盖，而且可以故意制造真实服务端才会有的恶劣情况。
#
#  它刻意做四件"刁难"客户端的事：
#    1. 把整个 SSE 响应体按 **37 字节** 的硬边界切开再发送
#       —— TCP 不保证消息边界，一个事件可能被切成 3 段，也可能 3 个事件挤在 1 段
#    2. 每一帧都带 `"usage": null`
#       —— 很多兼容网关就是这么发的；客户端若只判 contains，用量统计会失真
#    3. 在开头插一个 `: keepalive` 注释帧
#       —— 真实服务端用来防空闲超时；客户端必须能识别并忽略
#    4. 把 **finish_reason 与最后一段 content 塞在同一帧**
#       —— 最容易吞掉结束原因的地方
#
#  运行：
#      python mock_server.py [port] [logfile]
# ---------------------------------------------------------------------------
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8788
LOG = sys.argv[2] if len(sys.argv) > 2 else "mock_requests.log"

# 故意按任意字节边界切开发送
CHUNK_STEP = 37

# 每次响应结尾的固定用量（模拟服务端真实回报）
USAGE = {"prompt_tokens": 128, "completion_tokens": 24, "total_tokens": 152}


def log(line):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def frame(obj):
    return "data: " + json.dumps(obj, ensure_ascii=False) + "\n\n"


def chunk(content=None, finish=None, usage=None):
    """构造一个 OpenAI 风格的流式分片。"""
    d = {}
    if content is not None:
        d["content"] = content
    ch = {"index": 0, "delta": d}
    if finish is not None:
        ch["finish_reason"] = finish
    out = {"id": "chatcmpl-mock", "object": "chat.completion.chunk",
           "created": 0, "model": "mock-1", "choices": [ch]}
    # ★ 每一帧都带 usage（多数是 null）—— 客户端必须判 is_null
    out["usage"] = usage
    return frame(out)


def build_response(user_text):
    """根据用户输入决定返回什么，让验证脚本能命中确定的分支。"""
    # 分支一：空响应。服务端返回 200 但一个字都没有 —— 最容易被当成成功的失败。
    if "空" in user_text:
        log("[MODE] empty")
        return [
            ": keepalive\n\n",
            chunk(content="", finish="stop"),
            frame({"id": "chatcmpl-mock", "object": "chat.completion.chunk", "created": 0,
                   "model": "mock-1", "choices": [],
                   "usage": {"prompt_tokens": 128, "completion_tokens": 0, "total_tokens": 128}}),
            "data: [DONE]\n\n",
        ]

    # 分支二：被截断。finish_reason="length" 绝不能被当成正常完成。
    if "截断" in user_text:
        log("[MODE] truncated")
        parts = ["这段回答", "会在中途", "被截断"]
        stream = [": keepalive\n\n", chunk(content="", finish=None, usage=None)]
        for p in parts:
            stream.append(chunk(content=p, usage=None))
        stream.append(chunk(content="然后就没了", finish="length"))
        stream.append(frame({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                             "created": 0, "model": "mock-1", "choices": [],
                             "usage": USAGE}))
        stream.append("data: [DONE]\n\n")
        return stream

    # 分支三（默认）：正常回答，把用户的话回显一段，验证 UTF-8 一路没被破坏。
    log("[MODE] normal")
    echo = user_text[:20]
    text = ("你问的是「" + echo + "」。这是来自假服务端的流式回答，"
            "用来验证分帧、跨块累积与结束原因。")
    stream = [
        ": keepalive\n\n",                                   # ① 注释帧，必须被忽略
        chunk(content="", usage=None),                       # ② 首个空 delta（真实服务端常见）
        frame({"id": "chatcmpl-mock", "object": "chat.completion.chunk", "created": 0,
               "model": "mock-1",
               "choices": [{"index": 0,
                            "delta": {"role": "assistant", "content": None}}],
               "usage": None}),
    ]
    # ③ 正文切成若干小段 —— 模拟"逐 token 输出"
    for i in range(0, len(text), 7):
        stream.append(chunk(content=text[i:i + 7], usage=None))
    # ④ ★ 最后一段正文与 finish_reason 同帧（最容易吞掉结束原因的地方）
    stream.append(chunk(content="", finish="stop"))
    # ⑤ 用量单独一帧
    stream.append(frame({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                         "created": 0, "model": "mock-1", "choices": [], "usage": USAGE}))
    # ⑥ 结束标记
    stream.append("data: [DONE]\n\n")
    return stream


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
            # ★ 客户端如果发出的是 GBK 字节（Windows argv 的经典事故），
            #   这里会直接报 BAD JSON —— 验证脚本就是靠这一行抓那个 bug 的。
            log("[REQ] BAD JSON (encoding?): %s" % e)
            req = {}

        msgs = req.get("messages", [])
        tools = req.get("tools", [])
        log("[REQ] model=%s stream=%s tools=%d msgs=%d"
            % (req.get("model"), req.get("stream"), len(tools), len(msgs)))
        for m in msgs:
            log("[REQ]   %s: %s" % (m.get("role"), str(m.get("content"))[:60]))

        user_text = ""
        for m in reversed(msgs):
            if m.get("role") == "user":
                user_text = str(m.get("content") or "")
                break

        body = "".join(build_response(user_text))
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
