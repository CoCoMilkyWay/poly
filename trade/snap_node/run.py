import subprocess
import time
import websocket
import json
import os

DATA_DIR = os.path.expanduser("~/polygon-node/data")

BOR_CMD = [
    "bor",
    "--datadir", DATA_DIR,

    # snap / pbss sync
    "--syncmode", "snap",

    # networking
    "--maxpeers", "150",
    "--port", "30303",

    # txpool
    "--txpool.globalslots", "50000",
    "--txpool.globalqueue", "50000",

    # rpc
    "--http",
    "--http.addr", "0.0.0.0",
    "--http.port", "8545",
    "--http.api", "eth,net,web3,txpool",

    # ws
    "--ws",
    "--ws.addr", "0.0.0.0",
    "--ws.port", "8546",
    "--ws.api", "eth,net,web3,txpool",

    # performance
    "--cache", "4096",

    # logs
    "--verbosity", "3",
]


def start_bor():
    print("Starting bor...")
    return subprocess.Popen(BOR_CMD)


def wait_rpc():
    print("Waiting RPC...")
    time.sleep(10)


def listen_pending():
    print("Connecting WS...")

    ws = websocket.create_connection("ws://127.0.0.1:8546")

    sub = {
        "id": 1,
        "method": "eth_subscribe",
        "params": ["newPendingTransactions"]
    }

    ws.send(json.dumps(sub))

    while True:
        msg = ws.recv()
        print(msg)


if __name__ == "__main__":
    bor = start_bor()

    wait_rpc()

    listen_pending()