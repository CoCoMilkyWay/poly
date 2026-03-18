#!/usr/bin/env python3

from __future__ import annotations

import base64
import hashlib
import json
import os
import secrets
import select
import socket
import ssl
import struct
import threading
import time
from collections import defaultdict, deque
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from decimal import Decimal, getcontext
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse
from urllib.request import Request, urlopen

from rebuild import (
    DEFAULT_API_KEY,
    MAX_WORKERS,
    META_QUERY,
    PNL_SUBGRAPH_ID,
    POLYMARKET_SUBGRAPH_ID,
    USER_QUERY_BATCH_LIMIT,
    UserPosition,
    build_cached_condition_index,
    build_cached_token_data,
    chunked,
    fetch_conditions_batch,
    fetch_market_data_batch,
    fetch_user_positions_shard,
    format_decimal,
    gql,
    group_positions_by_user,
    load_addresses,
    load_previous_output,
    normalize_address,
)

getcontext().prec = 50

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
CONFIG_FILE = ROOT_DIR / "config.json"
EXAMPLE_CONFIG_FILE = ROOT_DIR / "example.json"
OUTPUT_JSON_FILE = SCRIPT_DIR / "tracker.json"
OUTPUT_JSON_TMP_FILE = SCRIPT_DIR / "tracker.json.tmp"

DEFAULT_SERVE_HOST = "0.0.0.0"
DEFAULT_SERVE_PORT = 8766
DEFAULT_RESYNC_INTERVAL_SEC = 300
DEFAULT_PING_INTERVAL_SEC = 20
DEFAULT_RECENT_EVENT_LIMIT = 512
DEFAULT_TOPIC_GROUP_SIZE = 50
DEFAULT_GET_LOGS_BLOCK_SPAN = 400
SNAPSHOT_BLOCK_LAG = 64
HTTP_TIMEOUT_SEC = 30
WS_REQUEST_TIMEOUT_SEC = 15.0
TRANSFER_FLAT_LOG_SCALE = 10_000

ZERO_ADDR = "0x0000000000000000000000000000000000000000"
CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045"
CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e"
NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a"
NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296"
PROTOCOL_ADDRS = {
    CONDITIONAL_TOKENS,
    CTF_EXCHANGE,
    NEG_RISK_CTF_EXCHANGE,
    NEG_RISK_ADAPTER,
}

TRANSFER_SINGLE_TOPIC = "0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62"
TRANSFER_BATCH_TOPIC = "0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb"
CONDITION_RESOLVE_TOPIC = "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894"
POSITION_SPLIT_TOPIC = "0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298"
POSITION_MERGE_TOPIC = "0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca"
POSITION_REDEEM_TOPIC = "0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d"
ORDER_FILL_TOPIC = "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6"
TOKEN_REGISTER_TOPIC = "0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d"
POSITION_CONVERT_TOPIC = "0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e"

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


@dataclass(frozen=True)
class RuntimeConfig:
    rpc_name: str
    rpc_http_url: str
    rpc_ws_url: str
    graph_api_key: str
    serve_host: str
    serve_port: int
    resync_interval_sec: int
    ping_interval_sec: int
    recent_event_limit: int
    topic_group_size: int
    get_logs_block_span: int


@dataclass(frozen=True)
class TransferLeg:
    block_number: int
    transaction_index: int
    tx_hash: str
    log_index: int
    operator: str
    from_addr: str
    to_addr: str
    token_id: str
    amount_raw: int


@dataclass(frozen=True)
class OrderFillEvent:
    block_number: int
    transaction_index: int
    tx_hash: str
    log_index: int
    exchange: str
    maker: str
    taker: str
    buyer: str
    seller: str
    token_id: str
    token_amount_raw: int
    collateral_amount_raw: int
    fee_raw: int

    @property
    def price(self) -> Decimal:
        assert self.token_amount_raw > 0, self
        return Decimal(self.collateral_amount_raw) / Decimal(self.token_amount_raw)


@dataclass
class TxContext:
    block_number: int
    transaction_index: int
    tx_hash: str
    transfers: list[TransferLeg] = field(default_factory=list)
    split_users: set[str] = field(default_factory=set)
    merge_users: set[str] = field(default_factory=set)
    redemption_users: set[str] = field(default_factory=set)
    convert_users: set[str] = field(default_factory=set)
    order_fills: list[OrderFillEvent] = field(default_factory=list)

    def sort_key(self) -> tuple[int, int]:
        min_log_index = min(
            [item.log_index for item in self.transfers]
            + [item.log_index for item in self.order_fills]
            + [0]
        )
        return self.transaction_index, min_log_index


@dataclass
class TokenInfo:
    token_id: str
    condition_id: str | None = None
    question_id: str | None = None
    outcome_index: int | None = None
    resolution_timestamp: int | None = None
    payout_numerators: list[int] = field(default_factory=list)
    payout_denominator: int | None = None


@dataclass
class ConditionInfo:
    condition_id: str
    question_id: str | None = None
    outcome_slot_count: int | None = None
    resolution_timestamp: int | None = None
    token_ids: list[str] = field(default_factory=list)
    payout_numerators: list[int] = field(default_factory=list)
    payout_denominator: int | None = None
    market_question: str = ""
    market_outcomes: list[str] = field(default_factory=list)


class TrackerState:
    def __init__(self, recent_event_limit: int) -> None:
        self.watched_users: list[str] = []
        self.watched_user_set: set[str] = set()
        self.positions_by_user: dict[str, dict[str, int]] = {}
        self.token_cache: dict[str, TokenInfo] = {}
        self.condition_cache: dict[str, ConditionInfo] = {}
        self.recent_events: deque[dict[str, Any]] = deque(maxlen=recent_event_limit)
        self.last_snapshot_block = 0
        self.last_applied_block = 0
        self.head_block = 0
        self.last_resync_started_at = 0
        self.last_resync_finished_at = 0
        self.last_output_written_at = 0
        self._lock = threading.Lock()

    def set_resync_started(self, at_unix_sec: int) -> None:
        with self._lock:
            self.last_resync_started_at = at_unix_sec

    def set_resync_finished(self, at_unix_sec: int) -> None:
        with self._lock:
            self.last_resync_finished_at = at_unix_sec

    def set_watched_users(self, users: list[str]) -> None:
        with self._lock:
            self.watched_users = list(users)
            self.watched_user_set = set(users)
            self.positions_by_user = {
                user: dict(self.positions_by_user.get(user, {}))
                for user in users
            }

    def replace_positions(
        self,
        users: list[str],
        positions_by_user: dict[str, list[UserPosition]],
        snapshot_block: int,
    ) -> None:
        with self._lock:
            self.watched_users = list(users)
            self.watched_user_set = set(users)
            rewritten: dict[str, dict[str, int]] = {}
            for user in users:
                rows = positions_by_user.get(user, [])
                token_map: dict[str, int] = {}
                for row in rows:
                    token_map[row.token_id] = row.amount_raw
                rewritten[user] = token_map
            self.positions_by_user = rewritten
            self.last_snapshot_block = snapshot_block
            self.last_applied_block = snapshot_block
            self.head_block = max(self.head_block, snapshot_block)

    def update_head(self, block_number: int) -> None:
        with self._lock:
            self.head_block = max(self.head_block, block_number)

    def advance_last_applied(self, block_number: int) -> None:
        with self._lock:
            self.last_applied_block = max(self.last_applied_block, block_number)

    def seed_condition(self, condition: ConditionInfo) -> None:
        with self._lock:
            current = self.condition_cache.get(condition.condition_id)
            if current is None:
                self.condition_cache[condition.condition_id] = condition
                return
            current.question_id = current.question_id or condition.question_id
            current.outcome_slot_count = (
                current.outcome_slot_count
                if current.outcome_slot_count is not None
                else condition.outcome_slot_count
            )
            current.resolution_timestamp = (
                current.resolution_timestamp
                if current.resolution_timestamp is not None
                else condition.resolution_timestamp
            )
            if not current.token_ids:
                current.token_ids = list(condition.token_ids)
            if not current.payout_numerators:
                current.payout_numerators = list(condition.payout_numerators)
            if current.payout_denominator is None:
                current.payout_denominator = condition.payout_denominator
            if not current.market_question:
                current.market_question = condition.market_question
            if not current.market_outcomes:
                current.market_outcomes = list(condition.market_outcomes)

    def seed_token(self, token: TokenInfo) -> None:
        with self._lock:
            current = self.token_cache.get(token.token_id)
            if current is None:
                self.token_cache[token.token_id] = token
                return
            current.condition_id = current.condition_id or token.condition_id
            current.question_id = current.question_id or token.question_id
            current.outcome_index = (
                current.outcome_index
                if current.outcome_index is not None
                else token.outcome_index
            )
            current.resolution_timestamp = (
                current.resolution_timestamp
                if current.resolution_timestamp is not None
                else token.resolution_timestamp
            )
            if not current.payout_numerators:
                current.payout_numerators = list(token.payout_numerators)
            if current.payout_denominator is None:
                current.payout_denominator = token.payout_denominator

    def apply_delta(
        self,
        user: str,
        token_id: str,
        amount_delta_raw: int,
        event: dict[str, Any],
    ) -> None:
        with self._lock:
            assert user in self.watched_user_set, user
            token_map = self.positions_by_user.setdefault(user, {})
            next_amount = token_map.get(token_id, 0) + amount_delta_raw
            assert next_amount >= 0, (user, token_id, token_map.get(token_id, 0), amount_delta_raw)
            if next_amount == 0:
                token_map.pop(token_id, None)
            else:
                token_map[token_id] = next_amount
            self.recent_events.append(event)

    def mark_output_written(self, at_unix_sec: int) -> None:
        with self._lock:
            self.last_output_written_at = at_unix_sec

    def render_payload(self) -> dict[str, Any]:
        with self._lock:
            watched_users = list(self.watched_users)
            positions_by_user = {
                user: dict(self.positions_by_user.get(user, {}))
                for user in watched_users
            }
            token_cache = {
                token_id: TokenInfo(
                    token_id=row.token_id,
                    condition_id=row.condition_id,
                    question_id=row.question_id,
                    outcome_index=row.outcome_index,
                    resolution_timestamp=row.resolution_timestamp,
                    payout_numerators=list(row.payout_numerators),
                    payout_denominator=row.payout_denominator,
                )
                for token_id, row in self.token_cache.items()
            }
            condition_cache = {
                condition_id: ConditionInfo(
                    condition_id=row.condition_id,
                    question_id=row.question_id,
                    outcome_slot_count=row.outcome_slot_count,
                    resolution_timestamp=row.resolution_timestamp,
                    token_ids=list(row.token_ids),
                    payout_numerators=list(row.payout_numerators),
                    payout_denominator=row.payout_denominator,
                    market_question=row.market_question,
                    market_outcomes=list(row.market_outcomes),
                )
                for condition_id, row in self.condition_cache.items()
            }
            recent_events = list(self.recent_events)
            last_snapshot_block = self.last_snapshot_block
            last_applied_block = self.last_applied_block
            head_block = self.head_block
            last_resync_started_at = self.last_resync_started_at
            last_resync_finished_at = self.last_resync_finished_at
            last_output_written_at = self.last_output_written_at

        token_holder_count: dict[str, int] = defaultdict(int)
        token_total_amount_raw: dict[str, int] = defaultdict(int)
        position_count = 0
        active_token_ids: set[str] = set()
        users_rows: list[dict[str, Any]] = []
        for user in watched_users:
            token_map = positions_by_user[user]
            rows = []
            for token_id, amount_raw in sorted(token_map.items(), key=lambda item: item[0]):
                position_count += 1
                active_token_ids.add(token_id)
                token_holder_count[token_id] += 1
                token_total_amount_raw[token_id] += amount_raw
                rows.append({"token_id": token_id, "amount_raw": str(amount_raw)})
            users_rows.append({"user": user, "positions": rows})

        active_condition_ids = sorted(
            {
                token_cache[token_id].condition_id
                for token_id in active_token_ids
                if token_id in token_cache and token_cache[token_id].condition_id is not None
            }
        )

        tokens_rows: list[dict[str, Any]] = []
        for token_id in sorted(active_token_ids):
            token = token_cache.get(token_id)
            condition = (
                condition_cache[token.condition_id]
                if token is not None
                and token.condition_id is not None
                and token.condition_id in condition_cache
                else None
            )
            outcome_text = ""
            if (
                token is not None
                and condition is not None
                and token.outcome_index is not None
                and 0 <= token.outcome_index < len(condition.market_outcomes)
            ):
                outcome_text = condition.market_outcomes[token.outcome_index]
            resolved = bool(
                token is not None
                and (
                    token.resolution_timestamp is not None
                    or (
                        token.payout_denominator not in {None, 0}
                        and bool(token.payout_numerators)
                    )
                )
            )
            tokens_rows.append(
                {
                    "token_id": token_id,
                    "condition_id": token.condition_id if token is not None else None,
                    "question_id": token.question_id if token is not None else None,
                    "outcome_index": token.outcome_index if token is not None else None,
                    "outcome_text": outcome_text,
                    "resolved": resolved,
                }
            )

        conditions_rows: list[dict[str, Any]] = []
        for condition_id in active_condition_ids:
            condition = condition_cache[condition_id]
            conditions_rows.append(
                {
                    "condition_id": condition.condition_id,
                    "question_id": condition.question_id,
                    "outcome_slot_count": condition.outcome_slot_count,
                    "resolution_timestamp": condition.resolution_timestamp,
                    "token_ids": list(condition.token_ids),
                    "payout_numerators": list(condition.payout_numerators),
                    "payout_denominator": condition.payout_denominator,
                    "market_question": condition.market_question,
                    "market_outcomes": list(condition.market_outcomes),
                }
            )

        aggregate_rows = [
            {
                "token_id": token_id,
                "holder_count": token_holder_count[token_id],
                "total_amount_raw": str(total_amount_raw),
            }
            for token_id, total_amount_raw in token_total_amount_raw.items()
        ]
        aggregate_rows.sort(
            key=lambda item: (-int(item["total_amount_raw"]), item["token_id"])
        )

        recent_rows = list(reversed(recent_events))
        return {
            "summary": {
                "user_count": len(watched_users),
                "position_count": position_count,
                "token_count": len(active_token_ids),
                "condition_count": len(active_condition_ids),
                "recent_event_count": len(recent_rows),
                "snapshot_block": last_snapshot_block,
                "last_applied_block": last_applied_block,
                "head_block": head_block,
                "behind_blocks": max(0, head_block - last_applied_block),
                "last_resync_started_at_unix_sec": last_resync_started_at,
                "last_resync_finished_at_unix_sec": last_resync_finished_at,
                "last_output_written_at_unix_sec": last_output_written_at,
            },
            "conditions": conditions_rows,
            "tokens": tokens_rows,
            "aggregate": aggregate_rows,
            "users": users_rows,
            "recent_events": recent_rows,
        }


def normalize_hex(value: str) -> str:
    normalized = value.strip().lower()
    assert normalized.startswith("0x"), normalized
    assert len(normalized) >= 2, normalized
    return normalized


def hex_to_int(value: str) -> int:
    return int(value, 16)


def int_to_hex(value: int) -> str:
    assert value >= 0, value
    return hex(value)


def address_to_topic(value: str) -> str:
    address = normalize_address(value)
    return "0x" + ("0" * 24) + address[2:]


def extract_word_hex(data: str, index: int) -> str:
    payload = normalize_hex(data)[2:]
    start = index * 64
    end = start + 64
    assert end <= len(payload), (index, len(payload))
    return payload[start:end]


def extract_uint256(data: str, index: int) -> int:
    return int(extract_word_hex(data, index), 16)


def extract_uint256_array_from_offset(data: str, offset_bytes: int) -> list[int]:
    assert offset_bytes % 32 == 0, offset_bytes
    base_index = offset_bytes // 32
    length = extract_uint256(data, base_index)
    return [
        extract_uint256(data, base_index + 1 + item_index)
        for item_index in range(length)
    ]


def extract_address_from_topic(topic: str) -> str:
    normalized = normalize_hex(topic)
    assert len(normalized) == 66, normalized
    return normalize_address("0x" + normalized[-40:])


def derive_ws_url(http_url: str) -> str:
    parsed = urlparse(http_url)
    assert parsed.scheme in {"http", "https"}, parsed.scheme
    scheme = "wss" if parsed.scheme == "https" else "ws"
    return parsed._replace(scheme=scheme).geturl()


def load_runtime_config() -> RuntimeConfig:
    config_path = CONFIG_FILE if CONFIG_FILE.exists() else EXAMPLE_CONFIG_FILE
    config_payload = json.loads(config_path.read_text(encoding="utf-8"))
    assert isinstance(config_payload, dict), type(config_payload)
    nodes = config_payload["rpc_nodes"]
    assert isinstance(nodes, list), type(nodes)
    nodes_by_name = {str(node["name"]): node for node in nodes}
    active_rpc_name = os.environ.get(
        "TRACKER_RPC_NAME",
        str(config_payload["active_rpc"]),
    ).strip()
    assert active_rpc_name in nodes_by_name, active_rpc_name
    node = nodes_by_name[active_rpc_name]

    rpc_http_url = os.environ.get("TRACKER_RPC_HTTP_URL", str(node["url"])).strip()
    rpc_ws_url = os.environ.get("TRACKER_RPC_WS_URL", "").strip()
    if not rpc_ws_url:
        rpc_ws_url = derive_ws_url(rpc_http_url)
    graph_api_key = os.environ.get("THE_GRAPH_API_KEY", DEFAULT_API_KEY).strip()

    stage1_block_span = int(config_payload.get("stage1_rpc_block_span", 100))
    serve_port = int(os.environ.get("TRACKER_PORT", DEFAULT_SERVE_PORT))
    resync_interval_sec = int(
        os.environ.get("TRACKER_RESYNC_INTERVAL_SEC", DEFAULT_RESYNC_INTERVAL_SEC)
    )
    ping_interval_sec = int(
        os.environ.get("TRACKER_PING_INTERVAL_SEC", DEFAULT_PING_INTERVAL_SEC)
    )
    recent_event_limit = int(
        os.environ.get("TRACKER_RECENT_EVENT_LIMIT", DEFAULT_RECENT_EVENT_LIMIT)
    )
    topic_group_size = int(
        os.environ.get("TRACKER_TOPIC_GROUP_SIZE", DEFAULT_TOPIC_GROUP_SIZE)
    )
    get_logs_block_span = int(
        os.environ.get(
            "TRACKER_GET_LOGS_BLOCK_SPAN",
            max(DEFAULT_GET_LOGS_BLOCK_SPAN, stage1_block_span * 4),
        )
    )

    assert rpc_http_url, rpc_http_url
    assert rpc_ws_url, rpc_ws_url
    assert graph_api_key, graph_api_key
    assert serve_port > 0, serve_port
    assert resync_interval_sec > 0, resync_interval_sec
    assert ping_interval_sec > 0, ping_interval_sec
    assert recent_event_limit > 0, recent_event_limit
    assert topic_group_size > 0, topic_group_size
    assert get_logs_block_span > 0, get_logs_block_span

    return RuntimeConfig(
        rpc_name=active_rpc_name,
        rpc_http_url=rpc_http_url,
        rpc_ws_url=rpc_ws_url,
        graph_api_key=graph_api_key,
        serve_host=os.environ.get("TRACKER_HOST", DEFAULT_SERVE_HOST).strip(),
        serve_port=serve_port,
        resync_interval_sec=resync_interval_sec,
        ping_interval_sec=ping_interval_sec,
        recent_event_limit=recent_event_limit,
        topic_group_size=topic_group_size,
        get_logs_block_span=get_logs_block_span,
    )


def rpc_call(rpc_url: str, method: str, params: list[Any]) -> Any:
    payload = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    ).encode("utf-8")
    request = Request(
        rpc_url,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "User-Agent": "poly-tracker",
        },
    )
    with urlopen(request, timeout=HTTP_TIMEOUT_SEC) as response:
        body = json.loads(response.read().decode("utf-8"))
    assert "result" in body, body
    return body["result"]


def rpc_batch_call(rpc_url: str, calls: list[tuple[str, list[Any]]]) -> list[Any]:
    assert calls, calls
    payload = json.dumps(
        [
            {
                "jsonrpc": "2.0",
                "id": index,
                "method": method,
                "params": params,
            }
            for index, (method, params) in enumerate(calls, start=1)
        ]
    ).encode("utf-8")
    request = Request(
        rpc_url,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "User-Agent": "poly-tracker",
        },
    )
    with urlopen(request, timeout=HTTP_TIMEOUT_SEC) as response:
        body = json.loads(response.read().decode("utf-8"))
    assert isinstance(body, list), type(body)
    body.sort(key=lambda item: int(item["id"]))
    assert len(body) == len(calls), (len(body), len(calls))
    results: list[Any] = []
    for expected_id, item in enumerate(body, start=1):
        assert int(item["id"]) == expected_id, (expected_id, item)
        assert "result" in item, item
        results.append(item["result"])
    return results


def current_head_block(rpc_url: str) -> int:
    return hex_to_int(rpc_call(rpc_url, "eth_blockNumber", []))


def raw_log_key(log: dict[str, Any]) -> tuple[int, str, int, str]:
    return (
        hex_to_int(log["blockNumber"]),
        normalize_hex(log["transactionHash"]),
        hex_to_int(log["logIndex"]),
        normalize_hex(log["address"]),
    )


def raw_log_sort_key(log: dict[str, Any]) -> tuple[int, int, int, str]:
    return (
        hex_to_int(log["blockNumber"]),
        hex_to_int(log["transactionIndex"]),
        hex_to_int(log["logIndex"]),
        normalize_hex(log["address"]),
    )


def load_previous_tracker_output() -> dict[str, Any] | None:
    if not OUTPUT_JSON_FILE.exists():
        return None
    payload = json.loads(OUTPUT_JSON_FILE.read_text(encoding="utf-8"))
    assert isinstance(payload, dict), type(payload)
    return payload


def resolve_snapshot_block(api_key: str) -> int:
    with ThreadPoolExecutor(max_workers=2) as executor:
        future_map = {
            executor.submit(
                gql,
                api_key,
                PNL_SUBGRAPH_ID,
                META_QUERY,
                {},
                "tracker.pnl.meta",
            ): "pnl",
            executor.submit(
                gql,
                api_key,
                POLYMARKET_SUBGRAPH_ID,
                META_QUERY,
                {},
                "tracker.polymarket.meta",
            ): "polymarket",
        }
        blocks: dict[str, int] = {}
        for future in as_completed(future_map):
            blocks[future_map[future]] = int(
                future.result()["_meta"]["block"]["number"]
            )
    snapshot_block = min(blocks["pnl"], blocks["polymarket"]) - SNAPSHOT_BLOCK_LAG
    assert snapshot_block > 0, blocks
    return snapshot_block


def fetch_positions_snapshot(
    api_key: str,
    users: list[str],
    snapshot_block: int,
) -> dict[str, list[UserPosition]]:
    user_groups = chunked(users, USER_QUERY_BATCH_LIMIT)
    rows: list[UserPosition] = []
    if not user_groups:
        return {user: [] for user in users}

    with ThreadPoolExecutor(max_workers=min(MAX_WORKERS, len(user_groups))) as executor:
        future_map = {
            executor.submit(
                fetch_user_positions_shard,
                api_key,
                group,
                snapshot_block,
                index,
                len(user_groups),
            ): group
            for index, group in enumerate(user_groups, start=1)
        }
        for future in as_completed(future_map):
            rows.extend(future.result())

    rows.sort(key=lambda item: item.entity_id)
    return group_positions_by_user(users, rows)


class SimpleWebSocket:
    def __init__(self, url: str) -> None:
        self._url = url
        self._parsed = urlparse(url)
        assert self._parsed.scheme in {"ws", "wss"}, self._parsed.scheme
        assert self._parsed.hostname, url
        self._buffer = b""
        self._queued_messages: deque[dict[str, Any]] = deque()
        self._closed = False

        port = self._parsed.port or (443 if self._parsed.scheme == "wss" else 80)
        raw_socket = socket.create_connection((self._parsed.hostname, port))
        if self._parsed.scheme == "wss":
            context = ssl.create_default_context()
            self._socket = context.wrap_socket(
                raw_socket,
                server_hostname=self._parsed.hostname,
            )
        else:
            self._socket = raw_socket
        self._handshake()

    def _handshake(self) -> None:
        host_header = self._parsed.netloc or self._parsed.hostname or ""
        path = self._parsed.path or "/"
        if self._parsed.query:
            path = f"{path}?{self._parsed.query}"
        nonce = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host_header}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {nonce}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        ).encode("ascii")
        self._socket.sendall(request)

        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self._socket.recv(4096)
            assert chunk, "websocket handshake closed"
            response += chunk
        header_bytes, self._buffer = response.split(b"\r\n\r\n", 1)
        header_lines = header_bytes.decode("ascii").split("\r\n")
        assert header_lines[0].startswith("HTTP/1.1 101"), header_lines[0]

        headers: dict[str, str] = {}
        for line in header_lines[1:]:
            if not line:
                continue
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()

        expected_accept = base64.b64encode(
            hashlib.sha1((nonce + WS_GUID).encode("ascii")).digest()
        ).decode("ascii")
        assert headers.get("upgrade", "").lower() == "websocket", headers
        assert "upgrade" in headers.get("connection", "").lower(), headers
        assert headers.get("sec-websocket-accept") == expected_accept, headers

    def _recv_exact(self, size: int, timeout_sec: float) -> bytes | None:
        deadline = time.time() + timeout_sec
        while len(self._buffer) < size:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            readable, _, _ = select.select([self._socket], [], [], remaining)
            if not readable:
                return None
            chunk = self._socket.recv(65536)
            if not chunk:
                self._closed = True
                return None
            self._buffer += chunk
        payload = self._buffer[:size]
        self._buffer = self._buffer[size:]
        return payload

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        assert not self._closed, "websocket closed"
        mask = secrets.token_bytes(4)
        masked_payload = bytes(
            byte ^ mask[index % 4]
            for index, byte in enumerate(payload)
        )
        header = bytearray()
        header.append(0x80 | opcode)
        payload_len = len(masked_payload)
        if payload_len < 126:
            header.append(0x80 | payload_len)
        elif payload_len < (1 << 16):
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", payload_len))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", payload_len))
        header.extend(mask)
        self._socket.sendall(bytes(header) + masked_payload)

    def send_json(self, payload: dict[str, Any]) -> None:
        self._send_frame(0x1, json.dumps(payload).encode("utf-8"))

    def send_ping(self) -> None:
        self._send_frame(0x9, b"")

    def recv_json(self, timeout_sec: float) -> dict[str, Any] | None:
        if self._queued_messages:
            return self._queued_messages.popleft()

        deadline = time.time() + timeout_sec
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None

            header = self._recv_exact(2, remaining)
            if header is None:
                if self._closed:
                    return {"_ws_closed": True}
                return None

            first_byte = header[0]
            second_byte = header[1]
            fin = bool(first_byte & 0x80)
            opcode = first_byte & 0x0F
            masked = bool(second_byte & 0x80)
            payload_len = second_byte & 0x7F
            assert fin, opcode

            if payload_len == 126:
                extra = self._recv_exact(2, remaining)
                assert extra is not None, "websocket truncated frame"
                payload_len = struct.unpack("!H", extra)[0]
            elif payload_len == 127:
                extra = self._recv_exact(8, remaining)
                assert extra is not None, "websocket truncated frame"
                payload_len = struct.unpack("!Q", extra)[0]

            mask = b""
            if masked:
                mask = self._recv_exact(4, remaining)
                assert mask is not None, "websocket truncated mask"

            payload = b""
            if payload_len:
                payload = self._recv_exact(payload_len, remaining)
                assert payload is not None, "websocket truncated payload"

            if masked:
                payload = bytes(
                    byte ^ mask[index % 4]
                    for index, byte in enumerate(payload)
                )

            if opcode == 0x1:
                return json.loads(payload.decode("utf-8"))
            if opcode == 0x8:
                self._closed = True
                return {"_ws_closed": True}
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            assert False, opcode

    def subscribe(self, request_id: int, params: list[Any]) -> str:
        self.send_json(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": "eth_subscribe",
                "params": params,
            }
        )
        deadline = time.time() + WS_REQUEST_TIMEOUT_SEC
        while True:
            remaining = deadline - time.time()
            assert remaining > 0, request_id
            message = self.recv_json(remaining)
            assert message is not None, request_id
            if message.get("id") == request_id:
                assert "result" in message, message
                return str(message["result"])
            self._queued_messages.append(message)

    def close(self) -> None:
        if self._closed:
            return
        self._send_frame(0x8, b"")
        self._socket.close()
        self._closed = True


class ProtocolTracker:
    def __init__(self, config: RuntimeConfig) -> None:
        self.config = config
        self.state = TrackerState(config.recent_event_limit)
        self._control_lock = threading.Lock()
        self._force_resync = False

    def request_resync(self) -> None:
        with self._control_lock:
            self._force_resync = True

    def clear_resync_request(self) -> None:
        with self._control_lock:
            self._force_resync = False

    def resync_requested(self) -> bool:
        with self._control_lock:
            return self._force_resync

    def seed_metadata_cache(self) -> None:
        rebuild_output = load_previous_output()
        if rebuild_output is not None:
            cached_conditions = build_cached_condition_index(rebuild_output)
            for row in cached_conditions.values():
                self.state.seed_condition(
                    ConditionInfo(
                        condition_id=row["condition_id"],
                        question_id=row["question_id"],
                        outcome_slot_count=row["outcome_slot_count"],
                        resolution_timestamp=row["resolution_timestamp"],
                        token_ids=list(row["token_ids"]),
                        payout_numerators=list(row["payout_numerators"]),
                        payout_denominator=row["payout_denominator"],
                        market_question=row["market_question"],
                        market_outcomes=list(row["market_outcomes"]),
                    )
                )

            _, cached_markets = build_cached_token_data(rebuild_output)
            for token_id, market in cached_markets.items():
                self.state.seed_token(
                    TokenInfo(
                        token_id=token_id,
                        condition_id=market.condition_id,
                        question_id=market.question_id,
                        outcome_index=market.outcome_index,
                        resolution_timestamp=market.resolution_timestamp,
                        payout_numerators=list(market.payout_numerators),
                        payout_denominator=market.payout_denominator,
                    )
                )

        tracker_output = load_previous_tracker_output()
        if tracker_output is None:
            return
        for row in tracker_output.get("conditions", []):
            if not isinstance(row, dict):
                continue
            condition_id = row.get("condition_id")
            if not isinstance(condition_id, str):
                continue
            self.state.seed_condition(
                ConditionInfo(
                    condition_id=condition_id,
                    question_id=row.get("question_id") if isinstance(row.get("question_id"), str) else None,
                    outcome_slot_count=int(row["outcome_slot_count"]) if isinstance(row.get("outcome_slot_count"), int) else None,
                    resolution_timestamp=int(row["resolution_timestamp"]) if isinstance(row.get("resolution_timestamp"), int) else None,
                    token_ids=[str(item) for item in row.get("token_ids", [])] if isinstance(row.get("token_ids"), list) else [],
                    payout_numerators=[int(item) for item in row.get("payout_numerators", [])] if isinstance(row.get("payout_numerators"), list) else [],
                    payout_denominator=int(row["payout_denominator"]) if isinstance(row.get("payout_denominator"), int) else None,
                    market_question=str(row.get("market_question") or ""),
                    market_outcomes=[str(item) for item in row.get("market_outcomes", [])] if isinstance(row.get("market_outcomes"), list) else [],
                )
            )
        for row in tracker_output.get("tokens", []):
            if not isinstance(row, dict):
                continue
            token_id = row.get("token_id")
            if not isinstance(token_id, str):
                continue
            self.state.seed_token(
                TokenInfo(
                    token_id=token_id,
                    condition_id=row.get("condition_id") if isinstance(row.get("condition_id"), str) else None,
                    question_id=row.get("question_id") if isinstance(row.get("question_id"), str) else None,
                    outcome_index=int(row["outcome_index"]) if isinstance(row.get("outcome_index"), int) else None,
                )
            )

    def bootstrap(self) -> None:
        self.seed_metadata_cache()
        self.full_resync()

    def write_output_json(self) -> None:
        self.state.mark_output_written(int(time.time()))
        payload = self.state.render_payload()
        OUTPUT_JSON_TMP_FILE.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        OUTPUT_JSON_TMP_FILE.replace(OUTPUT_JSON_FILE)

    def watched_address_groups(self, users: list[str]) -> list[list[str]]:
        return chunked(users, self.config.topic_group_size)

    def build_log_filters(
        self,
        users: list[str],
        from_block: int | None,
        to_block: int | None,
    ) -> list[dict[str, Any]]:
        address_groups = self.watched_address_groups(users)
        filters: list[dict[str, Any]] = []
        for group in address_groups:
            topic_group = [address_to_topic(user) for user in group]

            transfer_from_filter: dict[str, Any] = {
                "address": CONDITIONAL_TOKENS,
                "topics": [
                    [TRANSFER_SINGLE_TOPIC, TRANSFER_BATCH_TOPIC],
                    None,
                    topic_group,
                ],
            }
            transfer_to_filter: dict[str, Any] = {
                "address": CONDITIONAL_TOKENS,
                "topics": [
                    [TRANSFER_SINGLE_TOPIC, TRANSFER_BATCH_TOPIC],
                    None,
                    None,
                    topic_group,
                ],
            }
            semantic_filter: dict[str, Any] = {
                "address": CONDITIONAL_TOKENS,
                "topics": [
                    [
                        POSITION_SPLIT_TOPIC,
                        POSITION_MERGE_TOPIC,
                        POSITION_REDEEM_TOPIC,
                    ],
                    topic_group,
                ],
            }
            order_maker_filter: dict[str, Any] = {
                "address": [CTF_EXCHANGE, NEG_RISK_CTF_EXCHANGE],
                "topics": [[ORDER_FILL_TOPIC], None, topic_group],
            }
            order_taker_filter: dict[str, Any] = {
                "address": [CTF_EXCHANGE, NEG_RISK_CTF_EXCHANGE],
                "topics": [[ORDER_FILL_TOPIC], None, None, topic_group],
            }
            convert_filter: dict[str, Any] = {
                "address": NEG_RISK_ADAPTER,
                "topics": [[POSITION_CONVERT_TOPIC], topic_group],
            }

            for item in [
                transfer_from_filter,
                transfer_to_filter,
                semantic_filter,
                order_maker_filter,
                order_taker_filter,
                convert_filter,
            ]:
                if from_block is not None:
                    item["fromBlock"] = int_to_hex(from_block)
                if to_block is not None:
                    item["toBlock"] = int_to_hex(to_block)
                filters.append(item)
        return filters

    def fetch_logs_range(
        self,
        users: list[str],
        from_block: int,
        to_block: int,
    ) -> list[dict[str, Any]]:
        assert from_block <= to_block, (from_block, to_block)
        filters = self.build_log_filters(users, from_block, to_block)
        deduped: dict[tuple[int, str, int, str], dict[str, Any]] = {}
        results = rpc_batch_call(
            self.config.rpc_http_url,
            [("eth_getLogs", [item]) for item in filters],
        )
        for logs in results:
            assert isinstance(logs, list), type(logs)
            for log in logs:
                assert isinstance(log, dict), type(log)
                deduped[raw_log_key(log)] = log
        return sorted(deduped.values(), key=raw_log_sort_key)

    def hydrate_token_metadata(self, token_ids: list[str]) -> None:
        missing_token_ids = sorted(
            {
                token_id
                for token_id in token_ids
                if token_id not in self.state.token_cache
            }
        )
        if not missing_token_ids:
            return

        market_results: dict[str, Any] = {}
        token_groups = chunked(missing_token_ids, 100)
        with ThreadPoolExecutor(max_workers=min(MAX_WORKERS, len(token_groups))) as executor:
            future_map = {
                executor.submit(
                    fetch_market_data_batch,
                    self.config.graph_api_key,
                    group,
                    None,
                    f"tracker.market.latest.chunk={index}/{len(token_groups)}",
                ): group
                for index, group in enumerate(token_groups, start=1)
            }
            for future in as_completed(future_map):
                market_results.update(future.result())

        condition_ids = sorted(
            {
                market.condition_id
                for market in market_results.values()
                if market.condition_id is not None
            }
        )

        condition_results: dict[str, Any] = {}
        if condition_ids:
            condition_groups = chunked(condition_ids, 100)
            with ThreadPoolExecutor(max_workers=min(MAX_WORKERS, len(condition_groups))) as executor:
                future_map = {
                    executor.submit(
                        fetch_conditions_batch,
                        self.config.graph_api_key,
                        group,
                        None,
                        f"tracker.conditions.latest.chunk={index}/{len(condition_groups)}",
                    ): group
                    for index, group in enumerate(condition_groups, start=1)
                }
                for future in as_completed(future_map):
                    condition_results.update(future.result())

        for condition_id, condition in condition_results.items():
            existing = self.state.condition_cache.get(condition_id)
            market_question = existing.market_question if existing is not None else ""
            market_outcomes = list(existing.market_outcomes) if existing is not None else []
            resolution_timestamp = (
                existing.resolution_timestamp
                if existing is not None
                else None
            )
            question_id = existing.question_id if existing is not None else None
            outcome_slot_count = (
                existing.outcome_slot_count if existing is not None else None
            )
            self.state.seed_condition(
                ConditionInfo(
                    condition_id=condition_id,
                    question_id=question_id,
                    outcome_slot_count=outcome_slot_count,
                    resolution_timestamp=resolution_timestamp,
                    token_ids=list(condition.position_ids),
                    payout_numerators=list(condition.payout_numerators),
                    payout_denominator=condition.payout_denominator,
                    market_question=market_question,
                    market_outcomes=market_outcomes,
                )
            )

        for token_id, market in market_results.items():
            self.state.seed_token(
                TokenInfo(
                    token_id=token_id,
                    condition_id=market.condition_id,
                    question_id=market.question_id,
                    outcome_index=market.outcome_index,
                    resolution_timestamp=market.resolution_timestamp,
                    payout_numerators=list(market.payout_numerators),
                    payout_denominator=market.payout_denominator,
                )
            )
            if market.condition_id is None:
                continue
            existing = self.state.condition_cache.get(market.condition_id)
            if existing is None:
                continue
            self.state.seed_condition(
                ConditionInfo(
                    condition_id=market.condition_id,
                    question_id=market.question_id,
                    outcome_slot_count=market.outcome_slot_count,
                    resolution_timestamp=market.resolution_timestamp,
                    token_ids=list(existing.token_ids),
                    payout_numerators=list(existing.payout_numerators),
                    payout_denominator=existing.payout_denominator,
                    market_question=existing.market_question,
                    market_outcomes=list(existing.market_outcomes),
                )
            )

    def flatten_position_token_ids(
        self,
        positions_by_user: dict[str, list[UserPosition]],
    ) -> list[str]:
        return sorted(
            {
                row.token_id
                for rows in positions_by_user.values()
                for row in rows
            }
        )

    def full_resync(self) -> None:
        self.clear_resync_request()
        self.state.set_resync_started(int(time.time()))

        users = load_addresses()
        snapshot_block = resolve_snapshot_block(self.config.graph_api_key)
        positions_by_user = fetch_positions_snapshot(
            self.config.graph_api_key,
            users,
            snapshot_block,
        )
        self.state.replace_positions(users, positions_by_user, snapshot_block)
        self.hydrate_token_metadata(self.flatten_position_token_ids(positions_by_user))

        head_block = current_head_block(self.config.rpc_http_url)
        self.state.update_head(head_block)
        self.backfill_range(snapshot_block + 1, head_block)
        self.state.set_resync_finished(int(time.time()))
        self.write_output_json()

    def parse_transfer_single(self, log: dict[str, Any]) -> list[TransferLeg]:
        topics = log["topics"]
        data = log["data"]
        assert len(topics) == 4, topics
        return [
            TransferLeg(
                block_number=hex_to_int(log["blockNumber"]),
                transaction_index=hex_to_int(log["transactionIndex"]),
                tx_hash=normalize_hex(log["transactionHash"]),
                log_index=hex_to_int(log["logIndex"]) * TRANSFER_FLAT_LOG_SCALE,
                operator=extract_address_from_topic(topics[1]),
                from_addr=extract_address_from_topic(topics[2]),
                to_addr=extract_address_from_topic(topics[3]),
                token_id=str(extract_uint256(data, 0)),
                amount_raw=extract_uint256(data, 1),
            )
        ]

    def parse_transfer_batch(self, log: dict[str, Any]) -> list[TransferLeg]:
        topics = log["topics"]
        data = log["data"]
        assert len(topics) == 4, topics
        ids_offset = extract_uint256(data, 0)
        values_offset = extract_uint256(data, 1)
        ids = extract_uint256_array_from_offset(data, ids_offset)
        values = extract_uint256_array_from_offset(data, values_offset)
        assert len(ids) == len(values), (len(ids), len(values))

        block_number = hex_to_int(log["blockNumber"])
        transaction_index = hex_to_int(log["transactionIndex"])
        tx_hash = normalize_hex(log["transactionHash"])
        raw_log_index = hex_to_int(log["logIndex"])
        operator = extract_address_from_topic(topics[1])
        from_addr = extract_address_from_topic(topics[2])
        to_addr = extract_address_from_topic(topics[3])
        result: list[TransferLeg] = []
        for item_index, (token_id, amount_raw) in enumerate(zip(ids, values)):
            result.append(
                TransferLeg(
                    block_number=block_number,
                    transaction_index=transaction_index,
                    tx_hash=tx_hash,
                    log_index=raw_log_index * TRANSFER_FLAT_LOG_SCALE + item_index,
                    operator=operator,
                    from_addr=from_addr,
                    to_addr=to_addr,
                    token_id=str(token_id),
                    amount_raw=amount_raw,
                )
            )
        return result

    def parse_order_fill(self, log: dict[str, Any]) -> OrderFillEvent:
        topics = log["topics"]
        data = log["data"]
        assert len(topics) == 4, topics
        maker = extract_address_from_topic(topics[2])
        taker = extract_address_from_topic(topics[3])
        maker_asset_id = extract_uint256(data, 0)
        taker_asset_id = extract_uint256(data, 1)
        maker_amount = extract_uint256(data, 2)
        taker_amount = extract_uint256(data, 3)
        fee = extract_uint256(data, 4)

        assert (maker_asset_id == 0) ^ (taker_asset_id == 0), (
            maker_asset_id,
            taker_asset_id,
        )
        if maker_asset_id == 0:
            buyer = maker
            seller = taker
            token_id = str(taker_asset_id)
            token_amount = taker_amount
            collateral_amount = maker_amount
        else:
            buyer = taker
            seller = maker
            token_id = str(maker_asset_id)
            token_amount = maker_amount
            collateral_amount = taker_amount

        return OrderFillEvent(
            block_number=hex_to_int(log["blockNumber"]),
            transaction_index=hex_to_int(log["transactionIndex"]),
            tx_hash=normalize_hex(log["transactionHash"]),
            log_index=hex_to_int(log["logIndex"]),
            exchange=normalize_hex(log["address"]),
            maker=maker,
            taker=taker,
            buyer=buyer,
            seller=seller,
            token_id=token_id,
            token_amount_raw=token_amount,
            collateral_amount_raw=collateral_amount,
            fee_raw=fee,
        )

    def build_tx_contexts(self, raw_logs: list[dict[str, Any]]) -> list[TxContext]:
        tx_map: dict[str, TxContext] = {}
        for log in raw_logs:
            tx_hash = normalize_hex(log["transactionHash"])
            context = tx_map.get(tx_hash)
            if context is None:
                context = TxContext(
                    block_number=hex_to_int(log["blockNumber"]),
                    transaction_index=hex_to_int(log["transactionIndex"]),
                    tx_hash=tx_hash,
                )
                tx_map[tx_hash] = context

            address = normalize_hex(log["address"])
            topics = log["topics"]
            assert topics, log
            topic0 = normalize_hex(topics[0])

            if address == CONDITIONAL_TOKENS and topic0 == TRANSFER_SINGLE_TOPIC:
                context.transfers.extend(self.parse_transfer_single(log))
                continue
            if address == CONDITIONAL_TOKENS and topic0 == TRANSFER_BATCH_TOPIC:
                context.transfers.extend(self.parse_transfer_batch(log))
                continue
            if address == CONDITIONAL_TOKENS and topic0 == POSITION_SPLIT_TOPIC:
                context.split_users.add(extract_address_from_topic(topics[1]))
                continue
            if address == CONDITIONAL_TOKENS and topic0 == POSITION_MERGE_TOPIC:
                context.merge_users.add(extract_address_from_topic(topics[1]))
                continue
            if address == CONDITIONAL_TOKENS and topic0 == POSITION_REDEEM_TOPIC:
                context.redemption_users.add(extract_address_from_topic(topics[1]))
                continue
            if address in {CTF_EXCHANGE, NEG_RISK_CTF_EXCHANGE} and topic0 == ORDER_FILL_TOPIC:
                context.order_fills.append(self.parse_order_fill(log))
                continue
            if address == NEG_RISK_ADAPTER and topic0 == POSITION_CONVERT_TOPIC:
                context.convert_users.add(extract_address_from_topic(topics[1]))
                continue

        contexts = list(tx_map.values())
        for context in contexts:
            context.transfers.sort(key=lambda item: item.log_index)
            context.order_fills.sort(key=lambda item: item.log_index)
        contexts.sort(key=lambda item: item.sort_key())
        return contexts

    def classify_incoming(
        self,
        context: TxContext,
        transfer: TransferLeg,
        user: str,
    ) -> tuple[str, OrderFillEvent | None]:
        for fill in context.order_fills:
            if fill.buyer != user:
                continue
            if fill.token_id != transfer.token_id:
                continue
            if fill.token_amount_raw != transfer.amount_raw:
                continue
            return "order_buy", fill
        if user in context.split_users:
            return "split_in", None
        if user in context.convert_users:
            return "convert_in", None
        if transfer.from_addr == ZERO_ADDR:
            return "mint_in", None
        if transfer.from_addr in self.state.watched_user_set:
            return "tracked_in", None
        if transfer.from_addr in PROTOCOL_ADDRS or transfer.operator in PROTOCOL_ADDRS:
            return "protocol_in", None
        return "transfer_in", None

    def classify_outgoing(
        self,
        context: TxContext,
        transfer: TransferLeg,
        user: str,
    ) -> tuple[str, OrderFillEvent | None]:
        for fill in context.order_fills:
            if fill.seller != user:
                continue
            if fill.token_id != transfer.token_id:
                continue
            if fill.token_amount_raw != transfer.amount_raw:
                continue
            return "order_sell", fill
        if user in context.merge_users:
            return "merge_out", None
        if user in context.redemption_users:
            return "redeem_out", None
        if user in context.convert_users:
            return "convert_out", None
        if transfer.to_addr == ZERO_ADDR:
            return "burn_out", None
        if transfer.to_addr in self.state.watched_user_set:
            return "tracked_out", None
        if transfer.to_addr in PROTOCOL_ADDRS or transfer.operator in PROTOCOL_ADDRS:
            return "protocol_out", None
        return "transfer_out", None

    def build_user_event(
        self,
        context: TxContext,
        transfer: TransferLeg,
        user: str,
        direction: str,
        kind: str,
        matched_fill: OrderFillEvent | None,
    ) -> dict[str, Any]:
        counterparty = transfer.from_addr if direction == "in" else transfer.to_addr
        payload: dict[str, Any] = {
            "block_number": transfer.block_number,
            "tx_hash": transfer.tx_hash,
            "log_index": transfer.log_index,
            "user": user,
            "direction": direction,
            "kind": kind,
            "token_id": transfer.token_id,
            "amount_raw": str(transfer.amount_raw),
            "counterparty": counterparty,
            "operator": transfer.operator,
            "tx_transfer_count": len(context.transfers),
            "tx_order_fill_count": len(context.order_fills),
        }
        if matched_fill is not None:
            payload["exchange"] = matched_fill.exchange
            payload["collateral_amount_raw"] = str(matched_fill.collateral_amount_raw)
            payload["price"] = format_decimal(matched_fill.price, 10)
            payload["fee_raw"] = str(matched_fill.fee_raw)
        return payload

    def apply_block_logs(self, block_number: int, raw_logs: list[dict[str, Any]]) -> bool:
        changed = False
        touched_token_ids: set[str] = set()
        for context in self.build_tx_contexts(raw_logs):
            for transfer in context.transfers:
                if transfer.from_addr in self.state.watched_user_set:
                    kind, fill = self.classify_outgoing(context, transfer, transfer.from_addr)
                    event = self.build_user_event(
                        context,
                        transfer,
                        transfer.from_addr,
                        "out",
                        kind,
                        fill,
                    )
                    self.state.apply_delta(
                        transfer.from_addr,
                        transfer.token_id,
                        -transfer.amount_raw,
                        event,
                    )
                    touched_token_ids.add(transfer.token_id)
                    changed = True
                if transfer.to_addr in self.state.watched_user_set:
                    kind, fill = self.classify_incoming(context, transfer, transfer.to_addr)
                    event = self.build_user_event(
                        context,
                        transfer,
                        transfer.to_addr,
                        "in",
                        kind,
                        fill,
                    )
                    self.state.apply_delta(
                        transfer.to_addr,
                        transfer.token_id,
                        transfer.amount_raw,
                        event,
                    )
                    touched_token_ids.add(transfer.token_id)
                    changed = True
        self.state.update_head(block_number)
        self.state.advance_last_applied(block_number)
        if touched_token_ids:
            self.hydrate_token_metadata(sorted(touched_token_ids))
        return changed

    def backfill_range(self, from_block: int, to_block: int) -> None:
        if from_block > to_block:
            return
        users = list(self.state.watched_users)
        span = self.config.get_logs_block_span
        start_block = from_block
        any_change = False
        while start_block <= to_block:
            end_block = min(start_block + span - 1, to_block)
            logs = self.fetch_logs_range(users, start_block, end_block)
            block_map: dict[int, dict[tuple[int, str, int, str], dict[str, Any]]] = defaultdict(dict)
            for log in logs:
                block_map[hex_to_int(log["blockNumber"])][raw_log_key(log)] = log
            for block_number in sorted(block_map):
                changed = self.apply_block_logs(
                    block_number,
                    sorted(block_map[block_number].values(), key=raw_log_sort_key),
                )
                any_change = any_change or changed
            self.state.advance_last_applied(end_block)
            start_block = end_block + 1
        if any_change:
            self.write_output_json()

    def flush_complete_blocks(
        self,
        pending_logs: dict[int, dict[tuple[int, str, int, str], dict[str, Any]]],
        complete_before_block: int,
    ) -> None:
        if complete_before_block <= 0:
            return
        any_change = False
        ready_blocks = sorted(
            block_number
            for block_number in pending_logs
            if block_number < complete_before_block
        )
        for block_number in ready_blocks:
            changed = self.apply_block_logs(
                block_number,
                sorted(pending_logs.pop(block_number).values(), key=raw_log_sort_key),
            )
            any_change = any_change or changed
        self.state.advance_last_applied(complete_before_block - 1)
        if any_change:
            self.write_output_json()

    def subscribe_live_streams(self, ws: SimpleWebSocket) -> dict[str, str]:
        subscription_map: dict[str, str] = {}
        request_id = 1
        for filter_params in self.build_log_filters(
            list(self.state.watched_users),
            None,
            None,
        ):
            subscription_id = ws.subscribe(request_id, ["logs", filter_params])
            subscription_map[subscription_id] = "logs"
            request_id += 1
        head_subscription = ws.subscribe(request_id, ["newHeads"])
        subscription_map[head_subscription] = "newHeads"
        return subscription_map

    def run_live_until(self, deadline_unix_sec: float) -> None:
        ws = SimpleWebSocket(self.config.rpc_ws_url)
        subscriptions = self.subscribe_live_streams(ws)
        pending_logs: dict[int, dict[tuple[int, str, int, str], dict[str, Any]]] = defaultdict(dict)
        last_ping_at = time.time()

        while time.time() < deadline_unix_sec and not self.resync_requested():
            if time.time() - last_ping_at >= self.config.ping_interval_sec:
                ws.send_ping()
                last_ping_at = time.time()

            message = ws.recv_json(1.0)
            if message is None:
                continue
            if message.get("_ws_closed"):
                break
            if message.get("method") != "eth_subscription":
                continue

            params = message["params"]
            subscription_id = params["subscription"]
            result = params["result"]
            assert subscription_id in subscriptions, subscription_id
            if subscriptions[subscription_id] == "newHeads":
                head_block = hex_to_int(result["number"])
                self.state.update_head(head_block)
                self.flush_complete_blocks(pending_logs, head_block)
                continue

            log = result
            assert isinstance(log, dict), type(log)
            if log.get("removed"):
                self.request_resync()
                continue
            block_number = hex_to_int(log["blockNumber"])
            self.state.update_head(block_number)
            pending_logs[block_number][raw_log_key(log)] = log

        ws.close()
        head_block = current_head_block(self.config.rpc_http_url)
        self.state.update_head(head_block)
        pending_logs.clear()
        self.backfill_range(self.state.last_applied_block + 1, head_block)

    def run_forever(self) -> None:
        next_resync_at = time.time() + self.config.resync_interval_sec
        while True:
            if self.resync_requested() or time.time() >= next_resync_at:
                self.full_resync()
                next_resync_at = time.time() + self.config.resync_interval_sec
                continue
            self.run_live_until(next_resync_at)
            if not self.resync_requested() and time.time() < next_resync_at:
                time.sleep(1.0)


def build_http_handler(tracker: ProtocolTracker) -> type[BaseHTTPRequestHandler]:
    class TrackerHandler(BaseHTTPRequestHandler):
        def _write_json(self, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _read_json_body(self) -> dict[str, Any]:
            content_length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(content_length)
            if not raw:
                return {}
            payload = json.loads(raw.decode("utf-8"))
            assert isinstance(payload, dict), type(payload)
            return payload

        def do_GET(self) -> None:
            path = urlparse(self.path).path
            if path in {"/", "/api/state"}:
                self._write_json(tracker.state.render_payload())
                return
            if path == "/api/health":
                payload = tracker.state.render_payload()
                self._write_json(
                    {
                        "ok": True,
                        "summary": payload["summary"],
                    }
                )
                return
            if path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
                return
            assert False, path

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            if path == "/api/resync":
                self._read_json_body()
                tracker.request_resync()
                self._write_json(
                    {
                        "ok": True,
                        "summary": tracker.state.render_payload()["summary"],
                    }
                )
                return
            assert False, path

        def log_message(self, _format: str, *_args: Any) -> None:
            return

    return TrackerHandler


def serve_http(tracker: ProtocolTracker, host: str, port: int) -> None:
    server = ThreadingHTTPServer((host, port), build_http_handler(tracker))
    print(f"[tracker] serving http://{host}:{port}")
    server.serve_forever()


def main() -> None:
    config = load_runtime_config()
    tracker = ProtocolTracker(config)
    tracker.bootstrap()

    server_thread = threading.Thread(
        target=serve_http,
        args=(tracker, config.serve_host, config.serve_port),
        daemon=True,
    )
    server_thread.start()

    print(
        json.dumps(
            {
                "rpc_name": config.rpc_name,
                "rpc_http_url": config.rpc_http_url,
                "rpc_ws_url": config.rpc_ws_url,
                "listen": f"http://{config.serve_host}:{config.serve_port}",
                "address_file": str(SCRIPT_DIR / "address.txt"),
                "tracker_json_file": str(OUTPUT_JSON_FILE),
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    tracker.run_forever()


if __name__ == "__main__":
    main()
