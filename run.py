#!/usr/bin/env python3
import subprocess
import webbrowser
import time
import sys
import signal
import socket
import os
import atexit
from pathlib import Path

ROOT = Path(__file__).parent
BACKEND_DIR = ROOT / "core-backend"
BACKEND_BUILD = BACKEND_DIR / "projects" / "core" / "build"
FRONTEND_DIR = ROOT / "core-frontend"
CONFIG_FILE = ROOT / "config.json"

BACKEND_EXE = BACKEND_BUILD / ("core.exe" if sys.platform == "win32" else "core")
BACKEND_PORT = 8001
FRONTEND_PORT = 8000

processes = []
cleaning_up = False


def kill_port(port: int):
    """杀掉占用指定端口的进程"""
    try:
        result = subprocess.run(
            ["lsof", "-ti", f":{port}"],
            capture_output=True, text=True
        )
        if result.stdout.strip():
            pids = result.stdout.strip().split('\n')
            for pid in pids:
                try:
                    os.kill(int(pid), signal.SIGKILL)
                    print(f"[run.py] 杀掉端口 {port} 的进程 {pid}")
                except (ProcessLookupError, ValueError):
                    pass
            time.sleep(0.5)
    except FileNotFoundError:
        pass


def kill_by_pattern(pattern: str):
    """杀掉匹配模式的进程"""
    try:
        subprocess.run(["pkill", "-9", "-f", pattern], capture_output=True)
    except FileNotFoundError:
        pass


def cleanup_stale():
    """清理残留进程"""
    kill_port(BACKEND_PORT)
    kill_port(FRONTEND_PORT)
    kill_by_pattern("core-backend/projects/core/build/core")
    kill_by_pattern("uvicorn main:app.*8000")
    time.sleep(0.5)


def terminate_process(p, timeout=3):
    """优雅终止进程，超时后强杀"""
    if p.poll() is not None:
        return
    
    try:
        pgid = os.getpgid(p.pid)
        os.killpg(pgid, signal.SIGTERM)
    except (ProcessLookupError, OSError):
        try:
            p.terminate()
        except:
            pass
    
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            pgid = os.getpgid(p.pid)
            os.killpg(pgid, signal.SIGKILL)
        except (ProcessLookupError, OSError):
            try:
                p.kill()
            except:
                pass
        p.wait()


def cleanup(signum=None, frame=None):
    """清理所有子进程"""
    global cleaning_up
    if cleaning_up:
        return
    cleaning_up = True
    
    print("\n[run.py] 正在关闭...")
    
    for p in reversed(processes):
        terminate_process(p)
    
    cleanup_stale()
    print("[run.py] 已退出")
    sys.exit(0)


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
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)
    atexit.register(cleanup)

    assert CONFIG_FILE.exists(), f"配置文件 {CONFIG_FILE} 不存在"
    (ROOT / "data").mkdir(exist_ok=True)

    print("[run.py] 检查残留进程...")
    cleanup_stale()

    build_backend()

    print("[run.py] 启动 backend...")
    backend_proc = subprocess.Popen(
        [str(BACKEND_EXE), "--config", str(CONFIG_FILE)],
        cwd=ROOT,
        start_new_session=True,
    )
    processes.append(backend_proc)

    print("[run.py] 等待 backend API 就绪...")
    if not wait_for_port("127.0.0.1", BACKEND_PORT, timeout=10):
        if backend_proc.poll() is not None:
            print(f"[run.py] backend 启动失败, 退出码: {backend_proc.returncode}")
        else:
            print("[run.py] backend API 启动超时")
        cleanup()
        return

    print("[run.py] backend API 已就绪")

    print("[run.py] 启动 frontend...")
    frontend_proc = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "main:app", "--host",
            "0.0.0.0", "--port", str(FRONTEND_PORT), "--log-level", "warning"],
        cwd=FRONTEND_DIR,
        start_new_session=True,
    )
    processes.append(frontend_proc)

    if not wait_for_port("127.0.0.1", FRONTEND_PORT, timeout=10):
        print("[run.py] frontend 启动超时")
        cleanup()
        return

    url = f"http://localhost:{FRONTEND_PORT}"
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
