AgentForge · 浏览器版
=====================

启动方式
--------
双击 run-web.bat，或在 PowerShell 里：

    .\run-web.ps1

浏览器会自动打开 http://127.0.0.1:8080 ，在输入框里写任务即可。
关掉窗口（或 Ctrl+C）即停止服务。

第一次启动会问你要 API Key
---------------------------
支持任何 OpenAI 兼容端点。默认接 DeepSeek：

    https://api.deepseek.com/v1    模型 deepseek-chat

Key 用 Windows DPAPI 加密后存在本目录的 key.dat，只有当前 Windows 用户能解开。
想换 Key：删掉 key.dat 再启动。想换模型：设置环境变量 AF_MODEL / OPENAI_BASE_URL。

文件都在哪
----------
agent.exe          程序本体（无外部依赖）
run-web.ps1        启动脚本
run-web.bat        双击入口
workspace\web\     ★ Agent 的所有产出都写在这里
key.dat            加密后的 API Key（首次启动后生成）

卸载
----
直接删掉整个文件夹即可，不写注册表、不改 PATH。
