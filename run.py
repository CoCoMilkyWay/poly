#!/usr/bin/env python3
import subprocess
import webbrowser
import time
import sys
import signal
from pathlib import Path
import socket

ROOT = Path(__file__).parent
BACKEND_DIR = ROOT / "core-backend"
BACKEND_BUILD = BACKEND_DIR / "projects" / "core" / "build"
FRONTEND_DIR = ROOT / "core-frontend"
CONFIG_FILE = ROOT / "config.json"

# Windows 下的可执行文件名
BACKEND_EXE = BACKEND_BUILD / \
    ("core.exe" if sys.platform == "win32" else "core")


def build_backend():
    """编译 C++ backend (cmake 增量编译)"""
    print("[run.py] 编译 C++ backend...")
    BACKEND_BUILD.mkdir(parents=True, exist_ok=True)

    # cmake 配置
    result = subprocess.run([
        "cmake", "..",
        "-DCMAKE_C_COMPILER=clang",
        "-DCMAKE_CXX_COMPILER=clang++"
    ], cwd=BACKEND_BUILD)
    assert result.returncode == 0, "cmake 配置失败"

    # cmake 编译
    result = subprocess.run(
        ["cmake", "--build", ".", "--config", "Release"], cwd=BACKEND_BUILD)
    assert result.returncode == 0, "编译失败"

    print("[run.py] 编译完成")


def check_config():
    """检查配置文件"""
    assert CONFIG_FILE.exists(), f"配置文件 {CONFIG_FILE} 不存在, 请创建并填入 API_KEY"

    import json
    with open(CONFIG_FILE) as f:
        config = json.load(f)

    if config.get("api_key") == "YOUR_THE_GRAPH_API_KEY":
        print("[run.py] 警告: 请在 config.json 中填入有效的 The Graph API Key")
        print("[run.py] 获取 API Key: https://thegraph.com/studio/apikeys/")


def wait_for_port(host: str, port: int, timeout: int = 30):
    """等待端口就绪"""
    start_time = time.time()
    while time.time() - start_time < timeout:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(1)
            sock.connect((host, port))
            sock.close()
            return True
        except (socket.timeout, ConnectionRefusedError, OSError):
            time.sleep(0.2)
    return False


def main():
    processes = []

    def cleanup(signum=None, frame=None):
        print("\n[run.py] 正在关闭...")
        # 逐个关闭: 先 frontend 再 backend, 每个等退出后再关下一个
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
            p.wait()
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    # 1. 检查配置
    check_config()

    # 1.5 确保 data 目录存在
    (ROOT / "data").mkdir(exist_ok=True)

    # 2. 编译 C++ backend (cmake 自带增量编译)
    build_backend()

    # 3. 启动 C++ backend(后台)
    print("[run.py] 启动 backend...")
    backend_proc = subprocess.Popen(
        [str(BACKEND_EXE), "--config", str(CONFIG_FILE)],
        cwd=ROOT,
        start_new_session=True,
    )
    processes.append(backend_proc)

    # 3.5 等待 backend API 就绪
    print("[run.py] 等待 backend API 就绪...")
    assert wait_for_port("127.0.0.1", 8001), "backend API 启动超时"
    print("[run.py] backend API 已就绪")

    # 4. 启动 Python frontend(后台)
    print("[run.py] 启动 frontend...")
    frontend_proc = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "main:app", "--host",
            "0.0.0.0", "--port", "8000", "--log-level", "warning"],
        cwd=FRONTEND_DIR,
        start_new_session=True,
    )
    processes.append(frontend_proc)

    # 5. 等待服务就绪, 打开浏览器
    time.sleep(2)
    url = "http://localhost:8000"
    print(f"[run.py] 打开浏览器: {url}")
    webbrowser.open(url)

    # 6. 等待进程退出
    print("[run.py] 服务已启动, 按 Ctrl+C 退出")
    try:
        # 等待任一进程退出
        while True:
            for p in processes:
                if p.poll() is not None:
                    print(f"[run.py] 进程 {p.pid} 已退出, 退出码: {p.returncode}")
                    cleanup()
            time.sleep(1)
    except KeyboardInterrupt:
        cleanup()


if __name__ == "__main__":
    main()
