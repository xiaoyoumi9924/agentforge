# -*- coding: utf-8 -*-
# ---------------------------------------------------------------------------
#  make_session_fixture.py —— 造几份"有问题"的 JSONL，喂给 --session-debug
#
#  ★ 为什么要有这个脚本：
#    "半行损坏"、"压缩到一半被中断"这类边界，**靠一次真实对话永远碰不到**
#    （你不能指望进程刚好在写第 37 个字节时被杀）。
#    所以它们必须被**构造**出来 —— 这跟 L9 用 --ui-demo 把"工具执行的 1.2 秒空窗"
#    写成脚本是同一个道理：**把时序/边界写成输入，而不是交给运气。**
#
#  产出（默认写到 ./_sessfix/）：
#    a_halflane.jsonl   末尾一行被截断（模拟进程被杀）
#    b_compacted.jsonl  正常压缩过一轮（replaced_upto + 非空 summary）
#    c_partial.jsonl    ★ T3：compaction 写了、摘要还是空的
#    d_empty.jsonl      ★ T3 的灾难情形：截断后会把上下文截成空
# ---------------------------------------------------------------------------
import json, os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "_sessfix"
os.makedirs(OUT, exist_ok=True)

def w(name, lines):
    p = os.path.join(OUT, name)
    with open(p, "w", encoding="utf-8") as f:
        for ln in lines:
            f.write(ln + "\n")
    print(f"  {name}  {os.path.getsize(p)} bytes  {len(lines)} 行")

def meta(sid):
    return json.dumps({"t": "meta", "v": 1, "id": sid, "cwd": os.getcwd(),
                       "created_at": 1700000000000}, ensure_ascii=False)

def msg(seq, t, content, **kw):
    d = {"seq": seq, "ts": 1700000000000 + seq, "t": t, "content": content}
    d.update(kw)
    return json.dumps(d, ensure_ascii=False)

def comp(seq, replaced_upto, summary, level=3):
    return json.dumps({"seq": seq, "ts": 1700000000000 + seq, "t": "compaction",
                       "replaced_upto": replaced_upto, "summary": summary,
                       "level": level, "tokens_before": 9000, "tokens_after": 1200,
                       "kept_messages": 2}, ensure_ascii=False)

print("[夹具] 输出目录:", OUT)

# ---- a：末尾半行（进程写到一半被杀）----
w("a_halflane.jsonl", [
    meta("sess-a"),
    msg(1, "user", "第一轮的问题"),
    msg(2, "assistant", "好的，我先读文件"),
    msg(3, "user", "这一行写到一半就被杀了"),
] )
# 手工补一个被截断的行（不能 json.dumps，它一定是完整的）
with open(os.path.join(OUT, "a_halflane.jsonl"), "a", encoding="utf-8") as f:
    f.write('{"seq":4,"t":"assistant","content":"我正')
print("  a_halflane.jsonl  +1 半行")

# ---- b：正常压缩过一轮 ----
w("b_compacted.jsonl", [
    meta("sess-b"),
    msg(1, "user", "旧消息 1"),
    msg(2, "assistant", "旧消息 2"),
    msg(3, "user", "交接摘要：之前把 add 改成了 sum"),
    msg(4, "assistant", "新消息 1"),
    comp(5, 2, "交接摘要：之前把 add 改成了 sum"),
])

# ---- c：★ T3 —— compaction 写了，摘要还是空的 ----
w("c_partial.jsonl", [
    meta("sess-c"),
    msg(1, "user", "旧消息 1"),
    msg(2, "assistant", "旧消息 2"),
    comp(3, 2, ""),
])

# ---- d：★ T3 的灾难情形 —— 截断后会变成空上下文 ----
w("d_empty.jsonl", [
    meta("sess-d"),
    msg(1, "user", "唯一的一条消息"),
    comp(2, 1, "摘要：什么都被取代了"),
])

# ---- e：审计型 compaction（replaced_upto < 0，不参与重建）----
w("e_audit.jsonl", [
    meta("sess-e"),
    msg(1, "user", "消息 1"),
    msg(2, "assistant", "消息 2"),
    json.dumps({"seq": 3, "t": "compaction", "replaced_upto": -1,
                "summary": "", "level": 2, "tokens_before": 8000,
                "tokens_after": 3000, "kept_messages": 2}, ensure_ascii=False),
])
