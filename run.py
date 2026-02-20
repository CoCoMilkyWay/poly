#!/usr/bin/env python3
import subprocess
import webbrowser
import time
import sys
import signal
import socket
import os
from pathlib import Path

ROOT = Path(__file__).parent
BACKEND_DIR = ROOT / "core-backend"
BACKEND_BUILD = BACKEND_DIR / "projects" / "core" / "build"
FRONTEND_DIR = ROOT / "core-frontend"
CONFIG_FILE = ROOT / "config.json"

BACKEND_EXE = BACKEND_BUILD / \
    ("core.exe" if sys.platform == "win32" else "core")
BACKEND_PORT = 8001
FRONTEND_PORT = 8000


def port_in_use(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


def build_backend():
    print("[run.py] 编译 C++ backend...")
    BACKEND_BUILD.mkdir(parents=True, exist_ok=True)

    result = subprocess.run([
        "cmake", "..",
        "-DCMAKE_C_COMPILER=clang",
        "-DCMAKE_CXX_COMPILER=clang++"
    ], cwd=BACKEND_BUILD)
    assert result.returncode == 0, "cmake 配置失败"

    result = subprocess.run(
        ["cmake", "--build", ".", "--config", "Release"], cwd=BACKEND_BUILD)
    assert result.returncode == 0, "编译失败"

    print("[run.py] 编译完成")


def wait_for_port(port: int, timeout: int = 10):
    start = time.time()
    while time.time() - start < timeout:
        if port_in_use(port):
            return True
        time.sleep(0.2)
    return False


def main():
    assert CONFIG_FILE.exists(), f"配置文件 {CONFIG_FILE} 不存在"
    assert not port_in_use(BACKEND_PORT), f"端口 {BACKEND_PORT} 已被占用"
    assert not port_in_use(FRONTEND_PORT), f"端口 {FRONTEND_PORT} 已被占用"

    (ROOT / "data").mkdir(exist_ok=True)
    build_backend()

    print("[run.py] 启动 backend...")
    backend = subprocess.Popen(
        [str(BACKEND_EXE), "--config", str(CONFIG_FILE)],
        cwd=ROOT,
    )

    print("[run.py] 启动 frontend...")
    frontend = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "main:app", "--host",
            "0.0.0.0", "--port", str(FRONTEND_PORT), "--log-level", "warning"],
        cwd=FRONTEND_DIR,
    )

    try:
        assert wait_for_port(BACKEND_PORT), "backend 启动失败"
        assert wait_for_port(FRONTEND_PORT), "frontend 启动失败"

        url = f"http://localhost:{FRONTEND_PORT}"
        print(f"[run.py] 服务已启动: {url}")
        webbrowser.open(url)

        backend.wait()
    except KeyboardInterrupt:
        pass
    finally:
        print("[run.py] 正在关闭...")
        for proc in [backend, frontend]:
            if proc.poll() is None:
                proc.terminate()
        for proc in [backend, frontend]:
            proc.wait(timeout=10)
        print("[run.py] 已退出")


if __name__ == "__main__":
    main()
