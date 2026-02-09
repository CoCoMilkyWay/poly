#!/usr/bin/env python3
"""Polygon Full Node: Erigon 启动 + 同步进度 Dashboard

用法: python node/run.py
Dashboard: http://localhost:8800


架构
Polygon 网络由两层组成：

graph LR
    Heimdall["Heimdall (共识层)"] -->|checkpoint 验证| Erigon["Erigon Bor (执行层)"]
    Erigon -->|JSON-RPC :8545| Indexer["Custom Indexer (eth_getLogs)"]

Heimdall: Polygon 的共识/检查点服务,Erigon 通过 --bor.heimdall 连接

Erigon Bor: 执行层,存储区块/交易/日志

Erigon 的 --bor.heimdall 支持指向远程公共 API,不一定要本地跑 Heimdall 节点：
选项 A (推荐先用): 用公共 Heimdall API https://heimdall-api.polygon.technology,省去部署 Heimdall 的复杂度。同步完成后再评估是否需要本地 Heimdall。
选项 B: 本地跑 Heimdall + Erigon 全套。更自主,但 Heimdall 也需要额外磁盘和同步时间。

"""

import subprocess, threading, time, json, sys, signal, shutil, collections, re
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
import urllib.request

# ═══════════════════════ 配置 ═══════════════════════
ERIGON   = Path("/home/chuyin/work/erigon_v3.3.7_linux_amd64v2/erigon")
DATADIR  = Path(__file__).parent / "polygon-data"
HEIMDALL = "https://heimdall-api.polygon.technology"
RPC_ADDR = "127.0.0.1"
RPC_PORT = 8545
WEB_PORT = 8800
# ════════════════════════════════════════════════════

proc = None
log_buf = collections.deque(maxlen=800)
t0 = 0
sync_info = {}  # parsed from erigon log lines


def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "method": method, "params": params or [], "id": 1}).encode()
    req = urllib.request.Request(
        f"http://{RPC_ADDR}:{RPC_PORT}", data=body,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read()).get("result")
    except Exception:
        return None


def get_status():
    alive = proc is not None and proc.poll() is None
    syn = rpc("eth_syncing")
    bn  = rpc("eth_blockNumber")
    rpc_ok = syn is not None or bn is not None

    s = {
        "alive": alive, "rpc": rpc_ok, "syncing": False,
        "current": 0, "highest": 0, "pct": 0.0, "stages": [],
        "uptime": int(time.time() - t0) if alive else 0,
        "exit_code": proc.returncode if proc and not alive else None,
    }

    if isinstance(bn, str):
        s["current"] = int(bn, 16)

    if isinstance(syn, dict):
        s["syncing"] = True
        s["current"] = int(syn.get("currentBlock", "0x0"), 16)
        s["highest"] = int(syn.get("highestBlock", "0x0"), 16)
        s["stages"]  = syn.get("stages", [])
        if s["highest"] > 0:
            s["pct"] = round(s["current"] / s["highest"] * 100, 2)
    elif syn is False and s["current"] > 0:
        s["pct"] = 100.0

    try:
        u = shutil.disk_usage(str(DATADIR) if DATADIR.exists() else "/")
        s["disk"] = {"total": round(u.total / 1e9, 1), "used": round(u.used / 1e9, 1), "free": round(u.free / 1e9, 1)}
    except Exception:
        s["disk"] = {"total": 0, "used": 0, "free": 0}

    s["sync"] = dict(sync_info)
    return s


def _read_stream(stream):
    for raw in iter(stream.readline, b""):
        line = raw.decode("utf-8", errors="replace").rstrip()
        if line:
            log_buf.append(f"[{time.strftime('%H:%M:%S')}] {line}")
            _parse_log(line)


_KV_RE = re.compile(r'([\w][\w.-]*)=(?:"([^"]*)"|((?:[^\s"])+))')

def _parse_kv(line):
    """Parse key=value and key="quoted value" pairs from a log line."""
    kv = {}
    for m in _KV_RE.finditer(line):
        kv[m.group(1)] = m.group(2) if m.group(2) is not None else m.group(3)
    return kv

def _parse_frac(s):
    """'1212/1481' -> (1212, 1481)"""
    if "/" in s:
        a, b = s.split("/", 1)
        return int(a), int(b)
    return 0, 0


def _parse_log(line):
    global sync_info
    # [1/1 OtterSync] Syncing  file-metadata=X/Y files=A/B data=... time-left=... ...
    if "OtterSync" in line:
        kv = _parse_kv(line)
        if not kv:
            return
        info = {"phase": "snapshot", "ts": time.time()}
        info.update(kv)
        for fk in ("file-metadata", "files"):
            if fk in kv:
                c, t = _parse_frac(kv[fk])
                info[fk + "_cur"] = c
                info[fk + "_total"] = t
        for ik in ("peers", "conns"):
            if ik in kv:
                info[ik] = int(kv[ik])
        # data field: "5.04% - 1.8GB/35.0GB" or plain "1.7GB"
        data_raw = kv.get("data", "")
        m = re.match(r'([\d.]+)%', data_raw)
        if m:
            info["data_pct"] = float(m.group(1))
        sync_info = info
    # [snapshots] Idle
    elif "[snapshots] Idle" in line:
        kv = _parse_kv(line)
        sync_info.update(phase="idle", **kv)
    # stage lines like [2/12 Headers], [3/12 Bodies] etc.
    elif re.search(r'\[\d+/\d+ \w+\]', line):
        m = re.search(r'\[(\d+)/(\d+) (\w+)\]', line)
        if m:
            sync_info.update(phase="stage", stage_cur=m.group(1), stage_total=m.group(2), stage_name=m.group(3))


# ═══════════════════════ HTML Dashboard ═══════════════════════

HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Polygon Node</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg:#0a0a0a;--card:#141414;--border:#232323;
  --text:#e0e0e0;--dim:#666;--accent:#6c5ce7;
  --green:#00b894;--yellow:#fdcb6e;--red:#d63031;
}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:var(--bg);color:var(--text)}
.w{max-width:1200px;margin:0 auto;padding:24px}
header{display:flex;align-items:center;gap:16px;margin-bottom:20px}
header h1{font-size:1.5em;font-weight:600}
.badge{padding:4px 14px;border-radius:12px;font-size:.8em;font-weight:500}
.badge.g{background:rgba(0,184,148,.15);color:var(--green)}
.badge.y{background:rgba(253,203,110,.15);color:var(--yellow)}
.badge.r{background:rgba(214,48,49,.15);color:var(--red)}
.badge.x{background:rgba(102,102,102,.15);color:var(--dim)}

.sec{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px;margin-bottom:16px}
.sec h3{font-size:.88em;color:var(--dim);margin-bottom:14px}

.prog-row{margin-bottom:14px}
.prog-hd{display:flex;justify-content:space-between;font-size:.82em;margin-bottom:6px}
.prog-hd .dim{color:var(--dim)}
.prog-bar{height:20px;background:#1a1a1a;border-radius:10px;overflow:hidden}
.prog-fill{height:100%;border-radius:10px;transition:width .6s ease}
.prog-fill.a{background:linear-gradient(90deg,#6c5ce7,#a29bfe)}
.prog-fill.b{background:linear-gradient(90deg,#00b894,#55efc4)}

.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}
.stat{background:#0e0e0e;border-radius:8px;padding:12px 14px}
.stat .k{font-size:.68em;color:var(--dim);text-transform:uppercase;letter-spacing:.3px;margin-bottom:4px}
.stat .v{font-size:1.05em;font-weight:600;font-variant-numeric:tabular-nums}

.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:18px}
.card .l{font-size:.72em;color:var(--dim);text-transform:uppercase;letter-spacing:.5px;margin-bottom:6px}
.card .val{font-size:1.4em;font-weight:600;font-variant-numeric:tabular-nums}
.card .sub{font-size:.78em;color:var(--dim);margin-top:4px}

.stg-table{width:100%;border-collapse:collapse;margin-top:10px}
.stg-table td,.stg-table th{padding:5px 10px;text-align:left;font-size:.82em}
.stg-table th{color:var(--dim);font-weight:500}
.stg-table tr:not(:last-child) td{border-bottom:1px solid var(--border)}

#log-box{height:240px;overflow-y:auto;background:#0c0c0c;border-radius:8px;padding:12px;
  font-family:'SF Mono','Fira Code','Cascadia Code',monospace;font-size:.75em;line-height:1.6;color:#555}
#log-box .ln{white-space:pre-wrap;word-break:break-all}
#log-box .ln:hover{color:#aaa}
</style>
</head>
<body>
<div class="w">

<header>
  <h1>Polygon Node</h1>
  <span id="bd" class="badge x">启动中</span>
  <span style="flex:1"></span>
  <span style="color:var(--dim);font-size:.82em">bor-mainnet &middot; Erigon 3.3.7</span>
</header>

<!-- ======== Snapshot Download ======== -->
<div class="sec" id="snap-sec">
  <h3>Snapshot Download</h3>
  <div class="prog-row">
    <div class="prog-hd"><span>Overall</span><span class="dim" id="ov-txt">-</span></div>
    <div class="prog-bar"><div id="ov-bar" class="prog-fill b" style="width:0"></div></div>
  </div>
  <div class="prog-row">
    <div class="prog-hd"><span>Files Completed</span><span class="dim" id="fl-txt">-</span></div>
    <div class="prog-bar"><div id="fl-bar" class="prog-fill a" style="width:0"></div></div>
  </div>
  <div class="grid">
    <div class="stat"><div class="k">ETA</div><div class="v" id="s-eta">-</div></div>
    <div class="stat"><div class="k">Elapsed</div><div class="v" id="s-time">-</div></div>
    <div class="stat"><div class="k">Webseed DL</div><div class="v" id="s-ws">-</div></div>
    <div class="stat"><div class="k">Peer DL</div><div class="v" id="s-pd">-</div></div>
    <div class="stat"><div class="k">Hashing</div><div class="v" id="s-hash">-</div></div>
    <div class="stat"><div class="k">Peers</div><div class="v" id="s-peers">-</div></div>
    <div class="stat"><div class="k">Upload</div><div class="v" id="s-up">-</div></div>
    <div class="stat"><div class="k">Memory</div><div class="v" id="s-mem">-</div></div>
  </div>
</div>

<!-- ======== Block Sync ======== -->
<div class="sec" id="block-sec" style="display:none">
  <h3>Block Sync</h3>
  <div class="prog-row">
    <div class="prog-hd"><span>Progress</span><span class="dim" id="bp-txt">-</span></div>
    <div class="prog-bar"><div id="bp-bar" class="prog-fill b" style="width:0"></div></div>
  </div>
  <table class="stg-table" id="stg-tb"></table>
</div>

<!-- ======== Info Cards ======== -->
<div class="cards">
  <div class="card"><div class="l">当前区块</div><div class="val" id="cur">-</div></div>
  <div class="card"><div class="l">最高区块</div><div class="val" id="hi">-</div></div>
  <div class="card"><div class="l">磁盘剩余</div><div class="val" id="df">-</div><div class="sub" id="dd"></div></div>
  <div class="card"><div class="l">运行时间</div><div class="val" id="up">-</div></div>
</div>

<!-- ======== Logs ======== -->
<div class="sec">
  <h3 style="display:flex;justify-content:space-between">
    <span>Logs</span><span id="lc" style="font-weight:400;font-size:.85em"></span>
  </h3>
  <div id="log-box"></div>
</div>

</div>
<script>
const $=id=>document.getElementById(id);
const N=n=>n>0?n.toLocaleString():'-';
function hms(s){const h=Math.floor(s/3600),m=Math.floor(s%3600/60),sc=s%60;return h>0?h+'h '+m+'m':m>0?m+'m '+sc+'s':sc+'s'}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function pct(c,t){return t>0?(c/t*100).toFixed(1):0}

async function tick(){
 try{
  const d=await(await fetch('/api/status')).json();
  const sn=d.sync||{};
  // badge
  const b=$('bd');
  if(!d.alive){b.textContent='已停止'+(d.exit_code!=null?' ('+d.exit_code+')':'');b.className='badge r'}
  else if(sn.phase==='snapshot'){b.textContent='下载 Snapshots';b.className='badge y'}
  else if(d.syncing){b.textContent='同步区块';b.className='badge y'}
  else if(d.pct>=100){b.textContent='已同步';b.className='badge g'}
  else if(d.rpc){b.textContent='等待中';b.className='badge x'}
  else{b.textContent='启动中...';b.className='badge x'}

  // ── snapshot section ──
  const ss=$('snap-sec');
  if(sn.phase==='snapshot'){
    ss.style.display='';
    // overall progress from data field (e.g. "5.04% - 1.8GB/35.0GB")
    const dp=sn['data_pct']||0;
    const dataRaw=sn['data']||'-';
    $('ov-txt').textContent=dataRaw;
    $('ov-bar').style.width=dp+'%';
    // files completed
    const flc=sn['files_cur']||0,flt=sn['files_total']||0;
    const ffp=pct(flc,flt);
    $('fl-txt').textContent=flc+' / '+flt+'  ('+ffp+'%)';
    $('fl-bar').style.width=ffp+'%';
    // stats
    $('s-eta').textContent=sn['time-left']||'-';
    $('s-time').textContent=sn['total-time']||'-';
    $('s-ws').textContent=sn['webseed-download']||'-';
    $('s-pd').textContent=sn['peer-download']||'-';
    $('s-hash').textContent=sn['hashing-rate']||'-';
    $('s-peers').textContent=(sn['peers']||0)+' peers / '+(sn['conns']||0)+' conns';
    $('s-up').textContent=sn['upload']||'-';
    $('s-mem').textContent=(sn['alloc']||'-')+' / '+(sn['sys']||'-');
  }else{ss.style.display='none'}

  // ── block sync section ──
  const bs=$('block-sec');
  if(d.current>0||(d.stages&&d.stages.length)){
    bs.style.display='';
    $('bp-txt').textContent=d.pct+'%'+(d.highest>0?' ('+N(d.current)+' / '+N(d.highest)+')':'');
    $('bp-bar').style.width=Math.min(d.pct,100)+'%';
    const tb=$('stg-tb');
    if(d.stages&&d.stages.length){
      tb.innerHTML='<tr><th>Stage</th><th>Block</th></tr>'+d.stages.map(s=>{
        let bn=typeof s.block_number==='string'?parseInt(s.block_number,16):(s.block_number||0);
        return '<tr><td>'+(s.stage_name||s.id||'-')+'</td><td style="font-variant-numeric:tabular-nums">'+N(bn)+'</td></tr>';
      }).join('');
    }else{tb.innerHTML=''}
  }else{bs.style.display='none'}

  // ── cards ──
  $('cur').textContent=N(d.current);
  $('hi').textContent=N(d.highest);
  $('up').textContent=hms(d.uptime);
  if(d.disk){$('df').textContent=d.disk.free+' GB';$('dd').textContent=d.disk.used+' / '+d.disk.total+' GB'}
 }catch(e){}
}

async function logs(){
 try{
  const lines=await(await fetch('/api/logs')).json();
  const box=$('log-box');
  const atEnd=box.scrollHeight-box.scrollTop-box.clientHeight<60;
  box.innerHTML=lines.map(l=>'<div class="ln">'+esc(l)+'</div>').join('');
  $('lc').textContent=lines.length+' lines';
  if(atEnd)box.scrollTop=box.scrollHeight;
 }catch(e){}
}
setInterval(tick,2000);setInterval(logs,3000);tick();logs();
</script>
</body>
</html>"""


# ═══════════════════════ HTTP Server ═══════════════════════

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self._send(200, "text/html; charset=utf-8", HTML.encode())
        elif self.path == "/api/status":
            self._send(200, "application/json", json.dumps(get_status()).encode())
        elif self.path == "/api/logs":
            self._send(200, "application/json", json.dumps(list(log_buf)).encode())
        else:
            self.send_error(404)

    def _send(self, code, ct, body):
        self.send_response(code)
        self.send_header("Content-Type", ct)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


# ═══════════════════════ Main ═══════════════════════

def main():
    global proc, t0

    assert ERIGON.exists(), f"找不到 erigon: {ERIGON}"
    DATADIR.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(ERIGON),
        f"--datadir={DATADIR}",
        "--chain=bor-mainnet",
        "--prune.mode=full",
        f"--bor.heimdall={HEIMDALL}",
        "--http", f"--http.addr={RPC_ADDR}", f"--http.port={RPC_PORT}",
        "--http.api=eth,erigon,net,web3,debug,trace",
        "--ws", "--ws.port=8546",
        "--private.api.addr=127.0.0.1:9090",
        "--port=30303",
        "--torrent.port=42069",
        "--sync.loop.block.limit=10000",
        "--log.console.verbosity=info",
    ]

    print(f"[node] 启动 Erigon Polygon Full Node")
    print(f"[node] datadir: {DATADIR}")
    print(f"[node] Dashboard → http://localhost:{WEB_PORT}")
    print(f"[node] RPC      → http://{RPC_ADDR}:{RPC_PORT}")

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    t0 = time.time()

    threading.Thread(target=_read_stream, args=(proc.stdout,), daemon=True).start()
    threading.Thread(target=_read_stream, args=(proc.stderr,), daemon=True).start()

    def stop(sig=None, frame=None):
        print("\n[node] 正在关闭 Erigon...")
        if proc and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=60)
        sys.exit(0)

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    server = HTTPServer(("0.0.0.0", WEB_PORT), Handler)
    print(f"[node] Dashboard 就绪")
    server.serve_forever()


if __name__ == "__main__":
    main()
