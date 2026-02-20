#!/usr/bin/env python3
"""扫描 Polymarket 合约历史事件统计（动态终端刷新）"""

"""
Polymarket 事件扫描  (head=83,000,000)

  块进度:   68,281,999 / 83,000,000  (82.3%)  -  速度: 355 blk/s  ETA: 690.6 min

  合约                  事件                              总计        首block        末block      2020      2021      2022      2023      2024      2025      2026
  --------------------------------------------------------------------------------------------------------------------------------------------------------
  ConditionalTokens   TransferSingle          84,545,926     4,028,711    68,281,999         0 1,323,929 1,455,369   492,77439,667,70441,606,150         0
  ConditionalTokens   TransferBatch           54,120,488     4,028,608    68,281,999         0 1,202,775 1,832,003   376,67525,391,33625,317,699         0
  ConditionalTokens   ConditionPreparation        37,482     4,027,499    68,278,448         0     1,029     7,392     4,218    15,364     9,479         0
  ConditionalTokens   ConditionResolution         30,040     6,205,069    68,278,044         0       750     3,733     3,805    13,606     8,146         0
  ConditionalTokens   PositionSplit           21,647,506     4,028,608    68,281,999         0   617,762   968,524   230,22710,177,708 9,653,285         0
  ConditionalTokens   PositionsMerge          10,311,617     4,028,724    68,281,998         0   490,210   657,656   113,262 4,172,673 4,877,816         0
  ConditionalTokens   PayoutRedemption         4,987,113     6,233,711    68,281,991         0   332,902   246,192    69,917 1,747,897 2,590,205         0
  CTFExchange         OrderFilled             19,759,589    35,896,869    68,281,999         0         0         0   247,475 7,633,61911,878,495         0
  CTFExchange         OrdersMatched            8,334,148    35,896,869    68,281,999         0         0         0   101,248 3,233,641 4,999,259         0
  CTFExchange         TokenRegistered             31,180    35,887,522    68,278,152         0         0         0     6,744    14,526     9,910         0
  NegRiskCTFExchange  OrderFilled             57,493,076    51,408,357    68,281,999         0         0         0         030,359,91227,133,164         0
  NegRiskCTFExchange  OrdersMatched           26,593,833    51,408,357    68,281,999         0         0         0         014,092,05212,501,781         0
  NegRiskCTFExchange  TokenRegistered             25,004    51,405,773    68,278,481         0         0         0         0    16,042     8,962         0
  NegRiskAdapter      MarketPrepared                   0       -             -               0         0         0         0         0         0         0
  NegRiskAdapter      QuestionPrepared                 0       -             -               0         0         0         0         0         0         0
  NegRiskAdapter      PositionsConverted               0       -             -               0         0         0         0         0         0         0
  NegRiskAdapter      OutcomeReported                  0       -             -               0         0         0         0         0         0         0
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
    "0xf059ab16d1ca60e123eab60e3c02b68faf060347c701a5d14885a8e1def7b3a8": ("NegRiskAdapter", "MarketPrepared"),
    "0xaac410f87d423a922a7b226ac68f0c2eaf5bf6d15e644ac0758c7f96e2c253f7": ("NegRiskAdapter", "QuestionPrepared"),
    "0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e": ("NegRiskAdapter", "PositionsConverted"),
    "0x9e9fa7fd355160bd4cd3f22d4333519354beff1f5689bde87f2c5e63d8d484b2": ("NegRiskAdapter", "OutcomeReported"),
}

# ── 状态 ──────────────────────────────────────────────────────
counts    = {}   # (contract, event) -> int
first_blk = {}   # (contract, event) -> int
last_blk  = {}   # (contract, event) -> int
yearly     = {}   # (contract, event, year) -> int
recent_times = []  # 最近20次查询的 (block, time) 用于计算滑动平均速度

def rpc_batch(calls, timeout=30):
    batch = [{"jsonrpc": "2.0", "method": m, "params": p, "id": i}
             for i, (m, p) in enumerate(calls)]
    req = json.dumps(batch).encode()
    with urllib.request.urlopen(
        urllib.request.Request(RPC, req, {"Content-Type": "application/json"}),
        timeout=timeout
    ) as r:
        responses = json.loads(r.read())
    responses.sort(key=lambda x: x["id"])
    for resp in responses:
        assert "result" in resp, f"RPC error: {resp}"
    return [resp["result"] for resp in responses]

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

        results = rpc_batch([
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "address": CONTRACTS["ConditionalTokens"], "topics": [ct_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "address": CONTRACTS["CTFExchange"], "topics": [ex_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "address": CONTRACTS["NegRiskCTFExchange"], "topics": [ex_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "address": CONTRACTS["NegRiskAdapter"], "topics": [nra_topics]}]),
        ])
        process_logs(results[0], "ConditionalTokens")
        process_logs(results[1], "CTFExchange")
        process_logs(results[2], "NegRiskCTFExchange")
        process_logs(results[3], "NegRiskAdapter")

        render(end, head, time.time())
        cur = end + 1

    elapsed = time.time() - start_time
    print(f"\n  扫描完成，耗时 {elapsed:.0f}s\n")

if __name__ == "__main__":
    main()
