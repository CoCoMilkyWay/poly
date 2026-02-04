#!/usr/bin/env python3
import subprocess
import webbrowser
import time
import sys
import signal
from pathlib import Path

ROOT = Path(__file__).parent
BACKEND_DIR = ROOT / "core-backend"
BACKEND_BUILD = BACKEND_DIR / "projects" / "core" / "build"
FRONTEND_DIR = ROOT / "core-frontend"
CONFIG_FILE = ROOT / "config.json"

# Windows 下的可执行文件名
BACKEND_EXE = BACKEND_BUILD / ("core.exe" if sys.platform == "win32" else "core")


def need_rebuild():
    """检查是否需要重新编译"""
    if not BACKEND_EXE.exists():
        return True
    
    # 检查源文件是否比可执行文件新
    src_dir = BACKEND_DIR / "src"
    if not src_dir.exists():
        return True
    
    exe_mtime = BACKEND_EXE.stat().st_mtime
    for src_file in src_dir.glob("*"):
        if src_file.stat().st_mtime > exe_mtime:
            return True
    
    cmake_file = BACKEND_DIR / "projects" / "core" / "CMakeLists.txt"
    if cmake_file.exists() and cmake_file.stat().st_mtime > exe_mtime:
        return True
    
    return False


def build_backend():
    """编译 C++ backend"""
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
    result = subprocess.run(["cmake", "--build", ".", "--config", "Release"], cwd=BACKEND_BUILD)
    assert result.returncode == 0, "编译失败"
    
    print("[run.py] 编译完成")


def check_config():
    """检查配置文件"""
    example_file = ROOT / "config.json"
    
    if not CONFIG_FILE.exists():
        if example_file.exists():
            import shutil
            shutil.copy(example_file, CONFIG_FILE)
            print(f"[run.py] 已创建 config.json")
        else:
            print(f"[run.py] 错误: 配置文件 {CONFIG_FILE} 不存在")
            print("[run.py] 请创建 config.json 并填入 API_KEY")
            sys.exit(1)
    
    import json
    with open(CONFIG_FILE) as f:
        config = json.load(f)
    
    if config.get("api_key") == "YOUR_THE_GRAPH_API_KEY":
        print("[run.py] 警告: 请在 config.json 中填入有效的 The Graph API Key")
        print("[run.py] 获取 API Key: https://thegraph.com/studio/apikeys/")


def main():
    processes = []
    
    def cleanup(signum=None, frame=None):
        print("\n[run.py] 正在关闭...")
        for p in processes:
            if p.poll() is None:
                p.terminate()
        sys.exit(0)
    
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)
    
    # 1. 检查配置
    check_config()
    
    # 1.5 确保 data 目录存在
    (ROOT / "data").mkdir(exist_ok=True)
    
    # 2. 编译 C++ backend(如果需要)
    if need_rebuild():
        build_backend()
    else:
        print("[run.py] backend 已是最新，跳过编译")
    
    # 3. 启动 C++ backend(后台)
    print("[run.py] 启动 backend...")
    backend_proc = subprocess.Popen(
        [str(BACKEND_EXE), "--config", str(CONFIG_FILE)],
        cwd=ROOT
    )
    processes.append(backend_proc)
    
    # 4. 启动 Python frontend(后台)
    print("[run.py] 启动 frontend...")
    frontend_proc = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"],
        cwd=FRONTEND_DIR
    )
    processes.append(frontend_proc)
    
    # 5. 等待服务就绪，打开浏览器
    time.sleep(2)
    url = "http://localhost:8000"
    print(f"[run.py] 打开浏览器: {url}")
    webbrowser.open(url)
    
    # 6. 等待进程退出
    print("[run.py] 服务已启动，按 Ctrl+C 退出")
    try:
        # 等待任一进程退出
        while True:
            for p in processes:
                if p.poll() is not None:
                    print(f"[run.py] 进程 {p.pid} 已退出，退出码: {p.returncode}")
                    cleanup()
            time.sleep(1)
    except KeyboardInterrupt:
        cleanup()


if __name__ == "__main__":
    main()
