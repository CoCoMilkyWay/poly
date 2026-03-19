#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
FRONTEND_DIR = ROOT / "frontend"
BACKEND_EXE = BUILD_DIR / "tracker_backend"

BACKEND_PORT = int(os.environ.get("TRACKER_BACKEND_PORT", "8871"))
FRONTEND_PORT = int(os.environ.get("TRACKER_FRONTEND_PORT", "8870"))
BACKEND_HOST = os.environ.get("TRACKER_BACKEND_HOST", "0.0.0.0").strip()
FRONTEND_HOST = os.environ.get("TRACKER_FRONTEND_HOST", "0.0.0.0").strip()

REQUIRED_TOOLS = [
    "cmake",
    "clang++",
    "ninja",
]

REQUIRED_PACKAGES = [
    "build-essential",
    "libssl-dev",
    "ninja-build",
]


def port_in_use(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        return sock.connect_ex(("127.0.0.1", port)) == 0


def dpkg_package_installed(package: str) -> bool:
    result = subprocess.run(
        ["dpkg-query", "-W", "-f=${Status}", package],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0 and "install ok installed" in result.stdout


def assert_dependencies() -> None:
    assert sys.platform == "linux", "仅支持 Linux"
    missing_tools = [tool for tool in REQUIRED_TOOLS if shutil.which(tool) is None]
    missing_packages = [pkg for pkg in REQUIRED_PACKAGES if not dpkg_package_installed(pkg)]
    assert not missing_tools, f"缺少工具: {missing_tools}"
    assert not missing_packages, (
        "缺少系统依赖, 运行: sudo apt update && sudo apt install -y "
        + " ".join(missing_packages)
    )


def build_backend() -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    configure = subprocess.run(
        [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(BUILD_DIR),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        cwd=ROOT,
    )
    assert configure.returncode == 0, "cmake configure failed"
    build = subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--parallel"],
        cwd=ROOT,
    )
    assert build.returncode == 0, "cmake build failed"
    assert BACKEND_EXE.exists(), str(BACKEND_EXE)


def wait_for_port(port: int, proc: subprocess.Popen[str]) -> None:
    while True:
        if port_in_use(port):
            return
        if proc.poll() is not None:
            assert False, f"进程提前退出: {proc.poll()}"
        time.sleep(0.2)


def terminate_process(proc: subprocess.Popen[str] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    deadline = time.time() + 10
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.2)
    if proc.poll() is None:
        proc.kill()
    proc.wait()


def main() -> None:
    assert_dependencies()
    assert not port_in_use(BACKEND_PORT), f"backend 端口已占用: {BACKEND_PORT}"
    assert not port_in_use(FRONTEND_PORT), f"frontend 端口已占用: {FRONTEND_PORT}"
    assert FRONTEND_DIR.exists(), str(FRONTEND_DIR)

    build_backend()

    print(
        json.dumps(
            {
                "frontend_url": f"http://localhost:{FRONTEND_PORT}",
                "backend_url": f"http://localhost:{BACKEND_PORT}",
                "address_file": str(ROOT / "address.txt"),
                "snapshot_file": str(ROOT / "data" / "snapshot.json"),
                "history_file": str(ROOT / "data" / "history.json"),
                "aggregate_file": str(ROOT / "data" / "aggregate.json"),
                "meta_file": str(ROOT / "data" / "meta.json"),
            },
            ensure_ascii=False,
            indent=2,
        )
    )

    env = dict(os.environ)
    env["TRACKER_BACKEND_PORT"] = str(BACKEND_PORT)
    env["TRACKER_FRONTEND_PORT"] = str(FRONTEND_PORT)
    env["TRACKER_BACKEND_HOST"] = BACKEND_HOST
    env["TRACKER_FRONTEND_HOST"] = FRONTEND_HOST

    backend = subprocess.Popen(
        [str(BACKEND_EXE)],
        cwd=ROOT,
        env=env,
        start_new_session=True,
        text=True,
    )
    frontend = None

    try:
        wait_for_port(BACKEND_PORT, backend)
        frontend = subprocess.Popen(
            [
                sys.executable,
                "-m",
                "http.server",
                str(FRONTEND_PORT),
                "--bind",
                FRONTEND_HOST,
                "--directory",
                str(FRONTEND_DIR),
            ],
            cwd=ROOT,
            env=env,
            start_new_session=True,
            text=True,
        )
        wait_for_port(FRONTEND_PORT, frontend)

        backend_exit = backend.wait()
        assert backend_exit == 0, f"backend exited: {backend_exit}"
    finally:
        terminate_process(frontend)
        terminate_process(backend)


if __name__ == "__main__":
    main()
