#!/usr/bin/env python3
import subprocess
import webbrowser
import time
import sys
import signal
import socket
from pathlib import Path

ROOT = Path(__file__).parent
BACKEND_DIR = ROOT / "core-backend"
BACKEND_BUILD = BACKEND_DIR / "projects" / "core" / "build"
FRONTEND_DIR = ROOT / "core-frontend"
CONFIG_FILE = ROOT / "config.json"

BACKEND_EXE = BACKEND_BUILD / ("core.exe" if sys.platform == "win32" else "core")


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


def wait_for_port(host: str, port: int, timeout: int = 30):
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
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
            p.wait()
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    assert CONFIG_FILE.exists(), f"配置文件 {CONFIG_FILE} 不存在"
    (ROOT / "data").mkdir(exist_ok=True)

    build_backend()

    print("[run.py] 启动 backend...")
    backend_proc = subprocess.Popen(
        [str(BACKEND_EXE), "--config", str(CONFIG_FILE)],
        cwd=ROOT,
        start_new_session=True,
    )
    processes.append(backend_proc)

    print("[run.py] 等待 backend API 就绪...")
    assert wait_for_port("127.0.0.1", 8001), "backend API 启动超时"
    print("[run.py] backend API 已就绪")

    print("[run.py] 启动 frontend...")
    frontend_proc = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "main:app", "--host",
            "0.0.0.0", "--port", "8000", "--log-level", "warning"],
        cwd=FRONTEND_DIR,
        start_new_session=True,
    )
    processes.append(frontend_proc)

    time.sleep(2)
    url = "http://localhost:8000"
    print(f"[run.py] 打开浏览器: {url}")
    webbrowser.open(url)

    print("[run.py] 服务已启动, 按 Ctrl+C 退出")
    try:
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
