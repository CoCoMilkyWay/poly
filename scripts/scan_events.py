#!/usr/bin/env python3
"""扫描 Polymarket 合约历史事件统计（动态终端刷新）"""

"""
  Polymarket 事件扫描  (head=83,000,000)

  块进度:   62,889,999 / 83,000,000  (75.8%)  -  速度: 458 blk/s  ETA: 731.8 min

  合约                事件                          总计       首block       末block       2020       2021       2022       2023       2024       2025       2026
  ---------------------------------------------------------------------------------------------------------------------------------------------------------------
  ConditionalTokens   TransferSingle          15,922,185     4,028,711    62,889,999          0  1,319,462  1,457,584    493,197 12,651,942          0          0
  ConditionalTokens   TransferBatch           12,080,571     4,028,608    62,889,999          0  1,197,332  1,834,233    379,225  8,669,781          0          0
  ConditionalTokens   ConditionPreparation        23,270     4,027,499    62,887,016          0      1,012      7,388      4,236     10,634          0          0
  ConditionalTokens   ConditionResolution         17,159     6,205,069    62,889,281          0        743      3,734      3,797      8,885          0          0
  ConditionalTokens   PositionSplit            5,692,159     4,028,608    62,889,999          0    614,470    970,174    231,322  3,876,193          0          0
  ConditionalTokens   PositionsMerge           2,500,809     4,028,724    62,889,998          0    488,700    658,156    114,156  1,239,797          0          0
  ConditionalTokens   PayoutRedemption         1,284,556     6,233,711    62,889,985          0    331,252    247,416     70,202    635,686          0          0
  FPMMFactory         FPMMCreation                13,642     4,027,846    62,760,657          0        981      7,230      4,240      1,191          0          0
  FPMM                FPMMBuy                  1,068,771     4,028,711    62,889,297          0    264,187    640,882    161,829      1,873          0          0
  FPMM                FPMMSell                 1,064,706     4,028,724    62,876,506          0    448,241    540,392     75,316        757          0          0
  FPMM                FPMMFundingAdded           186,350     4,028,608    61,759,915          0     49,219    108,232     28,689        210          0          0
  FPMM                FPMMFundingRemoved         168,320     4,350,124    62,752,190          0     44,937     97,533     25,691        159          0          0
  CTFExchange         OrderFilled              3,215,134    35,896,869    62,889,996          0          0          0    245,770  2,969,364          0          0
  CTFExchange         OrdersMatched            1,324,672    35,896,869    62,889,996          0          0          0    100,553  1,224,119          0          0
  CTFExchange         TokenRegistered             16,372    35,887,522    62,887,050          0          0          0      6,738      9,634          0          0
  NegRiskCTFExchange  OrderFilled              9,044,559    51,408,357    62,889,999          0          0          0          0  9,044,559          0          0
  NegRiskCTFExchange  OrdersMatched            4,105,482    51,408,357    62,889,999          0          0          0          0  4,105,482          0          0
  NegRiskCTFExchange  TokenRegistered             11,444    51,405,773    62,881,562          0          0          0          0     11,444          0          0
  NegRiskAdapter      MarketPrepared                 947    50,748,168    62,881,479          0          0          0          0        947          0          0
  NegRiskAdapter      QuestionPrepared             5,732    50,750,368    62,881,531          0          0          0          0      5,732          0          0
  NegRiskAdapter      PositionsConverted         119,043    50,861,311    62,889,978          0          0          0          0    119,043          0          0
  NegRiskAdapter      OutcomeReported              4,239    51,868,332    62,889,281          0          0          0          0      4,239          0          0
"""

from pathlib import Path
import urllib.request
import time
import sys
import datetime
import json

_cfg = json.loads((Path(__file__).parent.parent / "config.json").read_text())
_node = next(n for n in _cfg["rpc_nodes"] if n["name"] == _cfg["active_rpc"])
RPC = _node["url"]
CHUNK = _node["chunk"]
HEAD = 83_000_000  # 固定 head

BLOCK_TIME = 2  # 秒/block
UTC = datetime.timezone.utc
NOW_TS = int(datetime.datetime.now(UTC).timestamp())
# HEAD 对应当前时间, 往前推算
BLOCK0_TS = NOW_TS - HEAD * BLOCK_TIME
YEAR0 = datetime.datetime.fromtimestamp(BLOCK0_TS, UTC).year
YEARS = list(range(YEAR0, datetime.datetime.now(UTC).year + 1))


def block_to_year(blknum):
    ts = NOW_TS - (HEAD - blknum) * BLOCK_TIME
    return datetime.datetime.fromtimestamp(ts, UTC).year


# ── 合约地址 ──────────────────────────────────────────────────
CONTRACTS = {
    "ConditionalTokens":   "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045",
    "FPMMFactory":         "0x8B9805A2f595B6705e74F7310829f2d299D21522",
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
    # FPMMFactory
    "0x92e0912d3d7f3192cad5c7ae3b47fb97f9c465c1dd12a5c24fd901ddb3905f43": ("FPMMFactory", "FPMMCreation"),
    # FPMM (动态合约)
    "0x4f62630f51608fc8a7603a9391a5101e58bd7c276139366fc107dc3b67c3dcf8": ("FPMM", "FPMMBuy"),
    "0xadcf2a240ed9300d681d9a3f5382b6c1beed1b7e46643e0c7b42cbe6e2d766b4": ("FPMM", "FPMMSell"),
    "0xec2dc3e5a3bb9aa0a1deb905d2bd23640d07f107e6ceb484024501aad964a951": ("FPMM", "FPMMFundingAdded"),
    "0x8b4b2c8ebd04c47fc8bce136a85df9b93fcb1f47c8aa296457d4391519d190e7": ("FPMM", "FPMMFundingRemoved"),
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
counts = {}   # (contract, event) -> int
first_blk = {}   # (contract, event) -> int
last_blk = {}   # (contract, event) -> int
yearly = {}   # (contract, event, year) -> int
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
    lines.append(
        f"  块进度: {cur_block:>12,} / {head:,}  ({pct:.1f}%)  -  速度: {rate:,.0f} blk/s  ETA: {eta/60:.1f} min")
    lines.append("")

    order = [
        ("ConditionalTokens", "TransferSingle"),
        ("ConditionalTokens", "TransferBatch"),
        ("ConditionalTokens", "ConditionPreparation"),
        ("ConditionalTokens", "ConditionResolution"),
        ("ConditionalTokens", "PositionSplit"),
        ("ConditionalTokens", "PositionsMerge"),
        ("ConditionalTokens", "PayoutRedemption"),
        ("FPMMFactory", "FPMMCreation"),
        ("FPMM", "FPMMBuy"),
        ("FPMM", "FPMMSell"),
        ("FPMM", "FPMMFundingAdded"),
        ("FPMM", "FPMMFundingRemoved"),
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
    year_hdrs = "".join(f"{y:>11}" for y in YEARS)
    lines.append(
        f"  {'合约':<18}  {'事件':<20}  {'总计':>12}  {'首block':>12}  {'末block':>12}{year_hdrs}")
    lines.append("  " + "-" * (18+2+20+2+12+2+12+2+12 + len(YEARS)*11))
    for key in order:
        cnt = counts.get(key, 0)
        fb = f"{first_blk[key]:>12,}" if key in first_blk else "-".center(12)
        lb = f"{last_blk[key]:>12,}" if key in last_blk else "-".center(12)
        ycols = "".join(
            f"{yearly.get((key[0], key[1], y), 0):>11,}" for y in YEARS)
        lines.append(
            f"  {key[0]:<18}  {key[1]:<20}  {cnt:>12,}  {fb}  {lb}{ycols}")

    for line in lines:
        sys.stdout.write(line + "\n")
    sys.stdout.flush()
    LINES = len(lines)


def main():
    head = HEAD
    print(f"\n  Polymarket 事件扫描  (head={head:,})\n")

    ct_topics = [t for t, (c, _) in EVENTS.items() if c == "ConditionalTokens"]
    fpmm_factory_topics = [
        t for t, (c, _) in EVENTS.items() if c == "FPMMFactory"]
    fpmm_topics = [t for t, (c, _) in EVENTS.items() if c == "FPMM"]
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
                              "address": CONTRACTS["FPMMFactory"], "topics": [fpmm_factory_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "topics": [fpmm_topics]}]),  # 不指定address
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "address": CONTRACTS["CTFExchange"], "topics": [ex_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "address": CONTRACTS["NegRiskCTFExchange"], "topics": [ex_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                              "address": CONTRACTS["NegRiskAdapter"], "topics": [nra_topics]}]),
        ])
        process_logs(results[0], "ConditionalTokens")
        process_logs(results[1], "FPMMFactory")
        process_logs(results[2], "FPMM")
        process_logs(results[3], "CTFExchange")
        process_logs(results[4], "NegRiskCTFExchange")
        process_logs(results[5], "NegRiskAdapter")

        render(end, head, time.time())
        cur = end + 1

    elapsed = time.time() - start_time
    print(f"\n  扫描完成, 耗时 {elapsed:.0f}s\n")


if __name__ == "__main__":
    main()
