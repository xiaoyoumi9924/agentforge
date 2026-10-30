// ---------------------------------------------------------------------------
//  ui/web_page.hpp —— 单页前端（编译进 exe）
//
//  ★ 为什么把 HTML 塞进 C++ 字符串而不是放磁盘上：
//    交付物是一个 exe。放磁盘就要回答"它相对谁"——当前目录？exe 目录？
//    老师双击运行时那个路径是什么？**每多一个"相对谁"的问题，演示就多一次翻车。**
//    单文件内嵌把这个问题直接消掉。
//
//  ★ 前端只做三件事：渲染事件流、画工具卡片、弹权限窗。
//    它**不**做任何决策 —— 所有判断都在 C++ 侧，浏览器只是个显示器。
//
//  ★ 拆分说明：MSVC 对**单个**字符串字面量有 16384 字节上限（C2026），
//    整页 HTML 远超这个数，所以拆成多段，运行时拼起来。
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace af::ui {

inline constexpr const char* kWebPage1 = R"AFWEB(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lumin · Web UI</title>
<style>
  :root{
    color-scheme: dark;
    --bg:#0a0f18; --side:#0b111b; --panel:#0d1520; --panel-2:#111a27; --panel-3:#162131;
    --line:#1b2533; --line-2:#263346; --line-3:#34465d;
    --ink:#e9eef6; --ink-2:#a7b2c4; --ink-3:#77849a; --ink-4:#5d6a7f;
    --accent:#5b9bff; --accent-deep:#2f6fe4; --accent-wash:rgba(91,155,255,.10);
    --ok:#3ddc97; --ok-wash:rgba(61,220,151,.08);
    --err:#ff8585; --warn:#f0b45e;
    --bubble:#1c3657; --bubble-line:#3c5f8c;
    --code:#0c131e; --code-head:#101925;
    --mono:"Cascadia Mono","Cascadia Code",Consolas,ui-monospace,"SFMono-Regular",monospace;
    --sans:"Segoe UI Variable Text","Segoe UI","Microsoft YaHei UI","Microsoft YaHei",system-ui,sans-serif;
    --ease:cubic-bezier(.2,.75,.25,1);
  }
  *{box-sizing:border-box}
  html,body{height:100%;overflow:hidden}
  body{position:fixed;inset:0;margin:0;background:var(--bg);color:var(--ink);
       font:400 14px/1.65 var(--sans);-webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility}
  ::selection{background:rgba(91,155,255,.28);color:#fff}
  *{scrollbar-width:thin;scrollbar-color:#2c3a4e transparent}
  ::-webkit-scrollbar{width:10px;height:10px}
  ::-webkit-scrollbar-track{background:transparent}
  ::-webkit-scrollbar-thumb{background:#2a374a;border-radius:99px;border:3px solid transparent;background-clip:content-box}
  ::-webkit-scrollbar-thumb:hover{background:#3a4a61;border:2px solid transparent;background-clip:content-box}
  :focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:6px}
  button,input,textarea{font:inherit;color:inherit}
  button{border:0;background:none;cursor:pointer}
  .app{display:grid;grid-template-columns:278px minmax(0,1fr);height:100vh;overflow:hidden}
  body.collapsed .app{grid-template-columns:0 minmax(0,1fr)}
  body.collapsed aside{transform:translateX(-100%);opacity:0;pointer-events:none}

  /* ---------------- 侧栏 ---------------- */
  aside{
    background:linear-gradient(180deg,#0c121d 0%,var(--side) 28%,#0a0f18 100%);
    border-right:1px solid var(--line);display:flex;flex-direction:column;
    min-width:0;height:100vh;overflow:hidden;
    transition:transform .2s var(--ease),opacity .2s var(--ease);
  }
  .side-top{display:flex;align-items:flex-start;justify-content:space-between;
            padding:17px 12px 0;flex:0 0 auto}
  .logo{display:flex;align-items:center;gap:10px;min-width:0}
  .logo .mark{
    width:30px;height:30px;border-radius:9px;flex:none;
    background-color:#111a2b;background-image:var(--lumin);
    background-position:center;background-size:cover;background-repeat:no-repeat;
    border:1px solid rgba(150,166,255,.34);
    box-shadow:0 0 16px -6px rgba(122,132,255,.75),inset 0 1px 0 rgba(255,255,255,.14);
  }
  .brandtext{display:flex;flex-direction:column;gap:4px;min-width:0;line-height:1}
  .brandtext b{font-size:14.5px;font-weight:650;letter-spacing:.01em;color:#edf2fa}
  .brandtext small{font:500 8.5px/1 var(--sans);letter-spacing:.17em;color:#7f8ca1}
  .iconbtn{
    display:inline-flex;align-items:center;justify-content:center;width:28px;height:28px;
    border-radius:7px;color:var(--ink-3);font-size:14px;line-height:1;
    transition:background .15s var(--ease),color .15s var(--ease)
  }
  .iconbtn:hover{background:#141e2c;color:var(--ink-2)}
  .iconbtn svg{width:15px;height:15px;display:block}
  #collapse{margin-top:1px;font-size:13px;color:#7a879c}
  #expand{display:none}
  body.collapsed #expand{display:inline-flex}

  .newchat{
    margin:33px 12px 12px;height:41px;padding:0 13px;border-radius:10px;flex:0 0 auto;
    border:1px solid #263650;background:linear-gradient(180deg,#13223a 0%,#101b2e 100%);
    color:#dbe6f7;font-size:13.5px;font-weight:550;display:flex;align-items:center;gap:9px;
    box-shadow:inset 0 1px 0 rgba(255,255,255,.03);
    transition:border-color .16s var(--ease),background .16s var(--ease)
  }
  .newchat:hover{border-color:#36557e;background:linear-gradient(180deg,#162943 0%,#112037 100%)}
  .newchat svg{width:15px;height:15px;color:var(--accent);flex:none}
  .sect{display:flex;align-items:center;justify-content:space-between;padding:0 16px 8px;
        font-size:11.5px;color:var(--ink-3);flex:0 0 auto}
  .sect #cnt{font:500 11px/1 var(--mono);color:#7b889d}
  .chats{flex:1 1 auto;min-height:0;overflow-y:auto;padding:0 9px 8px}
  .chat{
    display:flex;align-items:center;gap:8px;min-height:39px;padding:0 11px;border-radius:9px;
    border:1px solid transparent;color:#aeb9ca;font-size:13px;cursor:pointer;position:relative;
    transition:background .14s var(--ease),border-color .14s var(--ease),color .14s var(--ease)
  }
  .chat:hover{background:#121b28;color:#dbe3ef}
  .chat.on{
    background:linear-gradient(180deg,#182a42 0%,#142137 100%);border-color:#2b405f;color:#edf2f9;
    box-shadow:0 1px 0 rgba(255,255,255,.03) inset
  }
  .chat .t{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .chat .when{font:400 10px/1 var(--mono);color:var(--ink-3);opacity:0;transition:opacity .14s}
  .chat:hover .when{opacity:1}
  .chat.on .when{display:none}
  .chat .pin{font-size:9px;color:var(--accent);margin-left:3px;flex:0 0 auto}
  .chat .del{
    position:absolute;right:6px;top:50%;transform:translateY(-50%);opacity:0;
    width:21px;height:21px;border-radius:6px;background:#1b2635;color:var(--ink-3);
    font-size:12px;line-height:1;padding:0;transition:opacity .14s,color .14s,background .14s
  }
  .chat:hover .del{opacity:1}
  .chat .del:hover{color:var(--err);background:#2a1d24}
  .chat input.rename{
    flex:1;min-width:0;background:#0c141f;border:1px solid #36557e;border-radius:6px;
    color:var(--ink);font:400 12.5px/1.5 var(--sans);padding:3px 6px;outline:0
  }
  .side-foot{border-top:1px solid var(--line);padding:18px 8px 18px;display:flex;flex-direction:column;gap:2px;
             flex:0 0 auto;background:rgba(8,13,21,.3)}
  .footbtn{
    display:flex;align-items:center;gap:9px;min-height:38px;padding:0 9px;border-radius:8px;
    color:var(--ink-2);font-size:13px;text-align:left;transition:background .14s,color .14s
  }
  .footbtn:hover{background:#121b28;color:var(--ink)}
  .footbtn svg{width:15px;height:15px;color:#8b97aa;flex:none}
  .footbtn .k{font:400 10.5px/1 var(--mono);color:var(--ink-3);margin-left:auto}
  .who{display:flex;align-items:center;gap:8px;padding:9px 9px 3px;color:#7c899d;font-size:11.5px}
  .who .av{
    width:26px;height:26px;border-radius:50%;flex:none;
    background-color:#111a2b;background-image:var(--lumin);
    background-position:center;background-size:cover;background-repeat:no-repeat;
    border:1px solid rgba(150,166,255,.30);
    box-shadow:0 0 12px -6px rgba(122,132,255,.8)
  }

  /* ---------------- 主区 ---------------- */
  main{
    display:flex;flex-direction:column;min-width:0;min-height:0;height:100vh;overflow:hidden;
    background:
      radial-gradient(1100px 520px at 82% -18%,rgba(42,78,126,.22) 0%,rgba(10,15,24,0) 62%),
      linear-gradient(180deg,#0b1019 0%,var(--bg) 45%)
  }
  .topbar{
    display:flex;align-items:center;gap:12px;min-height:66px;padding:0 34px;flex:0 0 auto;
    border-bottom:1px solid #171f2b;background:rgba(10,15,24,.82);
    backdrop-filter:blur(10px);z-index:4
  }
  .topbar .ttlwrap{min-width:0;display:flex;flex-direction:column;gap:3px}
  .crumb{display:flex;align-items:center;gap:7px;font:500 9px/1 var(--sans);
         letter-spacing:.16em;color:#8491a6;white-space:nowrap}
  .crumb .sepc{color:#a4794d;letter-spacing:0}
  .ttl{font-size:14.5px;line-height:1.2;font-weight:650;color:#e7ecf4;
       overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:52vw}
  .topbar .sp{flex:1}
  .pill{
    display:inline-flex;align-items:center;gap:7px;height:26px;padding:0 10px;
    border:1px solid #4a2a2d;border-radius:999px;background:rgba(120,42,48,.10);
    font-size:11.5px;color:#e89090;white-space:nowrap
  }
  .pill.on{border-color:#1e4437;background:rgba(28,89,67,.12);color:var(--ok)}
  .dot{display:block;width:6px;height:6px;border-radius:50%;background:var(--err);
       box-shadow:0 0 0 3px rgba(255,123,123,.12);transition:.2s}
  .dot.on{background:var(--ok);box-shadow:0 0 0 3px rgba(61,220,151,.13)}

  .stream{flex:1 1 auto;min-height:0;overflow-y:auto;scroll-behavior:smooth}
  .wrap{max-width:960px;margin:0 auto;padding:34px 26px 24px}
  .empty{max-width:430px;margin:17vh auto 0;text-align:center;color:var(--ink-3)}
  .empty h1{margin:0 0 10px;font-size:23px;line-height:1.4;font-weight:600;color:#c8d1df;letter-spacing:-.01em}
  .empty p{margin:0;font-size:13px;line-height:1.9;color:#748196}
  .empty .keys{display:flex;align-items:center;justify-content:center;gap:7px;margin-top:18px;
               font-size:11.5px;color:#7d8a9f}
  .empty kbd{font:500 10.5px/1 var(--mono);color:#9dabbe;background:#131d2a;border:1px solid #283548;
             border-bottom-color:#33445a;border-radius:5px;padding:4px 6px}

  .row{display:flex;margin:20px 0 22px;animation:in .18s var(--ease)}
  .row.user{justify-content:flex-end;padding-left:46px}
  .bubble{
    max-width:76%;padding:11px 15px;border-radius:12px;white-space:pre-wrap;word-break:break-word;
    background:var(--bubble);border:1px solid var(--bubble-line);color:#eaf1fb;
    font-size:14.5px;line-height:1.55;box-shadow:0 10px 24px -20px #000
  }
  .seg{position:relative;margin:28px 0 24px 46px;animation:in .18s var(--ease)}
  .row.user + .seg{margin-top:61px}
  .ai-mark{
    position:absolute;left:-46px;top:-28px;width:32px;height:32px;border-radius:10px;
    background-color:#111a2b;background-image:var(--lumin);
    background-position:center;background-size:cover;background-repeat:no-repeat;
    border:1px solid rgba(150,166,255,.4);
    box-shadow:0 0 0 4px rgba(105,120,255,.06),0 8px 20px -10px rgba(105,120,255,.85),
               inset 0 1px 0 rgba(255,255,255,.16)
  }
  .seg.think .ai-mark{display:none}
  .asst{color:#c8d2e1;font-size:15px;line-height:1.82;word-break:break-word}
  .asst p{margin:0 0 14px}
  .asst p:last-child{margin-bottom:0}
  .asst h1,.asst h2,.asst h3{margin:4px 0 10px;color:#f0f4fa;font-weight:650;line-height:1.35;letter-spacing:-.01em}
  .asst h1{font-size:21px}
  .asst h2{font-size:19px}
  .asst h3{font-size:17px}
  .asst ul,.asst ol{margin:0 0 14px;padding-left:22px}
  .asst li{margin:3px 0}
  .asst li::marker{color:#65748a}
  .asst strong{color:#f2f5fa;font-weight:650}
  .asst code{font:400 12.5px/1.5 var(--mono);background:#17202d;border:1px solid #263244;
             border-radius:5px;padding:1px 5px;color:#dce5f2}
  /* Markdown 表格：不能把 | 原样丢给用户看 */
  .md-table{margin:16px 0 18px;max-width:100%;overflow:auto;border:1px solid #243347;
            border-radius:10px;background:#0b131e}
  .md-table table{width:100%;border-collapse:collapse;font-size:13px;line-height:1.6}
  .md-table th,.md-table td{padding:9px 12px;border-bottom:1px solid #1d2a3b;text-align:left;
                            vertical-align:top;word-break:break-word}
  .md-table th{background:#111c2a;color:#dfe8f4;font-weight:600;white-space:nowrap}
  .md-table tr:last-child td{border-bottom:0}
  .md-table td{color:#b9c6d8}
  .md-table td code{font-size:11.5px;white-space:nowrap}
  .md-table .ta-c{text-align:center}
  .md-table .ta-r{text-align:right}
  .asst hr{border:0;border-top:1px solid #2a374a;margin:20px 0}

  .code-card{
    margin:40px 0 20px;border:1px solid #233043;border-radius:12px;overflow:hidden;
    background:var(--code);box-shadow:0 24px 55px -46px #000
  }
  .code-head{
    display:flex;align-items:center;gap:8px;height:45px;padding:0 13px 0 15px;
    background:var(--code-head);border-bottom:1px solid #202c3d
  }
  .code-dot{width:7px;height:7px;border-radius:50%;background:var(--accent);flex:none;
            box-shadow:0 0 0 3px rgba(91,155,255,.11)}
  .code-lang{font:500 10.5px/1 var(--mono);letter-spacing:.13em;color:#aab6c8}
  .copy-code{
    margin-left:auto;height:26px;min-width:68px;padding:0 10px;border:1px solid #2a394d;border-radius:6px;
    background:#152132;color:#a6b4c8;font-size:11px;line-height:1;
    transition:border-color .15s,background .15s,color .15s
  }
  .copy-code:hover{border-color:#3a5170;background:#19283b;color:#dbe6f4}
  .copy-code.ok{color:var(--ok);border-color:#225844}
  .code-body{
    margin:0;padding:12px 24px 13px;overflow:auto;background:transparent;
    font:400 13px/1.7 var(--mono);color:#c6d1e0;white-space:pre;tab-size:2
  }
  .code-body code{font:inherit;background:none;border:0;padding:0;color:inherit}

  .tool{
    margin:14px 0 14px 46px;border:1px solid #222e3f;border-radius:11px;overflow:hidden;
    background:linear-gradient(180deg,#101925 0%,#0d1520 100%);
    animation:in .18s var(--ease)
  }
  .tool.run{border-color:#2e4b75;box-shadow:0 10px 30px -26px rgba(91,155,255,.9)}
  .tool.err{border-color:#5a3035}
  .tool-head{display:flex;align-items:center;gap:10px;min-height:42px;padding:0 13px;
             border-bottom:1px solid #1d2736;background:rgba(255,255,255,.012)}
  .tool-name{font:600 12px/1 var(--mono);color:#d3dceb}
  .tool-state{font:500 10.5px/1 var(--mono);color:#7f8ca0;display:flex;align-items:center;gap:6px}
  .tool-ms{margin-left:auto;font:400 10.5px/1 var(--mono);color:#7b889d;font-variant-numeric:tabular-nums}
  .tool-args{padding:9px 13px;font:400 11.5px/1.65 var(--mono);color:#9ca9bc;white-space:pre-wrap;
             word-break:break-all;border-bottom:1px dashed #1d2736}
  .tool-args:empty{display:none}
  .tool-out{margin:0;padding:11px 13px;font:400 11.5px/1.7 var(--mono);color:#c4cfdf;white-space:pre-wrap;
            word-break:break-word;max-height:280px;overflow:auto;background:#0a111b}
  .tool-out:empty{display:none}
  .notice{display:flex;gap:8px;align-items:flex-start;margin:9px 0 9px 46px;font-size:12px;line-height:1.7;color:#7b889d}
  .notice::before{content:"";width:4px;height:4px;border-radius:50%;background:#61718a;flex:none;margin-top:9px}
  .error{font-size:12.5px;line-height:1.65;color:#ff9a9a;background:rgba(255,110,110,.06);
         border:1px solid rgba(255,110,110,.22);border-radius:9px;padding:9px 12px;margin:11px 0 11px 46px;white-space:pre-wrap}
  .spin{display:inline-block;width:10px;height:10px;border:1.5px solid #314158;border-top-color:var(--accent);
        border-radius:50%;animation:spin .75s linear infinite}
  .spin[hidden]{display:none}

)AFWEB";

// Lumin 图标：128px WebP 内联，避免交付时多一个资源文件；按 30/32/26px 显示。
inline constexpr const char* kWebPage1C = R"AFWEB(
  :root{--lumin:url("data:image/webp;base64,UklGRuoHAABXRUJQVlA4IN4HAADQKQCdASqAAIAAPikShkIhoQsmPlwMAUJZQDEWKuSV93/VeaE6LlHPnvGP4Tny+YL+pnUT8yvnCekX/EeoB+yXWm+gn+oHpxezR5TNNUfJflvlixBd7zGs8T+mJmdeTP6N9g79c+th6Nn7GFFWxVvhmC0Ypf00MJd5p/SevZe8fiaz30lEKaeRuPCdgpkdejPy1qDlx4YmY1du/BPNpEaJ79VnVYMfrCkDQjE3uJulzbNd5Klvh+5JtpSYak1BtJmAKObw0s7nTDATo+61OOTnXARctJXGDnrnM2QeDsfz5bWp+bAE13pjG+Mfeyk16OMc+/XWzMD6Nc0PN1ZwYmNGKAepcyc9UrLLC6qRZ3TvZKhym0CfTtiwzJImE0HvOppWnv93XOwfgs6lGTJpc5ojy1FjuHhP+6Ej+dOfLmC/OmOhSzgaSfIiN7b8H9f3V7nRMomM3EIAAP7+g7GDHl3/4ETqu8M5wfoJByPvoFFu4+wGnqzNMNVzbkggf7S4Acs899uy8z4EOIQpFtxh5jmD9n2f1Xuv33Ub/oBUlfzXEfE7LFW9cFlCnXK/op9377SC1uOFEYc6b8wdlrqId0USegH8BD1+m51tfM9Z39eM5a3VhtWWe4bwykomcvFKstBlnsc4g4WTjTxSp7WXwgsmkmY0ClAHq8SdLp8Z9Ad8xHXyOmAPntlzgkjFy8TK/MYK3v6MEjgq6FOdm7aqs93mpgtlVpxT0E1y0D4iVBRcWY/MUy89zT2v4PjBd1Hnlhc3dXpGxsZBGTV+fMHlvJ9o2BdTHhtmVIAJtjl07rXBPX8la7FwWgiB+Lh9/RuyHPmqhOOcUDDJVSSZu32a55rO2QjXivUP9frFoD0RloriKJ3+2Cl2xYkLg6zx6HhU8NaN3WxWq8L6nX2KLcCkLVj4ZAQwa13w1U/vgdCcOW/+ec+mfkIC+Rtja9KwbARk/zvnJnqWkdd47HOt+q9uQE4aWqo/Cal9r2I4kVBrBBa451WTM4ENcsSKL3Bjac7Az5gwuNcdg1Q8TmdwgeJfKWKN+8bg5R6TdZOuwqZJsAU1PkjD6TgV17GHXeG9gXUz6E/c88lVqO4lrbiUG6i9VrCitH2k54CqoS8YsHNm54mX5fhfVXRUaebhFsEUHKWkoRIC7W27DgIIMX1Oo7YBgFS1eYc6ZLW7pSbSX+vKra+1BAzgP6u/zi5UtrL/uI6ruEeJEb90liZ63/pSuSlZmgqPSz7Nv+4YRyLJf665yD7iEKOhvh2v0alRk0iPuHqPUSvw+0+YPHyb3NrTGWnGnVNTsNH0QgYw7gSqIk/630VStcrSiNBlVV5l8CKu8eN39J0XG9DmDzZi92Ya2VL5OHOqHdkytRf+AtdFAqEBxHsjMvOZUmNqLxNYkrnNGK7TwfwQ0AEf8LmfY+JQpUImO5n6X70kg1DiZAAfQarMV8IXnZiZP25uW/GsWa73FbGipymjjcqc9IYXrVrj0Ygq66ArX8vl7bB1FxwvUKStnIhKaaFAJA2LAEKIV+21uf39Zd5U3r/rkrU68i/qK2TBtv9CiJ+3ld7hS8hmtcbMKbhT9Hm2vPZyaC9eNlhevHbazhvv20mmELD0QOmE/ALwp+N8OWgjuPqukHMMPWOLVXzEzk2eOR9hngGJW5f/tztyL3m7fbmPNiLpE/kXOmWPru++/G0EClK/wcR/85ZdQtYizSfaiwrld+TqTld/44853M4eeEdeoFXMbs1fr07k3VOVuIuPGG/kEPI9MJ+ELwyZGUStxhSy+oO61lFrksf0oZNdJjNwi+IF331v1p72HdZbHB+8ArUuK7lOlOe3uHyOEDWM8PHg9SWPHSlan0x5c2KlX7xPXWoetrYUXn83OAXiCe/ZozF0JtGFUCcrBaiwb9soo+z5d/TffRszjkxRxwsccVozK03KqOC9zN0PAbp/vRZpDBCe6hhO8Msbkw7zKgy6d8NAPZYhT2mpwtFfufVhjmkAyk/C6izGhffs0MiUtwTTKo3vhlu7bRjRhcfMiLgZFzar6n7TB4jNrBn4A4uQlUy4rrDQCJukpGPpIm21NsWauw0lAGHGNAv7je8sNDzSrX4eTFQc12vkickCOExOMyOA3ZP/KrCTl/g6NKfoF3Hz9R/HZYxgU9Ew1NKyY9uzr89N0mM9YZDGrIErUIRiiMxdUxVwaSf4wPGbrmr9kfP+rbr3CRaSwwRJr07U8R/If/iT+cdRgCNRefmsvbgXVqfN7W1VmjTfdA9Rf8p9vBd1jeX3XWgqJv3lzLZmdQGQ0AI146cs1ToIsm8mg2n4TV4n1uLHclsQ8sTRYCGakiUyTix7Gish7QxdCIA3CcWMDjBt7drG7wtGZk7Fo2kHpAHPxd4GOIFrIqBVhkKA4mivyjXhERwe3phb6X1F3AhxXLc2GVJgLiLBzHgCcitcjTJotov+lLJAUVJFTbN2Y7v0dtlCYd07Et1lHNjAcbU/MotlQF+fMri/zuyicDxDgvhkqPWYJbtGAKGwKLIDTUaNL+o2pCkPl/wc/f5Ma3PTgGFkNZ7dQvDUC9PRuFGCvcT84gX8dtRyIfVC5oN/1R6GK73E4L4FJnW/trVHT4AdYUyFHculJ51e+0JfjHsWpuvQ37kTaKEaUnHGmp+RyuWxSa+d64MM28dwuwu9AaEqX95X28jrvIYNIAAA")}
)AFWEB";

inline constexpr const char* kWebPage1B = R"AFWEB(
  /* ---------------- 思考过程 ---------------- */
  /* ★ 推理正文 + 它触发的工具卡片 + 步骤提示，全装进同一个可折叠块：
       收起后只剩一行摘要，答案自然占住视线。 */
  .seg.think{margin:20px 0 16px 46px}
  .row.user + .seg.think{margin-top:26px}   /* 过程块没有头像，不用像答案那样让出 61px */
  .seg-head{
    display:none;align-items:center;gap:7px;width:100%;padding:2px 0;text-align:left;
    color:#748197;font:500 12px/1.35 var(--sans);user-select:none;cursor:pointer
  }
  .seg.think .seg-head{display:flex}
  .seg-head:hover{color:var(--ink-2)}
  .seg-head .chev{display:inline-block;font-size:9px;opacity:.8;transition:transform .15s var(--ease)}
  .seg.think.open .seg-head .chev{transform:rotate(90deg)}
  .seg-head .meta{font:400 10.5px/1 var(--mono);opacity:.75;font-variant-numeric:tabular-nums}
  .seg-head .ln{flex:1;height:1px;margin-left:2px;background:linear-gradient(90deg,#28364a,rgba(40,54,74,0))}
  /* ★ 展开 / 收起思考的开关就长在"思考过程"这一行右端（不再挂顶栏右上角） */
  .seg-toggle{
    flex:none;display:inline-flex;align-items:center;gap:4px;height:22px;padding:0 8px;
    border:1px solid #263346;border-radius:999px;background:#0f1724;color:#8b99ad;
    font:500 11px/1 var(--sans);white-space:nowrap;
    transition:background .15s var(--ease),border-color .15s var(--ease),color .15s var(--ease)
  }
  .seg-toggle:hover{background:#131f2f;border-color:#34465d;color:#d6dfec}
  .seg-toggle svg{width:9px;height:9px;flex:none;transition:transform .15s var(--ease)}
  .seg.think.open .seg-toggle{color:#c3cfe2;border-color:#34465d;background:#131f2f}
  .seg.think.open .seg-toggle svg{transform:rotate(180deg)}
  .seg-body{display:none;margin-top:9px;padding:2px 0 2px 13px;border-left:1px solid #273446}
  .seg.think.open .seg-body{display:block}
  .seg-body .asst{color:#8390a5;font-size:12.5px;line-height:1.8;word-break:break-word}
  .seg-body .asst + .asst{margin-top:8px}
  .seg-body .asst p{margin:0 0 10px}
  .seg-body .asst p:last-child{margin-bottom:0}
  .seg-body .asst strong{color:#aeb9ca}
  .seg-body .asst code{font-size:11.5px;background:#131c28;border-color:#243044}
  .seg-body .tool{
    margin:11px 0;border-color:#1f2a3a;
    background:linear-gradient(180deg,#0e1723 0%,#0b131e 100%)
  }
  .seg-body .tool-head{min-height:36px}
  .seg-body .tool-out{max-height:220px}
  .seg-body .notice,.seg-body .error{margin:8px 0}

  /* ---------------- 输入区 ---------------- */
  .composer-wrap{
    flex:0 0 auto;padding:8px 34px 20px;
    background:linear-gradient(180deg,rgba(10,15,24,0) 0%,rgba(10,15,24,.92) 34%,var(--bg) 100%)
  }
  .composer{
    max-width:960px;margin:0 auto;min-height:118px;padding:13px 13px 10px;
    display:flex;flex-direction:column;background:#0d1522;border:1px solid #293a50;
    border-radius:14px;box-shadow:0 26px 70px -50px #000,inset 0 1px 0 rgba(255,255,255,.025);
    transition:border-color .16s var(--ease),box-shadow .16s var(--ease)
  }
  .composer:focus-within{border-color:#3d5c83;box-shadow:0 0 0 1px rgba(91,155,255,.08),0 26px 70px -50px #000}
  .composer.drag{border-color:var(--accent);box-shadow:0 0 0 2px rgba(91,155,255,.14)}
  textarea{
    width:100%;min-height:28px;max-height:170px;resize:none;border:0;outline:0;background:transparent;
    color:var(--ink);font:400 14.5px/1.65 var(--sans);padding:2px 6px
  }
  textarea:focus-visible{outline:none}
  textarea::placeholder{color:#7a879c}
  .cbar{display:flex;align-items:center;gap:7px;margin-top:auto;padding-top:9px;position:relative}
  .cbtn{
    display:inline-flex;align-items:center;justify-content:center;gap:6px;height:30px;padding:0 9px;
    border:1px solid #263346;border-radius:8px;background:#101927;color:#98a5b9;
    font-size:11.5px;line-height:1;white-space:nowrap;
    transition:border-color .15s,background .15s,color .15s
  }
  .cbtn:hover{border-color:#3a4f6b;background:#131f2f;color:#d6dfec}
  .cbtn.icon-only{width:32px;padding:0}
  .cbtn svg{width:13px;height:13px;flex:none}
  .cbtn.warn{border-color:#5c4124;color:#eab267}
  .cbar .sp{flex:1}
  .sendbtn{
    width:36px;height:36px;border:1px solid #4f8cdb;border-radius:10px;color:#fff;
    display:flex;align-items:center;justify-content:center;flex:none;
    background:linear-gradient(180deg,#5b9bff 0%,#2f6fe4 100%);
    box-shadow:0 8px 18px -10px rgba(47,111,228,.9),inset 0 1px 0 rgba(255,255,255,.3);
    transition:filter .15s var(--ease),transform .15s var(--ease)
  }
  .sendbtn:hover{filter:brightness(1.07)}
  .sendbtn:active{transform:translateY(1px)}
  .sendbtn:disabled{opacity:.4;filter:none;cursor:default;box-shadow:none}
  .sendbtn svg{width:15px;height:15px}

  .menu,.ctx{
    position:absolute;background:#0b121d;border:1px solid #2a384c;border-radius:10px;padding:5px;
    box-shadow:0 26px 55px -26px #000,inset 0 1px 0 rgba(255,255,255,.03);z-index:40
  }
  .menu{bottom:calc(100% + 9px);left:38px;min-width:190px}
  .ctx{position:fixed;left:0;top:0;min-width:164px;z-index:60}
  .menu[hidden],.ctx[hidden]{display:none}
  .mi{padding:8px 10px;border-radius:7px;font-size:12.5px;line-height:1.25;color:#a8b4c6;cursor:pointer}
  .mi:hover{background:#151f2e;color:#e4eaf3}
  .mi.on{background:#13243d;color:#cfe0ff}
  .mi.danger{color:#e6b274}
  .mi .sub{display:block;margin-top:4px;font:400 10.5px/1.4 var(--mono);color:#7b889d}

  .files{display:flex;flex-wrap:wrap;gap:6px;padding:1px 6px 7px}
  .files:empty{display:none}
  .file{display:inline-flex;align-items:center;gap:7px;padding:5px 8px;border:1px solid #263346;
        border-radius:7px;background:#111b29;font:400 11px/1 var(--mono);color:#8f9db1}
  .file b{font-weight:500;color:#ced9e8}
  .file span{color:#7b889d}
  .file button{border:0;background:transparent;color:#718096;font-size:13px;line-height:1;padding:0 1px}
  .file button:hover{color:var(--err)}

  .stats{max-width:960px;margin:7px auto 0;display:flex;align-items:center;gap:8px;
         font:400 11px/1.4 var(--mono);color:#77849a}
  .stats .sp{flex:1}
  .stats .d{color:#334154}
  .stats b{color:#9eabbd;font-weight:400;font-variant-numeric:tabular-nums}
  #clock{display:none}

  /* ---------------- 弹窗 ---------------- */
  .modal-wrap{
    position:fixed;inset:0;z-index:80;display:flex;align-items:center;justify-content:center;
    padding:20px;background:rgba(4,8,14,.66);backdrop-filter:blur(7px);animation:fade .16s var(--ease)
  }
  .modal-wrap[hidden]{display:none !important}
  .modal{
    width:min(520px,94vw);padding:20px;border:1px solid #2a3a4f;border-radius:14px;
    background:linear-gradient(180deg,#101925 0%,#0c141f 100%);
    box-shadow:0 46px 90px -40px #000,inset 0 1px 0 rgba(255,255,255,.04);
    animation:pop .18s var(--ease)
  }
  .modal h2{margin:0 0 6px;display:flex;align-items:center;gap:8px;font-size:15px;font-weight:650;color:#edf2fa}
  .modal h2 .badge{
    font:500 10px/1 var(--mono);color:var(--warn);border:1px solid rgba(240,180,94,.32);
    background:rgba(240,180,94,.08);padding:4px 7px;border-radius:999px
  }
  .modal label{display:block;margin:15px 0 0;font-size:12px;color:#95a2b5}
  .modal input{
    width:100%;margin-top:7px;padding:10px 11px;border:1px solid #29384c;border-radius:8px;
    background:#0a111b;color:var(--ink);font:400 13px/1.4 var(--mono);outline:0
  }
  .modal input:focus{border-color:#3a5b83;box-shadow:0 0 0 2px rgba(91,155,255,.08)}
  pre.prompt{
    margin:12px 0 16px;padding:12px;max-height:220px;overflow:auto;white-space:pre-wrap;word-break:break-all;
    border:1px solid #263346;border-radius:9px;background:#0a111b;
    font:400 12px/1.65 var(--mono);color:#c7d2e1
  }
  .modal-actions{display:flex;justify-content:flex-end;gap:9px;margin-top:18px}
  .modal-note{margin-top:12px;font-size:11.5px;line-height:1.75;color:#7d8a9f}
  .modal button{
    height:34px;padding:0 15px;border:1px solid #2b394d;border-radius:8px;
    background:#121c2a;color:#aab6c7;font-size:12.5px;font-weight:500;
    transition:background .15s,border-color .15s,color .15s
  }
  .modal button:hover{background:#172335;color:#e4eaf3;border-color:#3a4d67}
  .modal button.primary{
    border-color:#4f8cdb;background:linear-gradient(180deg,#5b9bff,#2f6fe4);color:#fff;
    box-shadow:inset 0 1px 0 rgba(255,255,255,.22)
  }
  .modal button.primary:hover{filter:brightness(1.07)}
  .modal button:disabled{opacity:.45;cursor:default}

  @keyframes spin{to{transform:rotate(360deg)}}
  @keyframes in{from{opacity:0;transform:translateY(3px)}to{opacity:1;transform:none}}
  @keyframes pop{from{opacity:0;transform:scale(.985) translateY(5px)}to{opacity:1;transform:none}}
  @keyframes fade{from{opacity:0}to{opacity:1}}

  @media (max-width:980px){
    .app{grid-template-columns:244px minmax(0,1fr)}
    .topbar{padding:0 24px}
    .composer-wrap{padding:8px 24px 10px}
  }
  @media (max-width:760px){
    .app,body.collapsed .app{grid-template-columns:minmax(0,1fr)}
    aside{position:absolute;inset:0 auto 0 0;z-index:30;width:278px;transform:translateX(-100%);
          opacity:0;pointer-events:none;box-shadow:24px 0 60px -40px #000}
    body:not(.collapsed) aside{transform:none;opacity:1;pointer-events:auto}
    body.collapsed #expand{display:inline-flex}
    .topbar{padding:0 16px;min-height:62px}
    .wrap{padding:26px 17px 30px}
    .row.user{padding-left:0}
    .seg,.seg.think,.tool,.notice,.error{margin-left:0}
    .row.user + .seg{margin-top:32px}
    .ai-mark{display:none}
    .composer-wrap{padding:8px 14px 8px}
    .bubble{max-width:92%}
    .ttl{max-width:56vw}
    .empty{margin-top:12vh}
  }
  @media (prefers-reduced-motion:reduce){
    *,*::before,*::after{animation-duration:.01ms !important;transition-duration:.01ms !important;scroll-behavior:auto !important}
  }

</style>
</head>
<body>
<div class="app">
<aside>
  <div class="side-top">
    <div class="logo">
      <i class="mark" aria-hidden="true"></i>
      <span class="brandtext"><b>Lumin</b><small>CODING WORKSPACE</small></span>
    </div>
    <button class="iconbtn" id="collapse" title="收起侧栏" aria-label="收起侧栏">«</button>
  </div>
  <button class="newchat" id="newchat">
    <svg viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M8 3v10M3 8h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>
    <span>新对话</span>
  </button>
  <div class="sect"><span>对话</span><span id="cnt">0</span></div>
  <div class="chats" id="chats"></div>
  <div class="side-foot">
    <button class="footbtn" id="optset">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" aria-hidden="true"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.83 2.83-.06-.06A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 .6 1.7 1.7 0 0 0-.4 1.1V21h-4v-.09A1.7 1.7 0 0 0 8.6 19.4a1.7 1.7 0 0 0-1.88.34l-.06.06-2.83-2.83.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-.6-1 1.7 1.7 0 0 0-1.1-.4H3v-4h.09A1.7 1.7 0 0 0 4.6 8.6a1.7 1.7 0 0 0-.34-1.88l-.06-.06 2.83-2.83.06.06A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-.6 1.7 1.7 0 0 0 .4-1.1V3h4v.09A1.7 1.7 0 0 0 15.4 4.6a1.7 1.7 0 0 0 1.88-.34l.06-.06 2.83 2.83-.06.06A1.7 1.7 0 0 0 19.4 9c.14.38.38.72.7 1 .3.27.68.42 1.08.43H21v4h-.09A1.7 1.7 0 0 0 19.4 15Z"/></svg>
      <span>设置</span><span class="k">API Key</span>
    </button>
    <div class="who"><span class="av" aria-hidden="true"></span><span id="whoTxt">本地 · Web UI</span></div>
  </div>
</aside>
)AFWEB";

inline constexpr const char* kWebPage3 = R"AFWEB(<main>
  <div class="topbar">
    <button class="iconbtn" id="expand" title="展开侧栏" aria-label="展开侧栏">
      <svg viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M3 5h10M3 8h10M3 11h10" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/></svg>
    </button>
    <div class="ttlwrap">
      <div class="crumb"><span>WORKSPACE</span><span class="sepc">/</span><span>对话</span></div>
      <span class="ttl" id="ttl">新对话</span>
    </div>
    <span class="sp"></span>
    <span class="pill" id="connpill" aria-live="polite"><span class="dot" id="dot"></span> <span id="conn">连接中…</span></span>
  </div>
  <div class="stream" id="stream">
    <div class="wrap" id="wrap">
      <div class="empty" id="empty">
        <h1>今天想让 Lumin 做什么？</h1>
        <p>描述一个任务。Lumin 会读取文件、运行工具，并把每一步留在这里。</p>
        <div class="keys"><span><kbd>Enter</kbd> 发送</span><span>·</span><span><kbd>Shift</kbd> + <kbd>Enter</kbd> 换行</span></div>
      </div>
    </div>
  </div>
  <div class="composer-wrap">
    <div class="composer" id="composer">
      <div class="files" id="files"></div>
      <textarea id="input" rows="1" placeholder="给 Lumin 发一条指令…" aria-label="给 Lumin 发一条指令" spellcheck="false"></textarea>
      <div class="cbar">
        <button class="cbtn icon-only" id="plus" title="添加附件" aria-label="添加附件">
          <svg viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M8 3v10M3 8h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>
        </button>
        <button class="cbtn" id="modeBtn" title="权限模式" aria-haspopup="true" aria-expanded="false">
          <span id="modeLabel">每次询问权限</span>
          <svg viewBox="0 0 12 12" fill="none" aria-hidden="true"><path d="m3 4.5 3 3 3-3" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg>
        </button>
        <div class="menu" id="modeMenu" hidden>
          <div class="mi on" data-v="ask">每次询问权限<span class="sub">工具执行前弹窗问你</span></div>
          <div class="mi danger" data-v="auto">自动允许（危险）<span class="sub">不再询问，全部放行</span></div>
        </div>
        <button class="cbtn" id="compact" title="压缩上下文：把较早的历史总结成一份交接摘要">压缩上下文</button>
        <span class="sp"></span>
        <button class="sendbtn" id="send" title="发送" aria-label="发送">
          <svg viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M8 12.6V3.4M4.4 7 8 3.4 11.6 7" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg>
        </button>
      </div>
    </div>
    <div class="stats">
      <span id="hint">等待任务…</span>
      <span class="d">|</span>
      <span><b id="stTurns">0</b> 轮</span><span class="d">·</span>
      <span><b id="stSteps">0</b> 步</span><span class="d">·</span>
      <span><b id="stTok">0</b> tok</span>
      <span class="sp"></span>
      <span id="clock"></span>
    </div>
  </div>
</main>
</div>

<input type="file" id="file" multiple hidden>

<div class="ctx" id="ctxmenu" hidden>
  <div class="mi" data-act="pin"><span id="pinLabel">置顶</span></div>
  <div class="mi" data-act="rename">重命名</div>
  <div class="mi danger" data-act="del">删除对话</div>
</div>

<div class="modal-wrap" id="modal" hidden>
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="permTitle">
    <h2 id="permTitle">权限请求 <span class="badge">需要你决定</span></h2>
    <pre class="prompt" id="modal-prompt"></pre>
    <div class="modal-actions">
      <button id="deny">拒绝</button>
      <button id="allow" class="primary">允许</button>
    </div>
    <div class="modal-note">选择只对这一条工具调用生效；关掉页面等于拒绝。<br>
      不想被反复打断：把输入框左边的权限模式改成「自动允许」。</div>
  </div>
</div>

<div class="modal-wrap" id="setmodal" hidden>
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="setTitle">
    <h2 id="setTitle">设置</h2>
    <label>DeepSeek API Key<input id="skey" type="password" placeholder="sk-..." autocomplete="off"></label>
    <div class="modal-note">只认 DeepSeek：接口 <b>https://api.deepseek.com/v1</b>、模型 <b>deepseek-chat</b>，
      这两项写死了。<br>Key 保存在本机浏览器（localStorage），不会上传到任何地方；保存后下一条消息就生效。</div>
    <div class="modal-actions">
      <button id="scancel">取消</button>
      <button class="primary" id="ssave">保存并应用</button>
    </div>
  </div>
</div>
)AFWEB";

inline constexpr const char* kWebPage4 = R"AFWEB(<script>
(function () {
  "use strict";
  var $ = function (s) { return document.querySelector(s); };
  var streamEl = $("#stream"), wrap = $("#wrap"), input = $("#input"), sendBtn = $("#send");
  var seg = null, cards = {}, perm = null, busy = false, atBottom = true;
  var FILE_MAX = 200 * 1024;          // 单个附件的上限
  var ATTACHED = [];                  // 待发送的附件 {name, text}
  var KEY = "af_web_ui_v3";
  // ★ 只接 DeepSeek：接口地址和模型写死，设置里只留 API Key。
  var BASE = "https://api.deepseek.com/v1", MODEL = "deepseek-chat";

  // ---------- 状态：对话 + 设置全部落 localStorage ----------
  var S = {};
  try { S = JSON.parse(localStorage.getItem(KEY)) || {}; } catch (e) { S = {}; }
  var chats = S.chats || [];
  var activeId = S.active || null;
  var settings = S.settings || { apiKey: "" };
  // ★ 用户要求：文件/工具权限默认全部放行，不再每次弹窗。
  //   仍保留下拉框；改成"每次询问"后本次页面会话会恢复弹窗。
  var permMode = "auto";
  var saveTimer = null;
  function save() {
    if (saveTimer) return;
    saveTimer = setTimeout(function () {
      saveTimer = null;
      try {
        localStorage.setItem(KEY, JSON.stringify(
          { chats: chats, active: activeId, settings: settings, permMode: permMode }));
      } catch (e) {}
    }, 250);
  }

  function esc(s) { return String(s == null ? "" : s); }
  function htmlEsc(s) {
    return esc(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                 .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
  }
  // ★ 极简 Markdown：标题、段落、列表、```代码块```、表格、`行内代码`、**粗体**。
  //   顺序不能反：先转义，再只把认得的标记还原成标签 —— 先替换再转义就是 XSS 口子。
  function inlineMd(s) {
    var e = htmlEsc(s);
    e = e.replace(/`([^`\n]+)`/g, "<code>$1</code>");
    e = e.replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>");
    return e;
  }
  // ---- Markdown 表格 ----
  // 模型经常输出：
  //   | 函数 | 作用 |
  //   |---|---|
  //   | build | ... |
  // 以前这几行会落进普通段落，用户看到的就是一堆竖线。
  function tableCells(line) {
    var s = String(line).trim();
    if (s.charAt(0) === "|") s = s.slice(1);
    if (s.charAt(s.length - 1) === "|") s = s.slice(0, -1);
    return s.split("|").map(function (x) { return x.trim(); });
  }
  function isTableSep(line) {
    var s = String(line).trim();
    if (s.indexOf("|") < 0) return false;
    var cells = tableCells(s);
    if (!cells.length) return false;
    return cells.every(function (c) { return /^:?-{3,}:?$/.test(c); });
  }
  function tableAlign(c) {
    var left = c.charAt(0) === ":", right = c.charAt(c.length - 1) === ":";
    return (left && right) ? "ta-c" : right ? "ta-r" : "";
  }
  function tableHtml(head, seps, rows) {
    var aligns = seps.map(tableAlign);
    var h = '<div class="md-table"><table><thead><tr>';
    for (var i = 0; i < head.length; i++) {
      h += '<th' + (aligns[i] ? ' class="' + aligns[i] + '"' : '') + '>' +
           inlineMd(head[i] || "") + "</th>";
    }
    h += "</tr></thead><tbody>";
    for (var r = 0; r < rows.length; r++) {
      h += "<tr>";
      for (var k = 0; k < head.length; k++) {
        h += "<td" + (aligns[k] ? ' class="' + aligns[k] + '"' : '') + '>' +
             inlineMd(rows[r][k] || "") + "</td>";
      }
      h += "</tr>";
    }
    return h + "</tbody></table></div>";
  }
  function codeLabel(lang) {
    var v = String(lang || "").toLowerCase();
    var map = { cpp: "CPP", "c++": "CPP", cxx: "CPP", c: "C", js: "JS", javascript: "JS",
                ts: "TS", typescript: "TS", py: "PY", python: "PY", json: "JSON",
                html: "HTML", css: "CSS", bash: "BASH", sh: "SH", shell: "SH", sql: "SQL" };
    return map[v] || (v ? v.toUpperCase().slice(0, 10) : "CODE");
  }
  function codeCard(lang, code) {
    return '<div class="code-card"><div class="code-head"><span class="code-dot"></span>' +
      '<span class="code-lang">' + htmlEsc(codeLabel(lang)) + '</span>' +
      '<button class="copy-code" type="button" aria-label="复制代码" title="复制这段代码">复制代码</button></div>' +
      '<pre class="code-body"><code>' + htmlEsc(code) + "</code></pre></div>";
  }
  function md(raw) {
    var lines = esc(raw).replace(/\r\n?/g, "\n").split("\n");
    var out = [], para = [];
    function flushPara() {
      if (!para.length) return;
      out.push("<p>" + para.join("<br>") + "</p>");
      para = [];
    }
    for (var i = 0; i < lines.length;) {
      var line = lines[i];
      if (line.slice(0, 3) === "```") {
        var m = line.match(/^```\s*([A-Za-z0-9_+#.-]*)\s*$/);
        var j = i + 1, code = [];
        while (j < lines.length && !/^```\s*$/.test(lines[j])) { code.push(lines[j]); j++; }
        if (m && j < lines.length) {
          flushPara();
          out.push(codeCard(m[1], code.join("\n")));
          i = j + 1;
          continue;
        }
      }
      if (/^\s*$/.test(line)) { flushPara(); i++; continue; }
      // 分隔线：模型常用 --- 分段，不能原样显示成三个减号
      if (/^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/.test(line)) {
        flushPara();
        out.push("<hr>");
        i++;
        continue;
      }
      // 表格：当前行有 |，下一行是 |---|:---:| 这种分隔行
      if (line.indexOf("|") >= 0 && i + 1 < lines.length && isTableSep(lines[i + 1])) {
        flushPara();
        var head = tableCells(line);
        var seps = tableCells(lines[i + 1]);
        var rows = [], k2 = i + 2;
        while (k2 < lines.length && /\S/.test(lines[k2]) && lines[k2].indexOf("|") >= 0) {
          rows.push(tableCells(lines[k2]));
          k2++;
        }
        out.push(tableHtml(head, seps, rows));
        i = k2;
        continue;
      }
      var h = line.match(/^(#{1,3})\s+(.+?)\s*$/);
      if (h) {
        flushPara();
        var lv = h[1].length;
        out.push("<h" + lv + ">" + inlineMd(h[2]) + "</h" + lv + ">");
        i++; continue;
      }
      if (/^\s*[-*]\s+/.test(line)) {
        flushPara();
        var items = [];
        while (i < lines.length && /^\s*[-*]\s+/.test(lines[i])) {
          items.push("<li>" + inlineMd(lines[i].replace(/^\s*[-*]\s+/, "")) + "</li>");
          i++;
        }
        out.push("<ul>" + items.join("") + "</ul>");
        continue;
      }
      if (/^\s*\d+\.\s+/.test(line)) {
        flushPara();
        var oitems = [];
        while (i < lines.length && /^\s*\d+\.\s+/.test(lines[i])) {
          oitems.push("<li>" + inlineMd(lines[i].replace(/^\s*\d+\.\s+/, "")) + "</li>");
          i++;
        }
        out.push("<ol>" + oitems.join("") + "</ol>");
        continue;
      }
      para.push(inlineMd(line));
      i++;
    }
    flushPara();
    return out.join("");
  }

  // 代码块复制：事件委托，流式重渲染后按钮仍然是活的。
  wrap.addEventListener("click", function (e) {
    var btn = e.target.closest(".copy-code");
    if (!btn) return;
    var card = btn.closest(".code-card");
    var codeEl = card && card.querySelector(".code-body code");
    if (!codeEl) return;
    var done = function (ok) {
      btn.textContent = ok ? "已复制" : "复制失败";
      btn.classList.toggle("ok", !!ok);
      setTimeout(function () { btn.textContent = "复制代码"; btn.classList.remove("ok"); }, 1400);
    };
    var text = codeEl.textContent;
    function legacyCopy() {
      var ta = document.createElement("textarea");
      ta.value = text; ta.style.position = "fixed"; ta.style.opacity = "0";
      document.body.appendChild(ta); ta.select();
      var ok = false;
      try { ok = document.execCommand("copy"); } catch (err) {}
      document.body.removeChild(ta);
      return ok;
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { done(true); }, function () { done(legacyCopy()); });
      return;
    }
    done(legacyCopy());
  });
  function nearBottom() {
    return streamEl.scrollHeight - streamEl.scrollTop - streamEl.clientHeight < 90;
  }
  function scroll() { if (atBottom) streamEl.scrollTop = streamEl.scrollHeight; }
  streamEl.addEventListener("scroll", function () { atBottom = nearBottom(); });

  // ---------- 对话 ----------
  function chatById(id) {
    for (var i = 0; i < chats.length; i++) if (chats[i].id === id) return chats[i];
    return null;
  }
  function activeChat() {
    if (!activeId || !chatById(activeId)) {
      if (!chats.length) return null;
      activeId = chats[0].id;
    }
    return chatById(activeId);
  }
  function newChat() {
    // ★ lastSeq / epoch：服务端每条事件都带 seq；这两个字段让页面知道
    //   "这个对话已经看到第几条"，复连时只补后面的（见 ws.onmessage 的 replay）。
    var c = { id: "c" + Date.now().toString(36), title: "新对话", ts: Date.now(),
              events: [], turns: 0, steps: 0, tok: 0, pinned: false,
              lastSeq: 0, epoch: null };
    chats.unshift(c);
    activeId = c.id;
    save(); renderSidebar(); renderStream(); input.focus({preventScroll:true});
    return c;
  }
  function delChat(id) {
    chats = chats.filter(function (c) { return c.id !== id; });
    if (activeId === id) activeId = chats.length ? chats[0].id : null;
    save(); renderSidebar(); renderStream();
  }
  function ago(ts) {
    var d = (Date.now() - ts) / 1000;
    if (d < 60) return "刚刚";
    if (d < 3600) return Math.floor(d / 60) + " 分钟前";
    if (d < 86400) return Math.floor(d / 3600) + " 小时前";
    return Math.floor(d / 86400) + " 天前";
  }
)AFWEB";

// 思考过程块：推理正文 + 工具卡片 + 步骤提示收在同一个可折叠容器里。
// 单独成段，免得把 kWebPage5 顶过 MSVC 的 16384 字节上限（C2026）。
inline constexpr const char* kWebPage4B = R"AFWEB(
  // ---------- 思考过程块 ----------
  // ★ 一段"过程" = 推理正文 + 它触发的工具卡片 + 步骤提示。这些都不是答案，
  //   所以装进同一个可折叠块：收起后只剩一行摘要，答案就能占住视线。
  //   展开 / 收起的开关长在这一行右端 —— 以前挂在顶栏右上角，离内容半个屏幕远，
  //   点之前还得先瞄一眼它现在写的是"展开"还是"收起"。
  var proc = null;                  // 当前这一轮的过程块（null = 这轮还没开始 / 已经结束）
  function newProc() {
    var box = document.createElement("div");
    box.className = "seg think";
    var mark = document.createElement("span");
    mark.className = "ai-mark"; mark.setAttribute("aria-hidden", "true");
    var head = document.createElement("div");
    head.className = "seg-head";
    var chev = document.createElement("span"); chev.className = "chev"; chev.textContent = "▶";
    var ttl  = document.createElement("span"); ttl.className  = "ttl";  ttl.textContent = "思考过程";
    var meta = document.createElement("span"); meta.className = "meta";
    var ln   = document.createElement("span"); ln.className   = "ln";
    var btn  = document.createElement("button");
    btn.className = "seg-toggle"; btn.type = "button";
    btn.setAttribute("aria-expanded", "false");
    btn.innerHTML =
      '<svg viewBox="0 0 12 12" fill="none" aria-hidden="true"><path d="m3 4.5 3 3 3-3" ' +
      'stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg>' +
      '<span class="lb">展开思考</span>';
    head.appendChild(chev); head.appendChild(ttl); head.appendChild(meta);
    head.appendChild(ln);   head.appendChild(btn);
    var body = document.createElement("div");
    body.className = "seg-body";
    box.appendChild(mark); box.appendChild(head); box.appendChild(body);
    wrap.appendChild(box);
    ensureNoEmpty();

    var p = { box: box, head: head, body: body, meta: meta, btn: btn,
              reason: null, tools: 0, touched: false };
    p.syncMeta = function () {
      var chars = 0, nodes = body.querySelectorAll(".asst");
      for (var i = 0; i < nodes.length; i++) {
        chars += Number(nodes[i].getAttribute("data-chars") || 0);
      }
      var bits = [];
      if (chars) bits.push(chars + " 字");
      if (p.tools) bits.push(p.tools + " 个工具");
      meta.textContent = bits.join(" · ");
    };
    p.setOpen = function (v) {
      box.classList.toggle("open", !!v);
      btn.setAttribute("aria-expanded", String(!!v));
      btn.querySelector(".lb").textContent = v ? "收起思考" : "展开思考";
      btn.title = v ? "收起这段思考过程（含工具调用）" : "展开这段思考过程（含工具调用）";
    };
    function toggle() {
      p.touched = true;             // 人手动拨过：后续自动展开/收起就不再抢方向盘
      p.setOpen(!box.classList.contains("open"));
    }
    // 真正的控件是右边那颗按钮（可 Tab、可回车）；整行点一下也能开关，只是顺手的。
    btn.addEventListener("click", function (e) { e.stopPropagation(); toggle(); });
    head.addEventListener("click", function (e) {
      if (e.target.closest(".seg-toggle")) return;   // 按钮自己处理，别触发两次
      toggle();
    });
    proc = p;
    return p;
  }
  function ensureProc() { return proc || newProc(); }
  function procAppend(p, txt) {
    if (!p.reason) {
      var el = document.createElement("div");
      el.className = "asst";
      p.body.appendChild(el);
      p.reason = { el: el, raw: "" };
    }
    p.reason.raw += esc(txt);
    p.reason.el.innerHTML = md(p.reason.raw);
    p.reason.el.setAttribute("data-chars",
      String(p.reason.raw.replace(/\s+/g, "").length));
    p.syncMeta();
  }
  // ★ 说完话立刻调工具 ⇒ 刚说的那段是过程，不是答案：把它的正文整块搬进过程块。
  //   复用同一个 DOM 节点，不重渲染 —— 已经画好的代码块、复制按钮都还在。
  function adoptSeg() {
    var s = seg;
    if (!s) return;
    seg = null;
    var raw = s.raw;
    var p = ensureProc();
    if (!raw.replace(/\s+/g, "").length) { s.box.remove(); return; }
    s.box.remove();
    s.body.setAttribute("data-chars", String(raw.replace(/\s+/g, "").length));
    p.body.appendChild(s.body);
    p.reason = null;                // 这段已经收尾：后面的推理另起一节
    p.syncMeta();
  }
)AFWEB";

inline constexpr const char* kWebPage5 = R"AFWEB(
  var renamingId = null;

  // ★ 置顶的排前面，其余按时间倒序。
  //   数组本身不重排 —— 只影响显示，免得"置顶"把 localStorage 里的顺序也改乱。
  function sortedChats() {
    return chats.slice().sort(function (a, b) {
      return ((b.pinned ? 1 : 0) - (a.pinned ? 1 : 0)) || (b.ts - a.ts);
    });
  }

  function renderSidebar() {
    var box = $("#chats");
    box.textContent = "";
    sortedChats().forEach(function (c) {
      var el = document.createElement("div");
      el.className = "chat" + (c.id === activeId ? " on" : "");
      el.setAttribute("role", "button");
      el.tabIndex = 0;
      if (c.id === activeId) el.setAttribute("aria-current", "true");
      if (c.id === renamingId) {
        var inp = document.createElement("input");
        inp.className = "rename";
        inp.value = c.title || "";
        inp.addEventListener("click", function (e) { e.stopPropagation(); });
        inp.addEventListener("keydown", function (e) {
          e.stopPropagation();
          if (e.key === "Enter") { commitRename(c, inp.value); }
          else if (e.key === "Escape") { renamingId = null; renderSidebar(); }
        });
        inp.addEventListener("blur", function () { commitRename(c, inp.value); });
        el.appendChild(inp);
        box.appendChild(el);
        setTimeout(function () { inp.focus(); inp.select(); }, 0);
        return;
      }
      if (c.pinned) {
        var pn = document.createElement("span");
        pn.className = "pin"; pn.textContent = "★";
        el.appendChild(pn);
      }
      var t = document.createElement("span");
      t.className = "t"; t.textContent = c.title || "新对话";
      var w = document.createElement("span");
      w.className = "when"; w.textContent = ago(c.ts);
      el.appendChild(t); el.appendChild(w);
      el.addEventListener("click", function () {
        if (activeId !== c.id) {
          activeId = c.id; save(); renderSidebar(); renderStream();
        }
        if (window.matchMedia && window.matchMedia("(max-width:760px)").matches) {
          document.body.classList.add("collapsed");
        }
      });
      el.addEventListener("keydown", function (e) {
        if (e.key === "Enter" || e.key === " ") { e.preventDefault(); el.click(); }
      });
      el.addEventListener("contextmenu", function (e) {
        e.preventDefault();
        openCtx(c.id, e.clientX, e.clientY);
      });
      box.appendChild(el);
    });
    $("#cnt").textContent = String(chats.length);
  }

  function commitRename(c, name) {
    renamingId = null;
    var v = String(name || "").trim();
    if (v) { c.title = v.slice(0, 40); c.ts = Date.now(); }
    save(); renderSidebar();
    if (c.id === activeId) $("#ttl").textContent = c.title || "新对话";
  }

  // ---- 右键菜单（置顶 / 重命名 / 删除）----
  var ctxId = null;
  function openCtx(id, x, y) {
    var c = chatById(id);
    if (!c) return;
    ctxId = id;
    $("#pinLabel").textContent = c.pinned ? "取消置顶" : "置顶";
    var m = $("#ctxmenu");
    m.hidden = false;
    var w = m.offsetWidth, h = m.offsetHeight;
    m.style.left = Math.min(x, window.innerWidth - w - 8) + "px";
    m.style.top  = Math.min(y, window.innerHeight - h - 8) + "px";
  }
  function closeCtx() { $("#ctxmenu").hidden = true; ctxId = null; }
  $("#ctxmenu").addEventListener("click", function (e) {
    e.stopPropagation();
    var it = e.target.closest(".mi");
    if (!it || !ctxId) return;
    var c = chatById(ctxId);
    var act = it.dataset.act;
    closeCtx();
    if (!c) return;
    if (act === "pin") { c.pinned = !c.pinned; save(); renderSidebar(); }
    else if (act === "rename") { renamingId = c.id; renderSidebar(); }
    else if (act === "del") { delChat(c.id); }
  });
  document.addEventListener("click", closeCtx);
  document.addEventListener("contextmenu", function (e) {
    if (!e.target.closest(".chat")) closeCtx();
  });

  // ---------- 底部统计 ----------
  function renderStats() {
    var c = activeChat();
    $("#stTurns").textContent = c ? (c.turns || 0) : 0;
    $("#stSteps").textContent = c ? (c.steps || 0) : 0;
    $("#stTok").textContent = c ? (c.tok || 0) : 0;
  }

  // ---------- 渲染 ----------
  function clearView() {
    wrap.textContent = "";
    seg = null; proc = null; cards = {};
    var e = document.createElement("div");
    e.className = "empty"; e.id = "empty";
    e.innerHTML = "<h1>今天想让 Lumin 做什么？</h1>" +
      "<p>描述一个任务。Lumin 会读取文件、运行工具，并把每一步留在这里。</p>" +
      "<div class=\"keys\"><span><kbd>Enter</kbd> 发送</span><span>·</span>" +
      "<span><kbd>Shift</kbd> + <kbd>Enter</kbd> 换行</span></div>";
    wrap.appendChild(e);
  }
  function ensureNoEmpty() {
    var e = $("#empty");
    if (e && wrap.children.length > 1) e.remove();
  }
  function renderStream() {
    clearView();
    var c = activeChat();
    $("#ttl").textContent = c ? (c.title || "新对话") : "新对话";
    if (c) c.events.forEach(function (ev) { renderEvent(ev, true); });
    endSeg();                          // 回放收尾：还开着的正文就是答案
    if (proc) { proc.setOpen(false); proc = null; }
    ensureNoEmpty();
    renderStats();
    atBottom = true; scroll();
  }
  // ★ 一段模型正文，在流式阶段是"未定性"的：
  //     · 后面还接着工具卡片  → 它是过程（搬进思考块，默认收起）
  //     · 这轮结束它还在最下面 → 它是答案（平铺、正常字号）
  //   所以先按答案的样子铺开，等看清后面跟着什么再定性（adoptSeg / endSeg）。
  //   为什么不在服务端判：流式的时候谁也看不见未来 —— 只能靠"后面出现了什么"来定。
  //   好处是实时和历史回放走的是同一套判定，不会两副样子。
  function newSeg() {
    var box = document.createElement("div");
    box.className = "seg";
    var mark = document.createElement("span");
    mark.className = "ai-mark";
    mark.setAttribute("aria-hidden", "true");
    box.appendChild(mark);
    var body = document.createElement("div");
    body.className = "asst";
    box.appendChild(body);
    wrap.appendChild(box);
    ensureNoEmpty();
    seg = { box: box, body: body, raw: "" };
    return seg;
  }
  function segAppend(s, txt) {
    s.raw += esc(txt);
    s.body.innerHTML = md(s.raw);     // 整段重渲染：delta 已被 16ms 合并过，几十 KB 答案也不卡
  }
  // 定性：这是答案 —— 留着平铺；只有空正文（纯空白）才把空壳删掉
  function endSeg() {
    var s = seg;
    if (!s) return;
    seg = null;
    if (!s.raw.replace(/\s+/g, "").length) s.box.remove();
  }
  function addCard(id, name) {
    var p = ensureProc();
    var d = document.createElement("div");
    d.className = "tool";
    d.innerHTML =
      '<div class="tool-head"><span class="tool-name"></span>' +
      '<span class="tool-state"><span class="spin" hidden></span><span class="txt"></span></span>' +
      '<span class="tool-ms">0.0s</span></div>' +
      '<div class="tool-args"></div><pre class="tool-out"></pre>';
    d.querySelector(".tool-name").textContent = esc(name || "tool");
    p.body.appendChild(d);            // ★ 只挂在过程块里：收起思考 = 工具卡片一起收起
    p.reason = null;
    p.tools += 1;
    p.syncMeta();
    cards[id] = d;
    ensureNoEmpty();
    return d;
  }
  function setState(card, txt, cls) {
    card.classList.toggle("run", cls === "run");
    card.querySelector(".tool-state .txt").textContent = txt;
    card.querySelector(".tool-state .spin").hidden = cls !== "run";
  }
  // ★ 服务重启/任务中断时，历史里可能留着只有 toolStart、没有 toolDone 的卡片。
  //   以前它会永远显示“执行中 0.0s”，看起来像 Agent 还在思考；其实早就没进程了。
  //   收到“本轮结束/服务空闲”时就把它标成已中断，避免假死。
  function markStaleTools(reason) {
    var running = wrap.querySelectorAll(".tool.run");
    for (var i = 0; i < running.length; i++) {
      var c = running[i];
      c.classList.remove("run");
      c.classList.add("err");
      setState(c, "已中断", "");
      var ms = c.querySelector(".tool-ms");
      if (ms) ms.textContent = "—";
      var out = c.querySelector(".tool-out");
      if (out && !out.textContent) out.textContent = reason;
    }
  }
  function addLine(cls, text) {
    var d = document.createElement("div");
    d.className = cls;
    d.textContent = esc(text);
    (proc ? proc.body : wrap).appendChild(d);   // 过程里的提示跟着过程块一起收起
    ensureNoEmpty();
  }

  // ★ 唯一的事件渲染入口：live 与"切回历史对话"走的是同一条路。
  //   两套渲染代码的下场一定是"历史里看着好好的，实时跑起来不一样"。
  function renderEvent(m, replayMode) {
    switch (m.t) {
      case "user": {
        var d = document.createElement("div");
        d.className = "row user";
        var b = document.createElement("div");
        b.className = "bubble"; b.textContent = esc(m.s);
        d.appendChild(b); wrap.appendChild(d);
        // 上一轮就停在这：正文收成答案，过程块留在原地（收起），这一轮重新开块。
        endSeg();
        if (proc) proc.setOpen(false);     // 没收到"本轮结束"就接着问：上一条过程也收起来
        proc = null;
        break;
      }
      case "text":
        // 已经跑过工具 ⇒ 现在这段正文就是答案：把过程块收起，让它占住视线。
        // （还没跑工具时的正文可能是"我先看看文件"这类过程话，先别急着收）
        if (proc && proc.tools && !proc.touched) proc.setOpen(false);
        if (!seg) newSeg();
        segAppend(seg, m.s);
        break;
      case "think":
        // ★ 正文已经定成答案后，迟到的 think 不能把答案**改判**回过程块
        //   （重试/第二轮模型调用可能把 reasoning 排在正文后面）。先收住答案，
        //   再另起一个过程块；否则用户会看到"答案被折叠块吞掉"。
        if (seg) {
          endSeg();
          if (proc) proc.setOpen(false);   // 上一块收起来，新的过程另起一行
          proc = null;
        }
        var p1 = ensureProc();
        if (!p1.touched) p1.setOpen(true);   // 还在生成时展开，能看见它在想
        procAppend(p1, m.s);
        break;
      case "toolCall": {
        adoptSeg();                          // 说完就调工具 ⇒ 刚才那段话是过程
        var p2 = ensureProc();
        var c1 = addCard(m.id, m.name);
        setState(c1, "已请求", "");
        if (!p2.touched) p2.setOpen(true);   // 工具在跑，过程展开给人看
        break;
      }
      case "toolArgs": {
        var c2 = cards[m.id] || addCard(m.id, m.name || "tool");
        c2.querySelector(".tool-args").textContent = esc(m.s);
        break;
      }
      case "toolStart": {
        var c3 = cards[m.id] || addCard(m.id, m.name);
        setState(c3, "执行中", "run");
        break;
      }
      case "tick": {
        var c4 = cards[m.id];
        if (c4) c4.querySelector(".tool-ms").textContent = (m.ms / 1000).toFixed(1) + "s";
        break;
      }
      case "toolDone": {
        var c5 = cards[m.id] || addCard(m.id, m.name || "tool");
        c5.classList.remove("run");
        if (m.ok === false) c5.classList.add("err");
        setState(c5, m.ok === false ? "失败" : "完成", "");
        c5.querySelector(".tool-ms").textContent = ((m.ms || 0) / 1000).toFixed(1) + "s";
        c5.querySelector(".tool-out").textContent = esc(m.s);
        break;
      }
      case "notice": {
        var note = String(m.s);
        // "本轮结束" 已经由底部状态条表达；正文里再刷一条只会抢答案的注意力。
        if (note.indexOf("本轮结束") < 0) addLine("notice", m.s);
        // 步数藏在"第 N 步：执行了 M 个工具"里 —— 底部统计条要的就是这个 N
        var sm = note.match(/第\s*(\d+)\s*步/);
        if (sm) { var cc = activeChat(); if (cc) { cc.steps = Number(sm[1]); renderStats(); save(); } }
        // ★ "done" 是**一次模型回复**的结束，不是**整轮任务**的结束 ——
        //   一轮里模型可能被调用好几次（每次工具结果之后都会再来一次）。
        if (note.indexOf("本轮结束") >= 0) {
          // 轮次结束：最后那段正文就是答案（平铺）；过程块收起，只留一行摘要
          endSeg();
          if (proc) { proc.reason = null; proc.setOpen(false); proc = null; }
          markStaleTools("本轮已结束，但这条工具没有返回结果（可能被中断）");
          busy = false; sendBtn.disabled = false;
          $("#hint").textContent = "本轮结束";
        }
        break;
      }
      case "error": {
        // ★ 报错不能跟着过程块一起收起 —— 收起来了用户就只看到"思考过程"一行，
        //   不知道这轮其实挂了。所以先收过程，再把错误行长在过程块外面。
        endSeg();
        if (proc) { proc.reason = null; proc.setOpen(false); proc = null; }
        var de = document.createElement("div");
        de.className = "error";
        de.textContent = esc(m.s);
        wrap.appendChild(de);
        ensureNoEmpty();
        break;
      }
      case "usage": {
        var cu = activeChat();
        if (cu) { cu.tok = Number(m.n) || 0; renderStats(); save(); }
        break;
      }
      case "done": if (!replayMode) $("#hint").textContent = "Lumin 继续中…"; break;
      case "perm": showPerm(m); break;
      case "permEnd": if (perm && perm.id === m.id) hidePerm(); break;
    }
    scroll();
  }

)AFWEB";

inline constexpr const char* kWebPage5B = R"AFWEB(
  // 事件入库 + 渲染。tick/done/perm 这类"过程噪声"不存 ——
  // 存了之后切回来会看到一堆卡在 3.2s 的旧计时器。
  // ★ 事件该进哪个对话：看事件**自带**的 cid，而不是"此刻选中的是哪个"。
  //   以前按选中对话入库 —— 跑着的时候切一下对话，答案就写进了别人的记录，
  //   开着两个页面时更明显（两个页面各按各的选中对话存同一串事件）。
  function chatForEvent(m) {
    if (!m || !m.cid) return activeChat() || newChat();
    var c = chatById(m.cid);
    if (!c) {
      // 另一个标签页发起的任务、或本地已删掉的对话：按服务的 cid 建一个，
      // 让事件有地方落，而不是盖到当前对话上。
      c = { id: m.cid, title: "新对话", ts: Date.now(), events: [], turns: 0,
            steps: 0, tok: 0, pinned: false, lastSeq: 0, epoch: null };
      chats.unshift(c);
      save(); renderSidebar();
    }
    return c;
  }
  function pushEvent(m) {
    var c = chatForEvent(m);
    var mine = (c.id === activeId);
    // ★ 记下"看到第几条"：掉线复连后服务端会把整段缓冲重发一遍，
    //   靠这个数把已经渲染过的事件滤掉，不会重复，也不会漏。
    if (m && m.seq) c.lastSeq = Math.max(c.lastSeq || 0, m.seq);
    if (["tick", "done", "perm", "permEnd", "replay", "hello", "state", "busy"].indexOf(m.t) < 0) {
      c.events.push(m);
      if (m.t === "user") {
        c.title = String(m.s).replace(/\s+/g, " ").slice(0, 22) || "新对话";
        c.ts = Date.now();
        c.turns = (c.turns || 0) + 1;
        c.steps = 0;
        renderSidebar();
        if (mine) { $("#ttl").textContent = c.title; }
      }
      save();
    }
    // ★ 只画"当前正在看的那一个"；别的对话的事件先存着，切过去由 renderStream()
    //   按存储重画 —— 否则会画到别人脸上。
    if (mine) { renderEvent(m, false); renderStats(); }
  }

  function showPerm(m) {
    if (permMode === "auto") {
      send({ t: "permAnswer", id: m.id, allow: true });
      addLine("notice", "已自动允许工具调用（权限模式：自动允许）");
      return;
    }
    perm = { id: m.id };
    $("#modal-prompt").textContent = m.prompt || "";
    $("#modal").hidden = false;
    $("#allow").focus();
    atBottom = true;
  }
  function hidePerm() { perm = null; $("#modal").hidden = true; }
  function answer(allow) {
    if (!perm) return;
    send({ t: "permAnswer", id: perm.id, allow: !!allow });
    $("#modal-prompt").textContent = allow ? "已允许，等待 Lumin 继续…" : "已拒绝，等待 Lumin 处理…";
    $("#allow").disabled = $("#deny").disabled = true;
    setTimeout(function () { $("#allow").disabled = $("#deny").disabled = false; hidePerm(); }, 350);
  }
  $("#allow").addEventListener("click", function () { answer(true); });
  $("#deny").addEventListener("click", function () { answer(false); });

  function send(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }

  // ---------- 附件 ----------
  function humanSize(n) { return n < 1024 ? n + " B" : (n / 1024).toFixed(1) + " KB"; }
  function addFiles(list) {
    var files = Array.prototype.slice.call(list || []);
    if (!files.length) return;
    var pending = files.length;
    files.forEach(function (f) {
      var fr = new FileReader();
      fr.onload = function () {
        var txt = String(fr.result || "");
        if (txt.indexOf("\u0000") >= 0) {
          addLine("notice", "跳过二进制文件 " + f.name + "（这一版只带文本）");
        } else if (f.size > FILE_MAX) {
          addLine("notice", "跳过 " + f.name + "（" + humanSize(f.size) + " 超过 200 KB）");
        } else {
          ATTACHED.push({ name: f.name, text: txt });
          renderFiles();
          $("#hint").textContent = "已附加 " + ATTACHED.length + " 个文件";
        }
        if (--pending === 0) { renderStats(); scroll(); }
      };
      fr.onerror = function () { if (--pending === 0) {} };
      fr.readAsText(f);
    });
  }
  function renderFiles() {
    var box = $("#files");
    box.textContent = "";
    ATTACHED.forEach(function (a, i) {
      var d = document.createElement("span");
      d.className = "file";
      var b = document.createElement("b"); b.textContent = a.name;
      var s = document.createElement("span"); s.textContent = humanSize(a.text.length);
      var x = document.createElement("button"); x.textContent = "×"; x.title = "移除";
      x.addEventListener("click", function () { ATTACHED.splice(i, 1); renderFiles(); renderStats(); });
      d.appendChild(b); d.appendChild(s); d.appendChild(x);
      box.appendChild(d);
    });
  }
  function attachedText() {
    if (!ATTACHED.length) return "";
    var out = "";
    ATTACHED.forEach(function (a) {
      out += "\n\n【附件 " + a.name + "】\n```\n" + a.text.replace(/```/g, "'''") + "\n```";
    });
    return out;
  }
)AFWEB";

inline constexpr const char* kWebPage6 = R"AFWEB(
  function submit() {
    var text = input.value.trim();
    if (!text && !ATTACHED.length) return;
    // ★ 正在跑也允许提交：服务端是 FIFO 队列，会排队而不是覆盖上一条。
    var queued = busy;
    // ★ 正好断线时按回车：send() 会静默丢掉这条消息，而 busy 已经被置真 ——
    //   页面就永远卡在"正在跑"，只能 F5。所以先确认连接真的在，
    //   再清输入框、再置 busy。
    if (!ws || ws.readyState !== 1) {
      $("#hint").textContent = "还没连上服务端，这条没发出去（等右上角变绿再发一次）";
      return;
    }
    var c = activeChat() || newChat();
    var shown = text || ("（只发了 " + ATTACHED.length + " 个附件）");
    var full = (text + attachedText()).trim();
    input.value = ""; autoGrow();
    ATTACHED = []; renderFiles();
    busy = true;
    $("#hint").textContent = queued
      ? "已排队：当前任务跑完就轮到这条"
      : "Lumin 正在跑…  关掉页面不会中断它";
    // ★ 不在这里画用户气泡：交给服务端回显（它会把这条进回放缓冲），
    //   否则刷新页面/开第二个标签页时，"提问"就没了，只剩答案。
    send({ t: "task", s: full, d: shown, cid: c.id });
    atBottom = true; scroll();
  }
  sendBtn.addEventListener("click", submit);
  input.addEventListener("keydown", function (e) {
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); submit(); }
  });
  function autoGrow() {
    input.style.height = "auto";
    input.style.height = Math.min(170, input.scrollHeight) + "px";
  }
  input.addEventListener("input", autoGrow);

  // 选文件 / 拖拽
  $("#plus").addEventListener("click", function () { $("#file").click(); });
  $("#file").addEventListener("change", function (e) { addFiles(e.target.files); e.target.value = ""; });
  var composer = $("#composer");
  ["dragenter", "dragover"].forEach(function (k) {
    composer.addEventListener(k, function (e) {
      e.preventDefault(); e.stopPropagation(); composer.classList.add("drag");
    });
  });
  ["dragleave", "drop"].forEach(function (k) {
    composer.addEventListener(k, function (e) {
      e.preventDefault(); e.stopPropagation();
      if (k === "dragleave" && e.relatedTarget && composer.contains(e.relatedTarget)) return;
      composer.classList.remove("drag");
    });
  });
  composer.addEventListener("drop", function (e) {
    if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length) addFiles(e.dataTransfer.files);
  });

  // ---------- 侧栏 / 权限下拉 / 设置 ----------
  $("#newchat").addEventListener("click", newChat);
  $("#collapse").addEventListener("click", function () { document.body.classList.add("collapsed"); });
  $("#expand").addEventListener("click", function () { document.body.classList.remove("collapsed"); });

  function setMode(v) {
    permMode = v;
    $("#modeLabel").textContent = v === "auto" ? "自动允许" : "每次询问权限";
    $("#modeBtn").classList.toggle("warn", v === "auto");
    var items = $("#modeMenu").querySelectorAll(".mi");
    for (var i = 0; i < items.length; i++) items[i].classList.toggle("on", items[i].dataset.v === v);
    save();
  }
  function closeModeMenu() {
    $("#modeMenu").hidden = true;
    $("#modeBtn").setAttribute("aria-expanded", "false");
  }
  $("#modeBtn").addEventListener("click", function (e) {
    e.stopPropagation();
    var open = $("#modeMenu").hidden;
    $("#modeMenu").hidden = !open;
    $("#modeBtn").setAttribute("aria-expanded", String(open));
  });
  $("#modeMenu").addEventListener("click", function (e) {
    var it = e.target.closest(".mi");
    if (!it) return;
    setMode(it.dataset.v);
    closeModeMenu();
  });
  document.addEventListener("click", closeModeMenu);

  // 压缩上下文：只发一条请求，进度和结果都由服务端用 notice 推回来。
  $("#compact").addEventListener("click", function () {
    if (!ws || ws.readyState !== 1) { addLine("notice", "还没连上服务端，压缩请求没发出去"); return; }
    send({ t: "compact", cid: activeId });
  });

  $("#optset").addEventListener("click", function () {
    $("#skey").value = settings.apiKey || "";
    $("#setmodal").hidden = false;
    $("#skey").focus();
  });
  $("#scancel").addEventListener("click", function () { $("#setmodal").hidden = true; });
  $("#ssave").addEventListener("click", function () {
    settings.apiKey = $("#skey").value.trim();
    save();
    send({ t: "config", apiKey: settings.apiKey, baseUrl: BASE, model: MODEL });
    $("#setmodal").hidden = true;
    $("#hint").textContent = "设置已保存 · 下一条消息起生效";
  });
  $("#skey").addEventListener("keydown", function (e) {
    if (e.key === "Enter") { e.preventDefault(); $("#ssave").click(); }
  });

  // ---------- WebSocket ----------
  var ws = null;
  function setConn(on) {
    $("#dot").classList.toggle("on", on);
    $("#connpill").classList.toggle("on", on);
    $("#conn").textContent = on ? "已连接" : "已断开";
  }
  function connect() {
    ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
    ws.onopen = function () { setConn(true); };
    ws.onclose = function () { setConn(false); setTimeout(connect, 1200); };
    ws.onerror = function () {};
    ws.onmessage = function (ev) {
      var m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.t === "hello") {
        setConn(true);
        if (m.port) $("#whoTxt").textContent = "本地 · 127.0.0.1:" + m.port;
        // ★ 第一次进来（本机还没存过 Key）直接把设置弹出来 ——
        //   用户不必先知道"左下角有个设置"，终端也不必再帮他填。
        //   取消也放行：他还能先看看界面，之后左下角「设置」随时能再开。
        if (!settings.apiKey) {
          $("#hint").textContent = "先填 API Key 才能对话，浏览器会记住";
          $("#optset").click();
        }
        return;
      }
      if (m.t === "replay") {
        // ★ 服务端的回放缓冲现在是**多对话混排**的（不再按 cid 清空，否则 B 提交
        //   一条任务会把正在跑的 A 的历史清掉）。所以这里一条条按各自的 cid 分拣，
        //   seq 也按**每个对话**分别去重 —— 混着一个计数器去重，会把别人的
        //   第 300 条当成"我见过的"直接丢掉。
        var evs = m.e || [];
        for (var i = 0; i < evs.length; i++) {
          var ev = evs[i];
          if (!ev || !ev.seq) continue;
          var rc = ev.cid ? chatById(ev.cid) : activeChat();
          if (!rc) continue;                     // 本地没有这个对话：别凭空造一个
          var base = (rc.epoch && rc.epoch === m.epoch) ? (rc.lastSeq || 0) : 0;
          if (ev.seq <= base) continue;
          rc.epoch = m.epoch || rc.epoch || null;
          pushEvent(ev);
        }
        save();
        return;
      }
      if (m.t === "state") {
        // ★ 服务端说了算：这轮还在不在跑。掉线错过"本轮结束"时靠它解锁。
        busy = !!m.busy;
        sendBtn.disabled = false;          // 忙也能提交：服务端排队，不覆盖
        if (!busy) markStaleTools("服务已空闲：这条工具在重启/断线时被中断了");
        if (busy) {
          $("#hint").textContent = "Lumin 正在跑…（可以继续提交，会排队）";
        } else if (String($("#hint").textContent).indexOf("Lumin 正在跑") >= 0) {
          $("#hint").textContent = "本轮结束";
        }
        return;
      }
      pushEvent(m);
    };
  }
  window.addEventListener("beforeunload", function () { try { save(); } catch (e) {} });
  setMode(permMode);
  if (window.matchMedia && window.matchMedia("(max-width:760px)").matches) {
    document.body.classList.add("collapsed");
  }
  renderSidebar();
  if (!chats.length) newChat(); else { renderStream(); }
  setInterval(function () {
    $("#clock").textContent = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  }, 1000);
  connect();
  input.focus({preventScroll:true});
})();
</script>
</body>
</html>
)AFWEB";

// ★ 为什么拆成多段再拼：MSVC 对**单个**字符串字面量有 16384 字节上限
//   （C2026: 字符串太大，已截断尾部字符）。整页 HTML 远超这个数，
//   拆开再在运行时拼起来即可 —— 交付物仍然只有一个 exe，不依赖磁盘文件。
inline const std::string& webPage() {
    static const std::string s =
        std::string(kWebPage1) + kWebPage1C + kWebPage1B + kWebPage3 + kWebPage4 +
        kWebPage4B + kWebPage5 + kWebPage5B + kWebPage6;
    return s;
}

}  // namespace af::ui


