#!/usr/bin/env python3
"""从本地RPC节点dump Polymarket事件到CSV文件"""

"""
chuyin@chuyin:~/work/poly$ /bin/python3 /home/chuyin/work/poly/scripts/dump_events.py
Head: 75,000,000
查询范围: 74,990,000 - 75,000,000 (10,000 blocks)


==================================================
开始查询 Erigon: http://127.0.0.1:8545... (chunk=2000)
==================================================

  [Erigon] Block: 75,000,000 - 75,000,000  (100.0%)  464 blk/s  ETA: 0s

  事件                             数量
  -----------------------------------
  TransferSingle             79,123
  TransferBatch              41,957
  OrderFilled                69,698
  OrdersMatched              29,024
  TokenRegistered                36
  PositionSplit              28,069
  PositionsMerge              4,249
  PayoutRedemption           11,950
  ConditionPreparation           66
  ConditionResolution            94
  MarketPrepared                  0
  QuestionPrepared                0
  PositionsConverted              0
  OutcomeReported                 0

[Erigon] 完成，耗时 21.5s
[Erigon] 输出目录: /home/chuyin/work/poly/scripts/events/Erigon/

==================================================
开始查询 dRPC: https://lb.drpc.org/ogrpc?network=polygon&dkey=Aj9... (chunk=1000)
==================================================

  [dRPC] Block: 75,000,000 - 75,000,000  (100.0%)  159 blk/s  ETA: 0s

  事件                             数量
  -----------------------------------
  TransferSingle             79,123
  TransferBatch              41,957
  OrderFilled                69,698
  OrdersMatched              29,024
  TokenRegistered                36
  PositionSplit              28,069
  PositionsMerge              4,249
  PayoutRedemption           11,950
  ConditionPreparation           66
  ConditionResolution            94
  MarketPrepared                  0
  QuestionPrepared                0
  PositionsConverted              0
  OutcomeReported                 0

[dRPC] 完成，耗时 62.8s
[dRPC] 输出目录: /home/chuyin/work/poly/scripts/events/dRPC/

==================================================
对比结果:
==================================================
  TransferSingle         79,123 vs 79,123 ✓
  TransferBatch          41,957 vs 41,957 ✓
  ConditionPreparation   66 vs 66 ✓
  ConditionResolution    94 vs 94 ✓
  PositionSplit          28,069 vs 28,069 ✓
  PositionsMerge         4,249 vs 4,249 ✓
  PayoutRedemption       11,950 vs 11,950 ✓
  OrderFilled            69,698 vs 69,698 ✓
  OrdersMatched          29,024 vs 29,024 ✓
  TokenRegistered        36 vs 36 ✓
  MarketPrepared         0 vs 0 ✓
  QuestionPrepared       0 vs 0 ✓
  PositionsConverted     0 vs 0 ✓
  OutcomeReported        0 vs 0 ✓

总体: 全部一致 ✓
"""

import urllib.request
import time
import sys
import os
import csv
import json

DRPC_API_KEY = "Aj9Y8zpk1EVEkcL8w3Z1mGcHAZE3DdUR8biw-uF7NYYO"
# https://drpc.org/dashboard/3f58f33a-64d4-4544-91c2-fcc376759867/keys

RPC_ENDPOINTS = {
    "Erigon": ("http://127.0.0.1:8545", 2000),
    "dRPC":   (f"https://lb.drpc.org/ogrpc?network=polygon&dkey={DRPC_API_KEY}", 1000),
}
BASE_OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "events")
MAX_PER_EVENT = 1000

# ── 合约地址 ──────────────────────────────────────────────────
CONTRACTS = {
    "ConditionalTokens":   "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045",
    "CTFExchange":         "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E",
    "NegRiskCTFExchange":  "0xC5d563A36AE78145C45a50134d48A1215220f80a",
    "NegRiskAdapter":      "0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296",
}

# ── 事件 topic0 ──────────────────────────────────────────────────
TOPICS = {
    # ConditionalTokens
    "TransferSingle":        "0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62",
    "TransferBatch":         "0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb",
    "ConditionPreparation":  "0xab3760c3bd2bb38b5bcf54dc79802ed67338b4cf29f3054ded67ed24661e4177",
    "ConditionResolution":   "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894",
    "PositionSplit":         "0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298",
    "PositionsMerge":        "0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca",
    "PayoutRedemption":      "0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d",
    # CTFExchange / NegRiskCTFExchange
    "OrderFilled":           "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6",
    "OrdersMatched":         "0x63bf4d16b7fa898ef4c4b2b6d90fd201e9c56313b65638af6088d149d2ce956c",
    "TokenRegistered":       "0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d",
    # NegRiskAdapter
    "MarketPrepared":        "0xaac410f87d423a922a7b226ac68f5ba267c3a70a6eb6db3a0d11a8027dc303b0",
    "QuestionPrepared":      "0xf059ab16d1ca60e123eab60e3c02d4ab3e7c267b7dbb5361a38e7fbc7d5e5a05",
    "PositionsConverted":    "0xbbed930dbfb7907ae2d60ddf78341a2c1f8cfe1e6f3b4e74c8c7b7a0be5e9e16",
    "OutcomeReported":       "0xd5a3c84c3953ba5cf608be6e2bd08447c931838ab29dc6543f5e5575d1574e8f",
}

# topic0 -> event name
TOPIC_TO_NAME = {v: k for k, v in TOPICS.items()}

# ── CSV 字段定义 ──────────────────────────────────────────────────
CSV_FIELDS = {
    "TransferSingle": [
        "block_number", "tx_hash", "log_index",
        "operator", "from", "to", "id", "value"
    ],
    "TransferBatch": [
        "block_number", "tx_hash", "log_index",
        "operator", "from", "to", "ids", "values"
    ],
    "ConditionPreparation": [
        "block_number", "tx_hash", "log_index",
        "conditionId", "oracle", "questionId", "outcomeSlotCount"
    ],
    "ConditionResolution": [
        "block_number", "tx_hash", "log_index",
        "conditionId", "oracle", "questionId", "outcomeSlotCount", "payoutNumerators"
    ],
    "PositionSplit": [
        "block_number", "tx_hash", "log_index",
        "stakeholder", "collateralToken", "parentCollectionId", "conditionId", "partition", "amount"
    ],
    "PositionsMerge": [
        "block_number", "tx_hash", "log_index",
        "stakeholder", "collateralToken", "parentCollectionId", "conditionId", "partition", "amount"
    ],
    "PayoutRedemption": [
        "block_number", "tx_hash", "log_index",
        "redeemer", "collateralToken", "parentCollectionId", "conditionId", "indexSets", "payout"
    ],
    "OrderFilled": [
        "block_number", "tx_hash", "log_index", "exchange",
        "orderHash", "maker", "taker", "makerAssetId", "takerAssetId",
        "makerAmountFilled", "takerAmountFilled", "fee"
    ],
    "OrdersMatched": [
        "block_number", "tx_hash", "log_index", "exchange",
        "takerOrderHash", "takerOrderMaker", "makerAssetId", "takerAssetId",
        "makerAmountFilled", "takerAmountFilled"
    ],
    "TokenRegistered": [
        "block_number", "tx_hash", "log_index", "exchange",
        "token0", "token1", "conditionId"
    ],
    "MarketPrepared": [
        "block_number", "tx_hash", "log_index",
        "marketId", "oracle", "feeBips", "data"
    ],
    "QuestionPrepared": [
        "block_number", "tx_hash", "log_index",
        "marketId", "questionId", "questionIndex", "data"
    ],
    "PositionsConverted": [
        "block_number", "tx_hash", "log_index",
        "stakeholder", "marketId", "indexSet", "amount"
    ],
    "OutcomeReported": [
        "block_number", "tx_hash", "log_index",
        "marketId", "questionId", "outcome"
    ],
}


def rpc_call(rpc_url, method, params, timeout=30):
    req = json.dumps({"jsonrpc": "2.0", "method": method,
                     "params": params, "id": 1}).encode()
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
        "User-Agent": "Mozilla/5.0",
    }
    with urllib.request.urlopen(
        urllib.request.Request(rpc_url, req, headers),
        timeout=timeout
    ) as r:
        resp = json.loads(r.read())
    assert "result" in resp, f"RPC error: {resp}"
    return resp["result"]


def rpc_batch(rpc_url, calls, timeout=30):
    """批量RPC调用，calls = [(method, params), ...]，返回结果列表"""
    batch = [{"jsonrpc": "2.0", "method": m, "params": p, "id": i}
             for i, (m, p) in enumerate(calls)]
    req = json.dumps(batch).encode()
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
        "User-Agent": "Mozilla/5.0",
    }
    with urllib.request.urlopen(
        urllib.request.Request(rpc_url, req, headers),
        timeout=timeout
    ) as r:
        responses = json.loads(r.read())
    responses.sort(key=lambda x: x["id"])
    for resp in responses:
        assert "result" in resp, f"RPC error: {resp}"
    return [resp["result"] for resp in responses]


def get_head(rpc_url):
    return int(rpc_call(rpc_url, "eth_blockNumber", []), 16)


def decode_uint(hex_str):
    return int(hex_str, 16) if hex_str else 0


def decode_address(hex_str):
    return "0x" + hex_str[-40:] if hex_str else "0x0"


def decode_bytes32(hex_str):
    if not hex_str:
        return "0x" + "0" * 64
    return hex_str if hex_str.startswith("0x") else "0x" + hex_str


def decode_uint_array(data, offset):
    """解码动态uint256数组"""
    arr_offset = decode_uint(data[offset:offset+64])
    arr_start = arr_offset * 2
    length = decode_uint(data[arr_start:arr_start+64])
    result = []
    for i in range(length):
        start = arr_start + 64 + i * 64
        result.append(decode_uint(data[start:start+64]))
    return result


def parse_log(log, exchange_name=None):
    """解析单个log，返回 (event_name, row_dict)"""
    topics = log.get("topics", [])
    if not topics:
        return None, None

    topic0 = topics[0].lower()
    event_name = TOPIC_TO_NAME.get(topic0)
    if not event_name:
        return None, None

    block_number = decode_uint(log["blockNumber"])
    tx_hash = log["transactionHash"]
    log_index = decode_uint(log["logIndex"])
    data = log.get("data", "0x")[2:]  # 去掉0x前缀

    row = {
        "block_number": block_number,
        "tx_hash": tx_hash,
        "log_index": log_index,
    }

    if event_name == "TransferSingle":
        row["operator"] = decode_address(topics[1])
        row["from"] = decode_address(topics[2])
        row["to"] = decode_address(topics[3])
        row["id"] = decode_uint(data[0:64])
        row["value"] = decode_uint(data[64:128])

    elif event_name == "TransferBatch":
        row["operator"] = decode_address(topics[1])
        row["from"] = decode_address(topics[2])
        row["to"] = decode_address(topics[3])
        row["ids"] = decode_uint_array(data, 0)
        row["values"] = decode_uint_array(data, 64)

    elif event_name == "ConditionPreparation":
        row["conditionId"] = decode_bytes32(topics[1])
        row["oracle"] = decode_address(topics[2])
        row["questionId"] = decode_bytes32(topics[3])
        row["outcomeSlotCount"] = decode_uint(data[0:64])

    elif event_name == "ConditionResolution":
        row["conditionId"] = decode_bytes32(topics[1])
        row["oracle"] = decode_address(topics[2])
        row["questionId"] = decode_bytes32(topics[3])
        row["outcomeSlotCount"] = decode_uint(data[0:64])
        row["payoutNumerators"] = decode_uint_array(data, 64)

    elif event_name == "PositionSplit":
        row["stakeholder"] = decode_address(topics[1])
        row["parentCollectionId"] = decode_bytes32(topics[2])
        row["conditionId"] = decode_bytes32(topics[3])
        row["collateralToken"] = decode_address(data[0:64])
        row["partition"] = decode_uint_array(data, 64)
        row["amount"] = decode_uint(data[128:192])

    elif event_name == "PositionsMerge":
        row["stakeholder"] = decode_address(topics[1])
        row["parentCollectionId"] = decode_bytes32(topics[2])
        row["conditionId"] = decode_bytes32(topics[3])
        row["collateralToken"] = decode_address(data[0:64])
        row["partition"] = decode_uint_array(data, 64)
        row["amount"] = decode_uint(data[128:192])

    elif event_name == "PayoutRedemption":
        row["redeemer"] = decode_address(topics[1])
        row["collateralToken"] = decode_address(topics[2])
        row["parentCollectionId"] = decode_bytes32(topics[3])
        row["conditionId"] = decode_bytes32(data[0:64])
        row["indexSets"] = decode_uint_array(data, 64)
        row["payout"] = decode_uint(data[128:192])

    elif event_name == "OrderFilled":
        row["exchange"] = exchange_name or "unknown"
        row["orderHash"] = decode_bytes32(topics[1])
        row["maker"] = decode_address(topics[2])
        row["taker"] = decode_address(topics[3])
        row["makerAssetId"] = decode_uint(data[0:64])
        row["takerAssetId"] = decode_uint(data[64:128])
        row["makerAmountFilled"] = decode_uint(data[128:192])
        row["takerAmountFilled"] = decode_uint(data[192:256])
        row["fee"] = decode_uint(data[256:320])

    elif event_name == "OrdersMatched":
        row["exchange"] = exchange_name or "unknown"
        row["takerOrderHash"] = decode_bytes32(topics[1])
        row["takerOrderMaker"] = decode_address(topics[2])
        row["makerAssetId"] = decode_uint(data[0:64])
        row["takerAssetId"] = decode_uint(data[64:128])
        row["makerAmountFilled"] = decode_uint(data[128:192])
        row["takerAmountFilled"] = decode_uint(data[192:256])

    elif event_name == "TokenRegistered":
        row["exchange"] = exchange_name or "unknown"
        row["token0"] = decode_uint(topics[1])
        row["token1"] = decode_uint(topics[2])
        row["conditionId"] = decode_bytes32(topics[3])

    elif event_name == "MarketPrepared":
        row["marketId"] = decode_bytes32(topics[1])
        row["oracle"] = decode_address(data[0:64])
        row["feeBips"] = decode_uint(data[64:128])
        row["data"] = "0x" + data[128:]

    elif event_name == "QuestionPrepared":
        row["marketId"] = decode_bytes32(topics[1])
        row["questionId"] = decode_bytes32(topics[2])
        row["questionIndex"] = decode_uint(data[0:64])
        row["data"] = "0x" + data[64:]

    elif event_name == "PositionsConverted":
        row["stakeholder"] = decode_address(topics[1])
        row["marketId"] = decode_bytes32(topics[2])
        row["indexSet"] = decode_uint(topics[3])  # indexed
        row["amount"] = decode_uint(data[0:64])

    elif event_name == "OutcomeReported":
        row["marketId"] = decode_bytes32(topics[1])
        row["questionId"] = decode_bytes32(topics[2])
        row["outcome"] = decode_uint(data[0:64]) != 0  # bool

    return event_name, row


def dump_node(node_name, rpc_url, chunk, output_dir, head, start_block, num_blocks):
    """dump单个节点的数据"""
    os.makedirs(output_dir, exist_ok=True)

    writers = {}
    files = {}
    for event_name, fields in CSV_FIELDS.items():
        path = os.path.join(output_dir, f"{event_name}.csv")
        f = open(path, "w", newline="")
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        writers[event_name] = w
        files[event_name] = f

    counts = {name: 0 for name in CSV_FIELDS}
    written = {name: 0 for name in CSV_FIELDS}

    cur = start_block
    start_time = time.time()
    lines_printed = 0

    def render_progress(cur_block, end_block):
        nonlocal lines_printed
        if lines_printed:
            sys.stdout.write(f"\033[{lines_printed}A\033[J")

        elapsed = time.time() - start_time
        pct = (end_block - start_block) / num_blocks * 100
        rate = (end_block - start_block) / elapsed if elapsed > 0 else 0
        eta = (head - end_block) / rate if rate > 0 else 0

        lines = []
        lines.append(
            f"  [{node_name}] Block: {cur_block:,} - {end_block:,}  ({pct:.1f}%)  {rate:,.0f} blk/s  ETA: {eta:.0f}s")
        lines.append("")
        lines.append(f"  {'事件':<22} {'数量':>10}")
        lines.append("  " + "-" * 35)

        display_order = [
            "TransferSingle", "TransferBatch",
            "OrderFilled", "OrdersMatched", "TokenRegistered",
            "PositionSplit", "PositionsMerge", "PayoutRedemption",
            "ConditionPreparation", "ConditionResolution",
            "MarketPrepared", "QuestionPrepared", "PositionsConverted", "OutcomeReported",
        ]
        for name in display_order:
            cnt = counts.get(name, 0)
            lines.append(f"  {name:<22} {cnt:>10,}")

        for line in lines:
            sys.stdout.write(line + "\n")
        sys.stdout.flush()
        lines_printed = len(lines)

    def flush_all():
        for f in files.values():
            f.flush()

    def should_write(event_name):
        return written[event_name] < MAX_PER_EVENT

    ct_topics = [
        TOPICS["TransferSingle"],
        TOPICS["TransferBatch"],
        TOPICS["ConditionPreparation"],
        TOPICS["ConditionResolution"],
        TOPICS["PositionSplit"],
        TOPICS["PositionsMerge"],
        TOPICS["PayoutRedemption"],
    ]
    ex_topics = [
        TOPICS["OrderFilled"],
        TOPICS["OrdersMatched"],
        TOPICS["TokenRegistered"],
    ]
    nra_topics = [
        TOPICS["MarketPrepared"],
        TOPICS["QuestionPrepared"],
        TOPICS["PositionsConverted"],
        TOPICS["OutcomeReported"],
    ]

    while cur <= head:
        end = min(cur + chunk - 1, head)
        all_logs = []

        results = rpc_batch(rpc_url, [
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                             "address": CONTRACTS["ConditionalTokens"], "topics": [ct_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                             "address": CONTRACTS["CTFExchange"], "topics": [ex_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                             "address": CONTRACTS["NegRiskCTFExchange"], "topics": [ex_topics]}]),
            ("eth_getLogs", [{"fromBlock": hex(cur), "toBlock": hex(end),
                             "address": CONTRACTS["NegRiskAdapter"], "topics": [nra_topics]}]),
        ])

        all_logs.extend((log, None) for log in results[0])
        all_logs.extend((log, "CTFExchange") for log in results[1])
        all_logs.extend((log, "NegRiskCTFExchange") for log in results[2])
        all_logs.extend((log, None) for log in results[3])

        for log, exchange_name in all_logs:
            event_name, row = parse_log(log, exchange_name)
            if event_name and row:
                counts[event_name] += 1
                if should_write(event_name):
                    writers[event_name].writerow(row)
                    written[event_name] += 1

        flush_all()
        render_progress(cur, end)
        cur = end + 1

    for f in files.values():
        f.close()

    elapsed = time.time() - start_time
    print(f"\n[{node_name}] 完成，耗时 {elapsed:.1f}s")
    print(f"[{node_name}] 输出目录: {output_dir}/")
    return counts


def main():
    # head = get_head()
    head = 75_000_000
    num_blocks = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    start_block = head - num_blocks

    print(f"Head: {head:,}")
    print(f"查询范围: {start_block:,} - {head:,} ({num_blocks:,} blocks)")
    print()

    results = {}
    for node_name, (rpc_url, chunk) in RPC_ENDPOINTS.items():
        print(f"\n{'='*50}")
        print(f"开始查询 {node_name}: {rpc_url[:50]}... (chunk={chunk})")
        print(f"{'='*50}\n")
        output_dir = os.path.join(BASE_OUTPUT_DIR, node_name)
        results[node_name] = dump_node(
            node_name, rpc_url, chunk, output_dir, head, start_block, num_blocks)

    print(f"\n{'='*50}")
    print("对比结果:")
    print(f"{'='*50}")
    nodes = list(results.keys())
    all_match = True
    for event_name in CSV_FIELDS:
        counts = [results[n][event_name] for n in nodes]
        match = "✓" if len(set(counts)) == 1 else "✗"
        if match == "✗":
            all_match = False
        print(f"  {event_name:<22} {' vs '.join(f'{c:,}' for c in counts)} {match}")
    print(f"\n总体: {'全部一致 ✓' if all_match else '存在差异 ✗'}")


if __name__ == "__main__":
    main()
