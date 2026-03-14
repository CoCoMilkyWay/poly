#!/usr/bin/env python3
import os
import subprocess
import time
import sys
import socket
import shutil
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).parent
BACKEND_DIR = ROOT / "core-backend"
BACKEND_BUILD = BACKEND_DIR / "projects" / "core" / "build"
FRONTEND_DIR = ROOT / "core-frontend"
CONFIG_FILE = ROOT / "config.json"

BACKEND_EXE = BACKEND_BUILD / "core"
BACKEND_PORT = 8001
FRONTEND_PORT = 8000
BACKEND_STARTUP_TIMEOUT = 600
FRONTEND_STARTUP_TIMEOUT = 600
GRACEFUL_SHUTDOWN_TIMEOUT = 600

# Build modes (set ONLY ONE to True)
ENABLE_PROFILE = True
ENABLE_PRODUCTION = False

LINUX_REQUIRED_TOOLS = [
    "cmake",
    "clang",
    "clang++",
    "ninja",
]

LINUX_REQUIRED_PACKAGES = [
    "build-essential",
    "libssl-dev",
    "libprotobuf-dev",
    "protobuf-compiler",
    "ninja-build",
]


def find_tracy_profiler() -> Optional[Path]:
    override = os.environ.get(
        "TRACY_PROFILER_EXE") or os.environ.get("TRACY_PROFILER_PATH")
    if override:
        p = Path(override)
        if p.is_file():
            return p

    candidates = [
        BACKEND_DIR / "packages" / "tracy" / "tracy-profiler",
        Path.home() / ".local" / "bin" / "tracy-profiler",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def launch_tracy_ui() -> Optional[subprocess.Popen]:
    tracy_exe = find_tracy_profiler()
    if not tracy_exe:
        print("[Tracy] tracy-profiler not found (skipping UI auto-launch)")
        return None

    try:
        proc = subprocess.Popen(
            [str(tracy_exe), "-a", "localhost"],
            cwd=tracy_exe.parent,
            start_new_session=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        print(f"[Tracy] UI launched: {tracy_exe}")
        return proc
    except Exception as e:
        print(f"[Tracy] Failed to launch: {e}")
        return None


def port_in_use(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


def dpkg_package_installed(pkg: str) -> bool:
    result = subprocess.run(
        ["dpkg-query", "-W", "-f=${Status}", pkg],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0 and "install ok installed" in result.stdout


def assert_linux_dependencies():
    assert sys.platform == "linux", "[run.py] 仅支持 Linux"

    missing_tools = any(shutil.which(t) is None for t in LINUX_REQUIRED_TOOLS)
    missing_packages = any(not dpkg_package_installed(p)
                           for p in LINUX_REQUIRED_PACKAGES)
    assert not (missing_tools or missing_packages), (
        "[run.py] Linux 依赖未安装完整\n"
        "sudo apt update && sudo apt install -y "
        + " ".join(LINUX_REQUIRED_PACKAGES)
    )


def build_backend():
    print("[run.py] 编译 C++ backend...")
    BACKEND_BUILD.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        "cmake", "..",
        "-G", "Ninja",
        "-DCMAKE_C_COMPILER=clang",
        "-DCMAKE_CXX_COMPILER=clang++",
        f"-DPROFILE_MODE={'ON' if ENABLE_PROFILE else 'OFF'}",
    ]
    ccache_bin = shutil.which("ccache")
    if ccache_bin:
        cmake_args += [
            f"-DCMAKE_C_COMPILER_LAUNCHER={ccache_bin}",
            f"-DCMAKE_CXX_COMPILER_LAUNCHER={ccache_bin}",
        ]
    result = subprocess.run(cmake_args, cwd=BACKEND_BUILD)
    assert result.returncode == 0, "cmake 配置失败"

    result = subprocess.run(
        ["cmake", "--build", ".", "--config", "Release", "--parallel"],
        cwd=BACKEND_BUILD,
    )
    assert result.returncode == 0, "编译失败"

    mode = "PROFILE" if ENABLE_PROFILE else (
        "PRODUCTION" if ENABLE_PRODUCTION else "DEBUG")
    print(f"[run.py] 编译完成 (mode: {mode})")


def wait_for_port(port: int, timeout: int = 10, proc: Optional[subprocess.Popen] = None):
    start = time.time()
    while time.time() - start < timeout:
        if port_in_use(port):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def terminate_then_wait(proc: Optional[subprocess.Popen], name: str, timeout: int):
    if proc is None:
        return
    if proc.poll() is not None:
        return

    print(f"[run.py] 停止 {name}...")
    proc.terminate()
    deadline = time.time() + timeout
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.2)
    if proc.poll() is None:
        print(f"[run.py] {name} 超时，强制 kill")
        proc.kill()
    proc.wait()
    print(f"[run.py] {name} 已退出")


def main():
    assert CONFIG_FILE.exists(), f"配置文件 {CONFIG_FILE} 不存在"
    assert_linux_dependencies()
    assert not port_in_use(BACKEND_PORT), f"端口 {BACKEND_PORT} 已被占用"
    assert not port_in_use(FRONTEND_PORT), f"端口 {FRONTEND_PORT} 已被占用"

    (ROOT / "data").mkdir(exist_ok=True)
    build_backend()

    tracy_proc = None
    if ENABLE_PROFILE:
        tracy_proc = launch_tracy_ui()
        time.sleep(1.0)

    print("[run.py] 启动 backend...")
    backend = subprocess.Popen(
        [str(BACKEND_EXE), "--config", str(CONFIG_FILE)],
        cwd=ROOT,
        start_new_session=True,
    )
    frontend = None

    try:
        assert wait_for_port(BACKEND_PORT, timeout=BACKEND_STARTUP_TIMEOUT, proc=backend), (
            f"backend 启动失败 (timeout={BACKEND_STARTUP_TIMEOUT}s, exit={backend.poll()})"
        )

        print("[run.py] 启动 frontend...")
        frontend = subprocess.Popen(
            [sys.executable, "-m", "uvicorn", "main:app", "--host",
             "0.0.0.0", "--port", str(FRONTEND_PORT), "--log-level", "warning"],
            cwd=FRONTEND_DIR,
            start_new_session=True,
        )
        assert wait_for_port(FRONTEND_PORT, timeout=FRONTEND_STARTUP_TIMEOUT, proc=frontend), (
            f"frontend 启动失败 (timeout={FRONTEND_STARTUP_TIMEOUT}s, exit={frontend.poll()})"
        )

        url = f"http://localhost:{FRONTEND_PORT}"
        print(f"[run.py] 服务已启动: {url}")
        # webbrowser.open(url)

        backend.wait()
    except KeyboardInterrupt:
        pass
    finally:
        print("[run.py] 正在关闭...")
        # Shutdown is strictly ordered to avoid frontend->backend requests during teardown.
        terminate_then_wait(frontend, "frontend", GRACEFUL_SHUTDOWN_TIMEOUT)
        # Close tracy-ui before backend to avoid profiler UI reporting disconnect errors.
        terminate_then_wait(tracy_proc, "tracy-ui", 5)
        terminate_then_wait(backend, "backend", GRACEFUL_SHUTDOWN_TIMEOUT)
        print("[run.py] 已退出")


if __name__ == "__main__":
    main()
