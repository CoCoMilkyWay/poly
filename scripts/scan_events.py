#!/usr/bin/env python3
"""扫描 Polymarket 合约历史事件统计（动态终端刷新）"""

"""
  Polymarket 事件扫描  (head=76,549,285)

  块进度:   60,925,999 / 76,549,285  (79.6%)  -  速度: 15,330 blk/s  ETA: 17.0 min

  合约                 事件                 总计        首个block      最后block
  ----------------------------------------------------------------------------------
  ConditionalTokens    TransferSingle    xxx,xxx     4,027,499    76,549,000
  ConditionalTokens    TransferBatch     xxx,xxx     4,027,499    76,549,000
  ...
"""

import datetime
import json
import sys
import time
import urllib.request

RPC = "http://127.0.0.1:8545"
CHUNK = 2000  # 每次查询块数
HEAD = 83_000_000  # 固定 head

BLOCK_TIME = 2  # 秒/block
UTC = datetime.timezone.utc
NOW_TS = int(datetime.datetime.now(UTC).timestamp())
# HEAD 对应当前时间，往前推算
BLOCK0_TS = NOW_TS - HEAD * BLOCK_TIME
YEAR0 = datetime.datetime.fromtimestamp(BLOCK0_TS, UTC).year
YEARS = list(range(YEAR0, datetime.datetime.now(UTC).year + 1))

def block_to_year(blknum):
    ts = NOW_TS - (HEAD - blknum) * BLOCK_TIME
    return datetime.datetime.fromtimestamp(ts, UTC).year

# ── 合约地址 ──────────────────────────────────────────────────
CONTRACTS = {
    "ConditionalTokens":   "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045",
    "CTFExchange":         "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E",
    "NegRiskCTFExchange":  "0xC5d563A36AE78145C45a50134d48A1215220f80a",
    "NegRiskAdapter":      "0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296",
}

# ── 事件 topic（keccak256 of signature）────────────────────────
EVENTS = {
    # ConditionalTokens
    "0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62": ("ConditionalTokens", "TransferSingle"),
    "0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb": ("ConditionalTokens", "TransferBatch"),
    "0xab3760c3bd2bb38b5bcf54dc79802ed67338b4cf29f3054ded67ed24661e4177": ("ConditionalTokens", "ConditionPreparation"),
    "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894": ("ConditionalTokens", "ConditionResolution"),
    "0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298": ("ConditionalTokens", "PositionSplit"),
    "0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca": ("ConditionalTokens", "PositionsMerge"),
    "0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d": ("ConditionalTokens", "PayoutRedemption"),
    # CTFExchange
    "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6": ("CTFExchange", "OrderFilled"),
    "0x63bf4d16b7fa898ef4c4b2b6d90fd201e9c56313b65638af6088d149d2ce956c": ("CTFExchange", "OrdersMatched"),
    "0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d": ("CTFExchange", "TokenRegistered"),
    # NegRiskCTFExchange (same topics as CTFExchange)
    # NegRiskAdapter
    "0xaac410f87d423a922a7b226ac68f5ba267c3a70a6eb6db3a0d11a8027dc303b0": ("NegRiskAdapter", "MarketPrepared"),
    "0xf059ab16d1ca60e123eab60e3c02d4ab3e7c267b7dbb5361a38e7fbc7d5e5a05": ("NegRiskAdapter", "QuestionPrepared"),
    "0xbbed930dbfb7907ae2d60ddf78341a2c1f8cfe1e6f3b4e74c8c7b7a0be5e9e16": ("NegRiskAdapter", "PositionsConverted"),
    "0xd5a3c84c3953ba5cf608be6e2bd08447c931838ab29dc6543f5e5575d1574e8f": ("NegRiskAdapter", "OutcomeReported"),
}

# ── 状态 ──────────────────────────────────────────────────────
counts    = {}   # (contract, event) -> int
first_blk = {}   # (contract, event) -> int
last_blk  = {}   # (contract, event) -> int
yearly     = {}   # (contract, event, year) -> int
recent_times = []  # 最近20次查询的 (block, time) 用于计算滑动平均速度

def rpc_call(method, params, timeout=20):
    req = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": 1}).encode()
    with urllib.request.urlopen(
        urllib.request.Request(RPC, req, {"Content-Type": "application/json"}),
        timeout=timeout
    ) as r:
        return json.loads(r.read())

def get_head():
    res = rpc_call("eth_blockNumber", [])
    return int(res["result"], 16)

def process_logs(logs, contract_name):
    for log in logs:
        if not log.get("topics"):
            continue
        topic = log["topics"][0].lower()
        blknum = int(log["blockNumber"], 16)

        if topic not in EVENTS:
            continue
        base_contract, event_name = EVENTS[topic]
        key = (contract_name, event_name)

        counts[key] = counts.get(key, 0) + 1
        if key not in first_blk or blknum < first_blk[key]:
            first_blk[key] = blknum
        if key not in last_blk or blknum > last_blk[key]:
            last_blk[key] = blknum

        year = block_to_year(blknum)
        ykey = (contract_name, event_name, year)
        yearly[ykey] = yearly.get(ykey, 0) + 1

LINES = 0

def render(cur_block, head, now):
    global LINES
    if LINES:
        sys.stdout.write(f"\033[{LINES}A\033[J")

    recent_times.append((cur_block, now))
    if len(recent_times) > 20:
        recent_times.pop(0)

    pct = cur_block / head * 100 if head else 0
    if len(recent_times) >= 2:
        blk_diff = recent_times[-1][0] - recent_times[0][0]
        time_diff = recent_times[-1][1] - recent_times[0][1]
        rate = blk_diff / time_diff if time_diff > 0 else 0
    else:
        rate = 0
    eta = (head - cur_block) / rate if rate > 0 else 0

    lines = []
    lines.append(f"  块进度: {cur_block:>12,} / {head:,}  ({pct:.1f}%)  -  速度: {rate:,.0f} blk/s  ETA: {eta/60:.1f} min")
    lines.append("")

    order = [
        ("ConditionalTokens", "TransferSingle"),
        ("ConditionalTokens", "TransferBatch"),
        ("ConditionalTokens", "ConditionPreparation"),
        ("ConditionalTokens", "ConditionResolution"),
        ("ConditionalTokens", "PositionSplit"),
        ("ConditionalTokens", "PositionsMerge"),
        ("ConditionalTokens", "PayoutRedemption"),
        ("CTFExchange", "OrderFilled"),
        ("CTFExchange", "OrdersMatched"),
        ("CTFExchange", "TokenRegistered"),
        ("NegRiskCTFExchange", "OrderFilled"),
        ("NegRiskCTFExchange", "OrdersMatched"),
        ("NegRiskCTFExchange", "TokenRegistered"),
        ("NegRiskAdapter", "MarketPrepared"),
        ("NegRiskAdapter", "QuestionPrepared"),
        ("NegRiskAdapter", "PositionsConverted"),
        ("NegRiskAdapter", "OutcomeReported"),
    ]
    year_hdrs = "".join(f"{y:>10}" for y in YEARS)
    lines.append(f"  {'合约':<18}  {'事件':<20}  {'总计':>12}  {'首block':>12}  {'末block':>12}{year_hdrs}")
    lines.append("  " + "-" * (18+2+20+2+12+2+12+2+12 + len(YEARS)*10))
    for key in order:
        cnt = counts.get(key, 0)
        fb = f"{first_blk[key]:>12,}" if key in first_blk else "-".center(12)
        lb = f"{last_blk[key]:>12,}" if key in last_blk else "-".center(12)
        ycols = "".join(f"{yearly.get((key[0], key[1], y), 0):>10,}" for y in YEARS)
        lines.append(f"  {key[0]:<18}  {key[1]:<20}  {cnt:>12,}  {fb}  {lb}{ycols}")

    for line in lines:
        sys.stdout.write(line + "\n")
    sys.stdout.flush()
    LINES = len(lines)

def main():
    head = HEAD
    print(f"\n  Polymarket 事件扫描  (head={head:,})\n")

    ct_topics = [t for t, (c, _) in EVENTS.items() if c == "ConditionalTokens"]
    ex_topics = [t for t, (c, _) in EVENTS.items() if c == "CTFExchange"]
    nra_topics = [t for t, (c, _) in EVENTS.items() if c == "NegRiskAdapter"]

    start_time = time.time()
    cur = 0

    while cur <= head:
        end = min(cur + CHUNK - 1, head)

        # ConditionalTokens
        res = rpc_call("eth_getLogs", [{
            "fromBlock": hex(cur),
            "toBlock":   hex(end),
            "address":   CONTRACTS["ConditionalTokens"],
            "topics":    [ct_topics],
        }], timeout=30)
        assert "result" in res, res
        process_logs(res["result"], "ConditionalTokens")

        # CTFExchange
        res = rpc_call("eth_getLogs", [{
            "fromBlock": hex(cur),
            "toBlock":   hex(end),
            "address":   CONTRACTS["CTFExchange"],
            "topics":    [ex_topics],
        }], timeout=30)
        assert "result" in res, res
        process_logs(res["result"], "CTFExchange")

        # NegRiskCTFExchange
        res = rpc_call("eth_getLogs", [{
            "fromBlock": hex(cur),
            "toBlock":   hex(end),
            "address":   CONTRACTS["NegRiskCTFExchange"],
            "topics":    [ex_topics],
        }], timeout=30)
        assert "result" in res, res
        process_logs(res["result"], "NegRiskCTFExchange")

        # NegRiskAdapter
        res = rpc_call("eth_getLogs", [{
            "fromBlock": hex(cur),
            "toBlock":   hex(end),
            "address":   CONTRACTS["NegRiskAdapter"],
            "topics":    [nra_topics],
        }], timeout=30)
        assert "result" in res, res
        process_logs(res["result"], "NegRiskAdapter")

        render(end, head, time.time())
        cur = end + 1

    elapsed = time.time() - start_time
    print(f"\n  扫描完成，耗时 {elapsed:.0f}s\n")

if __name__ == "__main__":
    main()
