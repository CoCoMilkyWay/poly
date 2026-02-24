#!/usr/bin/env python3
"""Polygon Full Node: Erigon 启动 + 同步进度 Dashboard

用法: python run.py
Dashboard: http://localhost:8800

架构: Heimdall (共识层) -> Erigon Bor (执行层) -> JSON-RPC :8545 -> Indexer (eth_getLogs)

- Heimdall: 共识/检查点服务, Erigon 通过 --bor.heimdall 连接
- Erigon Bor: 执行层, 存储区块/交易/日志
- --bor.heimdall 可指向远程公共 API (https://heimdall-api.polygon.technology), 不必本地跑 Heimdall
"""

# ═══════════════════════ Erigon Snapshot 存储结构 ═══════════════════════
#
# snapshots/ 目录总计 ~11000 个文件，~4.5TB，分布在 5 个位置：
#
# 1. snapshots/ (根目录) — 区块链段数据 (~1.7TB)
#    文件命名: v1.1-{start}-{end}-{type}.seg  +  对应 .idx 索引
#    type: bodies, headers, transactions, borevents, borspans, borcheckpoints
#    每个 .seg 都有配套的 .seg.torrent 和 .idx.torrent
#
#    分段粒度随区块高度递减（合并策略）:
#      - 旧区块: 500k blocks/seg (如 000000-000500)
#      - 中期:   100k blocks/seg (如 050000-050100)
#      - 近期:    10k blocks/seg (如 076400-076410)
#      - 链尖:     1k blocks/seg (如 076476-076477)
#    borcheckpoints 始终是 100k 粒度，嵌套在 500k 大段内。
#
# 2. snapshots/domain/ — 状态数据 (~1.1TB)
#    accounts, code, storage, commitment 的 KV 存储 (.kv, .bt, .kvei)
#    数字范围是 "step" 而非 block number，如 0-2048, 2048-3072
#    还有 rcache (读缓存) 和 commitment (MPT 承诺)
#
# 3. snapshots/history/ — 历史状态数据 (~1.4TB)
#    accounts, code, storage 的历史版本 (.v 文件)
#    rcache 历史、receipt 历史
#    是整个 snapshot 中最大的部分
#
# 4. snapshots/idx/ — 倒排索引 (~0.2TB)
#    logaddrs, logtopics, tracesfrom, tracesto 等 (.ef 文件)
#    rcache 的 ef 索引
#
# 5. snapshots/accessor/ — 访问器 (~0.05TB)
#    accounts, code, rcache 的 .vi 文件
#    全部已下载完成
#
# ── 下载机制 ──
# Erigon 通过 BitTorrent (OtterSync) 下载 snapshot 文件。
# 每个预期文件都有对应的 .torrent 元数据文件。
# 下载中的文件以 .part 后缀保存，完成后去掉后缀。
#
# .part 文件是**稀疏文件** (sparse file):
#   - os.stat().st_size (apparent size) = 文件的期望总大小（预分配）
#   - os.stat().st_blocks * 512        = 实际已写入磁盘的字节数
#   因此 on_disk / apparent_size 即为该文件的真实下载进度。
#
# 重启后 OtterSync 会重新 hash 所有已下载文件做完整性校验，
# 日志中的 data="X% - A/B" 显示的是 **hashing 进度**而非下载进度。
# hashing 速度约 2.5GB/s，1.7TB 数据大约需要 11 分钟才能跑完。
#
# ═════════════════════════════════════════════════════════════════════════


import collections
import json
import logging
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)

# ═══════════════════════ 配置 ═══════════════════════
PROJECT_DIR = Path(__file__).parent.parent
NODE_DIR = PROJECT_DIR / "polygon-node"
ERIGON = NODE_DIR / "erigon_v3.3.8_linux_amd64v2" / "erigon"
DATADIR = NODE_DIR / "data"
SNAP_DIR = DATADIR / "snapshots"
SNAP_SUBDIRS = ['', 'domain', 'history', 'idx', 'accessor']
HEIMDALL = "https://heimdall-api.polygon.technology"
RPC_ADDR = "127.0.0.1"
RPC_PORT = 8545
WEB_PORT = 8800
HTML_FILE = Path(__file__).parent / "dashboard.html"
# ════════════════════════════════════════════════════

# ═══════════════════════ 全局状态 ═══════════════════════
proc = None
proc_start_time = 0
log_buffer = collections.deque(maxlen=800)
sync_state = {}
snap_cache = {}
snap_cache_time = 0
highest_block_seen = 0
block_history = collections.deque(maxlen=120)  # 保留2分钟历史 (每秒采样一次)
# ════════════════════════════════════════════════════════

# ═══════════════════════ 正则表达式 ═══════════════════════
KV_RE = re.compile(r'([\w][\w.-]*)=(?:"([^"]*)"|((?:[^\s"])+))')
STAGE_RE = re.compile(r'\[(\d+)/(\d+) (\w+)\]')
DATA_PCT_RE = re.compile(r'([\d.]+)%')
# ══════════════════════════════════════════════════════════

# ═══════════════════════ 解析函数 ═══════════════════════


def parse_kv(line):
    """Parse key=value and key="quoted value" pairs from a log line."""
    kv = {}
    for m in KV_RE.finditer(line):
        kv[m.group(1)] = m.group(2) if m.group(2) is not None else m.group(3)
    return kv


def parse_frac(s):
    """'1212/1481' -> (1212, 1481)"""
    if "/" in s:
        a, b = s.split("/", 1)
        return int(a), int(b)
    return 0, 0


def parse_log(line):
    """解析 Erigon 日志行，提取同步状态信息。"""
    global sync_state
    # [1/1 OtterSync] Syncing  file-metadata=X/Y files=A/B data=... time-left=... ...
    if "OtterSync" in line:
        kv = parse_kv(line)
        if not kv:
            return
        info = {"phase": "snapshot", "ts": time.time()}
        info.update(kv)
        for fk in ("file-metadata", "files"):
            if fk in kv:
                c, t = parse_frac(kv[fk])
                info[fk + "_cur"] = c
                info[fk + "_total"] = t
        for ik in ("peers", "conns"):
            if ik in kv:
                info[ik] = int(kv[ik])
        # data field: "5.04% - 1.8GB/35.0GB" or plain "1.7GB"
        data_raw = kv.get("data", "")
        m = DATA_PCT_RE.match(data_raw)
        if m:
            info["data_pct"] = float(m.group(1))
        sync_state = info
    # [snapshots] Idle
    elif "[snapshots] Idle" in line:
        kv = parse_kv(line)
        sync_state.update(phase="idle", **kv)
    # [4/6 Execution] serial executed blk=76518000 blks=237 blk/s=11
    elif "Execution" in line and "serial executed" in line:
        kv = parse_kv(line)
        if "blk" in kv:
            sync_state.update(
                phase="execution",
                exec_block=int(kv["blk"]),
                exec_blks=int(kv.get("blks", 0)),
                exec_blk_per_sec=float(kv.get("blk/s", 0)),
                ts=time.time()
            )
    # [3/6 Senders] Started from=X to=Y
    elif "Senders" in line and "Started" in line:
        kv = parse_kv(line)
        sync_state.update(phase="senders", **kv, ts=time.time())
    # stage lines like [2/12 Headers], [3/12 Bodies] etc.
    else:
        m = STAGE_RE.search(line)
        if m:
            sync_state.update(phase="stage", stage_cur=m.group(
                1), stage_total=m.group(2), stage_name=m.group(3), ts=time.time())
# ════════════════════════════════════════════════════════════


# ═══════════════════════ 快照扫描 ═══════════════════════
def scan_snapshot_dir(subdir):
    """扫描单个 snapshot 子目录，统计文件大小和完成度。"""
    path = str(SNAP_DIR / subdir) if subdir else str(SNAP_DIR)
    if not os.path.isdir(path):
        return None

    expected = actual = complete = partial = 0
    for f in os.listdir(path):
        fp = os.path.join(path, f)
        if not os.path.isfile(fp) or f.endswith('.torrent'):
            continue
        st = os.stat(fp)
        expected += st.st_size
        if f.endswith('.part'):
            actual += st.st_blocks * 512
            partial += 1
        else:
            actual += st.st_size
            complete += 1

    name = subdir if subdir else 'segments'
    return name, {
        "expected": expected,
        "actual": actual,
        "complete": complete,
        "partial": partial,
        "pct": round(actual / expected * 100, 1) if expected > 0 else 100.0,
    }


def scan_snapshot_progress():
    """扫描 snapshots 目录，利用稀疏文件的 apparent size vs on-disk blocks 计算真实下载进度。"""
    global snap_cache, snap_cache_time
    now = time.time()
    if now - snap_cache_time < 60 and snap_cache:
        return snap_cache

    result = {}
    total_expected = total_actual = 0

    for subdir in SNAP_SUBDIRS:
        scan_result = scan_snapshot_dir(subdir)
        if scan_result:
            name, stats = scan_result
            result[name] = stats
            total_expected += stats["expected"]
            total_actual += stats["actual"]

    result["total"] = {
        "expected": total_expected,
        "actual": total_actual,
        "pct": round(total_actual / total_expected * 100, 1) if total_expected > 0 else 100.0,
    }

    snap_cache = result
    snap_cache_time = now
    return result
# ════════════════════════════════════════════════════════════


# ═══════════════════════ RPC 调用 ═══════════════════════
def rpc_call(method, params=None):
    """调用 Erigon JSON-RPC 接口。"""
    body = json.dumps({"jsonrpc": "2.0", "method": method,
                      "params": params or [], "id": 1}).encode()
    req = urllib.request.Request(
        f"http://{RPC_ADDR}:{RPC_PORT}",
        data=body,
        headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read()).get("result")
    except Exception:
        return None


def get_rpc_status():
    """获取区块同步状态。"""
    global highest_block_seen, block_history

    syncing = rpc_call("eth_syncing")
    block_num = rpc_call("eth_blockNumber")

    status = {
        "rpc": syncing is not None or block_num is not None,
        "syncing": False,
        "current": 0,
        "highest": 0,
        "pct": 0.0,
        "stages": [],
        "speed": 0.0,
    }

    if isinstance(block_num, str):
        status["current"] = int(block_num, 16)

    if isinstance(syncing, dict):
        status["syncing"] = True
        status["current"] = int(syncing.get("currentBlock", "0x0"), 16)
        reported_highest = int(syncing.get("highestBlock", "0x0"), 16)
        highest_block_seen = max(highest_block_seen, reported_highest)
        status["highest"] = highest_block_seen
        status["stages"] = syncing.get("stages", [])
        if status["highest"] > 0:
            status["pct"] = round(status["current"] /
                                  status["highest"] * 100, 2)
    elif syncing is False and status["current"] > 0:
        status["pct"] = 100.0

    # 记录区块历史并计算速度
    now = time.time()
    if status["current"] > 0:
        block_history.append((now, status["current"]))

        # 计算最近60秒的平均速度
        if len(block_history) >= 2:
            cutoff_time = now - 60
            recent = [x for x in block_history if x[0] >= cutoff_time]
            if len(recent) >= 2:
                time_span = recent[-1][0] - recent[0][0]
                block_span = recent[-1][1] - recent[0][1]
                if time_span > 0:
                    status["speed"] = round(block_span / time_span, 2)

    return status


def get_disk_status():
    """获取磁盘使用情况。"""
    usage = shutil.disk_usage(str(DATADIR) if DATADIR.exists() else "/")
    return {
        "total": round(usage.total / 1e9, 1),
        "used": round(usage.used / 1e9, 1),
        "free": round(usage.free / 1e9, 1),
    }


def get_status():
    """获取完整的节点状态信息。"""
    alive = proc is not None and proc.poll() is None
    rpc_status = get_rpc_status()

    status = {
        "alive": alive,
        "uptime": int(time.time() - proc_start_time) if alive else 0,
        "exit_code": proc.returncode if proc and not alive else None,
    }

    status.update(rpc_status)
    status["disk"] = get_disk_status()
    status["sync"] = dict(sync_state)
    status["snap_progress"] = scan_snapshot_progress()

    return status
# ════════════════════════════════════════════════════════════


# ═══════════════════════ 进程管理 ═══════════════════════
def read_stream(stream):
    """读取进程输出流，记录日志并解析状态。"""
    for raw in iter(stream.readline, b""):
        line = raw.decode("utf-8", errors="replace").rstrip()
        if line:
            log_buffer.append(f"[{time.strftime('%H:%M:%S')}] {line}")
            parse_log(line)
# ════════════════════════════════════════════════════════════


# ═══════════════════════ HTTP Server ═══════════════════════
class DashboardHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self.send_response_data(
                200, "text/html; charset=utf-8", HTML_FILE.read_bytes())
        elif self.path == "/api/status":
            self.send_response_json(200, get_status())
        elif self.path == "/api/logs":
            self.send_response_json(200, list(log_buffer))
        else:
            self.send_error(404)

    def send_response_data(self, code, content_type, body):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.end_headers()
        self.wfile.write(body)

    def send_response_json(self, code, data):
        self.send_response_data(code, "application/json",
                                json.dumps(data).encode())

    def log_message(self, *_):
        pass
# ════════════════════════════════════════════════════════════


# ═══════════════════════ 进程启动 ═══════════════════════
def build_erigon_command():
    """构建 Erigon 启动命令。"""
    return [
        str(ERIGON),
        f"--datadir={DATADIR}",              # 数据目录
        "--chain=bor-mainnet",
        "--prune.mode=blocks",               # 保留 blocks, 不存 state 历史
        f"--bor.heimdall={HEIMDALL}",        # Heimdall 服务
        "--http", f"--http.addr={RPC_ADDR}", f"--http.port={RPC_PORT}",
        "--http.api=eth,net,web3",
        "--ws", "--ws.port=8546",
        "--private.api.addr=127.0.0.1:9090",
        "--port=30303",
        "--torrent.port=42069",
        "--torrent.conns.perfile=50",        # 增加 torrent 并发，快抓快下 segments
        "--torrent.upload.rate=0",           # 不限制上传
        "--torrent.download.rate=0",         # 不限制下载，快速获取 segments
        "--batchSize=6g",                    # 执行阶段批量提高（依赖 RAM）
        "--sync.loop.block.limit=5000",      # 每轮最多处理 2048 个块，快追高度
        "--snap.keepblocks=true",            # 保留 snapshot 避免丢数据
        "--snap.state.stop=false",           # 允许 snapshot state 自动生成
        "--sync.parallel-state-flushing",    # 并行写 state，加速 rebuild
        "--db.writemap",                     # MDBX 写入映射加速磁盘提交
        "--db.read.concurrency=512",         # 提高并行读取，加快 replay
        "--state.cache=16G",                 # RAM 足够的话，加大 state cache
        "--sync.loop.throttle=0s",           # 不限制循环延迟
    ]


def build_clean_env():
    """构建干净的环境变量（禁用代理，避免 torrent 流量走代理）。"""
    env = os.environ.copy()
    for key in ['http_proxy', 'https_proxy', 'HTTP_PROXY', 'HTTPS_PROXY', 'all_proxy', 'ALL_PROXY']:
        env.pop(key, None)
    return env


def start_erigon_process():
    """启动 Erigon 进程。"""
    global proc, proc_start_time

    cmd = build_erigon_command()
    env = build_clean_env()

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env)
    proc_start_time = time.time()

    for stream in (proc.stdout, proc.stderr):
        threading.Thread(target=read_stream, args=(
            stream,), daemon=True).start()


def stop_erigon_process(sig=None, frame=None):
    """停止 Erigon 进程。"""
    logging.info("收到停止信号，正在关闭 Erigon...")
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=60)
            logging.info("Erigon 已正常退出")
        except subprocess.TimeoutExpired:
            logging.warning("Erigon 未在 60 秒内退出，强制终止")
            proc.kill()
            proc.wait()
    sys.exit(0)
# ════════════════════════════════════════════════════════════


# ═══════════════════════ 主函数 ═══════════════════════
def main():
    assert ERIGON.exists(), f"找不到 erigon: {ERIGON}"
    assert HTML_FILE.exists(), f"找不到 dashboard: {HTML_FILE}"
    DATADIR.mkdir(parents=True, exist_ok=True)

    logging.info("启动 Erigon Polygon Full Node")
    logging.info(f"datadir: {DATADIR}")
    logging.info(f"Dashboard → http://localhost:{WEB_PORT}")
    logging.info(f"RPC      → http://{RPC_ADDR}:{RPC_PORT}")

    start_erigon_process()

    signal.signal(signal.SIGINT, stop_erigon_process)
    signal.signal(signal.SIGTERM, stop_erigon_process)

    server = HTTPServer(("0.0.0.0", WEB_PORT), DashboardHandler)
    logging.info("Dashboard 就绪")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        stop_erigon_process()
# ════════════════════════════════════════════════════════════


if __name__ == "__main__":
    main()
