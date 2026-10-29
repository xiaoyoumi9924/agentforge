#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_subagent_fixture.py —— L11 的验证夹具：20 个"探索型"文件

★ 设计要点：每个文件里有**两个**可区分的东西

    MARK-07                    ← 结论层：名字。摘要里会出现它。
    PAYLOAD-07-<600 字节填充>  ← 过程层：正文。**只有真正读过才会进上下文**。

  这个区分是 C39 能不能成立的关键：
    如果只放 MARK-nn，那么"摘要里提到 MARK-07"和"读过 MARK-07 的全文"
    在日志上长得一模一样 —— 断言会**同时放过两种情况**，等于没测。

  ★ 这也是本讲要证明的那件事本身：
    子代理省下的不是"不认识这些文件"，而是"**不把这些文件的正文留在主上下文里**"。
    结论可以回传，过程不能。

用法：
    python make_subagent_fixture.py [输出目录] [文件数]
"""
import os
import sys

N_FILES = 20
PER_FILE = 600          # 每个文件的填充字节数 → 20 × ~700B ≈ 14KB


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "_subfix"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else N_FILES
    d = os.path.join(out, "mark")
    os.makedirs(d, exist_ok=True)

    total = 0
    for i in range(1, n + 1):
        tag = "MARK-%02d" % i
        # 填充用**可数、可 grep、且与序号绑定**的字符块：
        # 用随机字节会让"grep 到了几次"这件事不可复现。
        filler = ("PAYLOAD-%02d-" % i) + ("x" * PER_FILE)
        body = (
            "%s\n"          # 名字（结论层）
            "这个文件是 L11 子代理夹具的第 %d 号文件。\n"
            "%s\n"          # 正文（过程层）
            "END-%s\n" % (tag, i, filler, tag)
        )
        p = os.path.join(d, "%s.txt" % tag)
        with open(p, "w", encoding="utf-8") as f:
            f.write(body)
        total += len(body.encode("utf-8"))

    # 一份"答案清单"，给验证脚本算召回率用（不进 agent 上下文）
    with open(os.path.join(out, "_answer.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join("MARK-%02d" % i for i in range(1, n + 1)))

    print("生成 %d 个文件于 %s" % (n, d))
    print("每个约 %d 字节，合计 %d 字节（≈ %.1f KB）"
          % (PER_FILE + 60, total, total / 1024.0))
    print("★ 结论层标记 MARK-nn 会出现在摘要里；过程层标记 PAYLOAD-nn 只应出现在子代理请求里")


if __name__ == "__main__":
    main()
