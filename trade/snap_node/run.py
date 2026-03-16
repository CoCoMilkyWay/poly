#!/usr/bin/env python3
"""
Polygon Pending Transaction Monitor

Usage:
  python3 run.py                    # 监听所有 pending tx
  python3 run.py 0x1234... 0x5678...  # 监听特定地址
"""

import websocket
import json
import sys
import time
import urllib.request

WS_URL = "ws://127.0.0.1:8546"
RPC_URL = "http://127.0.0.1:8545"


def check_sync():
    """检查同步状态"""
    try:
        req = urllib.request.Request(
            RPC_URL,
            data=json.dumps({"jsonrpc": "2.0", "method": "eth_syncing", "params": [], "id": 1}).encode(),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req, timeout=5) as resp:
            result = json.loads(resp.read()).get("result", False)
            if result and result is not True:
                current = int(result["currentBlock"], 16)
                highest = int(result["highestBlock"], 16)
                print(f"Syncing: {current:,} / {highest:,} ({100*current/highest:.2f}%)")
            else:
                print("Node synced!")
    except Exception as e:
        print(f"RPC error: {e}")
        sys.exit(1)


def get_tx(tx_hash):
    """获取交易详情"""
    req = urllib.request.Request(
        RPC_URL,
        data=json.dumps({
            "jsonrpc": "2.0",
            "method": "eth_getTransactionByHash",
            "params": [tx_hash],
            "id": 1
        }).encode(),
        headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.loads(resp.read()).get("result")


def listen(watch_addresses=None):
    """监听 pending transactions"""
    watch_set = set(a.lower() for a in watch_addresses) if watch_addresses else None
    
    print(f"Connecting to {WS_URL}...")
    ws = websocket.create_connection(WS_URL)
    
    ws.send(json.dumps({
        "id": 1,
        "jsonrpc": "2.0",
        "method": "eth_subscribe",
        "params": ["newPendingTransactions"]
    }))
    print(f"Subscribed: {ws.recv()}")
    
    if watch_set:
        print(f"Watching addresses: {watch_addresses}")
    print("-" * 60)
    
    count = 0
    while True:
        try:
            msg = json.loads(ws.recv())
            if "params" not in msg:
                continue
                
            tx_hash = msg["params"]["result"]
            count += 1
            
            # 过滤地址
            if watch_set:
                tx = get_tx(tx_hash)
                if not tx:
                    continue
                to_addr = (tx.get("to") or "").lower()
                from_addr = (tx.get("from") or "").lower()
                if to_addr not in watch_set and from_addr not in watch_set:
                    continue
                print(f"[{count}] {tx_hash} | {from_addr[:10]}... -> {to_addr[:10]}...")
            else:
                print(f"[{count}] {tx_hash}")
                
        except Exception as e:
            print(f"Error: {e}, reconnecting...")
            time.sleep(1)
            ws = websocket.create_connection(WS_URL)
            ws.send(json.dumps({
                "id": 1, "jsonrpc": "2.0",
                "method": "eth_subscribe",
                "params": ["newPendingTransactions"]
            }))


if __name__ == "__main__":
    check_sync()
    addresses = sys.argv[1:] if len(sys.argv) > 1 else None
    listen(addresses)
