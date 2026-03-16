#!/usr/bin/env python3

'''
刷新流程(选一):
    刷新持仓,刷新price
    刷新持仓,补全price(可能部分用stale数据)
    刷新price
刷新price后:
    过滤掉已经结算的token
    重算用户/aggre持仓比例
    阈值过滤, 重算aggre持仓比例
'''

from __future__ import annotations

import json
import os
import sys
import threading
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from decimal import Decimal, getcontext
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, TypeVar
from urllib.parse import urlparse
from urllib.request import Request, urlopen

getcontext().prec = 50

SCRIPT_DIR = Path(__file__).resolve().parent
ADDRESS_FILE = SCRIPT_DIR / "address.txt"
OUTPUT_JSON_FILE = SCRIPT_DIR / "rebuild.json"
OUTPUT_JSON_TMP_FILE = SCRIPT_DIR / "rebuild.json.tmp"
OUTPUT_HTML_FILE = SCRIPT_DIR / "rebuild.html"

DEFAULT_API_KEY = "1d7a83f3e6778cd93dfbae707bb192de"
DEFAULT_SERVE_HOST = "0.0.0.0"
DEFAULT_SERVE_PORT = 8765
POLYMARKET_SUBGRAPH_ID = "81Dm16JjuFSrqz813HysXoUPvzTwE7fsfPk2RTf66nyC"
PNL_SUBGRAPH_ID = "6c58N5U4MtQE2Y8njfVrrAfRykzfqajMGeTMEvMmskVz"
GAMMA_API_BASE = "https://gamma-api.polymarket.com"

USER_POSITIONS_PAGE_LIMIT = 1000
USER_QUERY_BATCH_LIMIT = 50
ID_QUERY_BATCH_LIMIT = 100
MAX_WORKERS = 8
MARKET_META_MAX_WORKERS = 8
MAX_BAD_INDEXER_RETRIES = 100
AGGREGATE_WEIGHT_THRESHOLD = Decimal("0.001")

UNIT = Decimal("1000000")
ZERO = Decimal("0")
ONE = Decimal("1")
TItem = TypeVar("TItem")
TResult = TypeVar("TResult")

FULL_REFRESH_STAGES = [
    "load_addresses",
    "resolve_snapshot_block",
    "fetch_user_positions",
    "fetch_market_data_held",
    "fetch_conditions",
    "fetch_market_data_sibling",
    "prepare_prices",
    "compute_output",
    "filter_and_normalize",
    "fetch_gamma_market_meta",
    "write_json",
]
PRICE_REFRESH_STAGES = [
    "fetch_market_data_held",
    "fetch_conditions",
    "fetch_market_data_sibling",
    "prepare_prices",
    "compute_output",
    "filter_and_normalize",
    "fetch_gamma_market_meta",
    "write_json",
]

STAGE_DISPLAY_NAMES = {
    "load_addresses": "load addresses",
    "resolve_snapshot_block": "resolve snapshot block",
    "fetch_user_positions": "fetch user positions",
    "fetch_market_data_held": "fetch held token data",
    "fetch_conditions": "fetch condition data",
    "fetch_market_data_sibling": "fetch sibling token data",
    "prepare_prices": "prepare prices",
    "compute_output": "compute output",
    "filter_and_normalize": "filter and normalize",
    "fetch_gamma_market_meta": "fetch market metadata",
    "write_json": "write json",
}

STAGE_DISPLAY_UNITS = {
    "load_addresses": "users",
    "resolve_snapshot_block": "subgraphs",
    "fetch_user_positions": "users",
    "fetch_market_data_held": "tokens",
    "fetch_conditions": "conditions",
    "fetch_market_data_sibling": "tokens",
    "fetch_gamma_market_meta": "markets",
}

FLOW_REFRESH_HOLDINGS_FILL_MISSING_PRICE = "refresh_holdings_fill_missing_price"
FLOW_REFRESH_HOLDINGS_REFRESH_PRICE = "refresh_holdings_refresh_price"
FLOW_REFRESH_PRICE = "refresh_price"

META_QUERY = """
query Meta {
  _meta {
    block {
      number
    }
  }
}
"""

USER_POSITIONS_QUERY = """
query UserPositions($users: [String!]!, $after: String!, $block: Int!, $first: Int!) {
  userPositions(
    first: $first
    orderBy: id
    orderDirection: asc
    block: { number: $block }
    where: {user_in: $users, amount_gt: "0", id_gt: $after}
  ) {
    id
    user
    tokenId
    amount
  }
}
"""

MARKET_DATA_QUERY = """
query MarketDatas($ids: [ID!]!, $block: Int!) {
  marketDatas(
    where: {id_in: $ids}
    orderBy: id
    orderDirection: asc
    block: { number: $block }
  ) {
    id
    outcomeIndex
    priceOrderbook
    condition {
      id
      questionId
      outcomeSlotCount
      resolutionTimestamp
      payoutNumerators
      payoutDenominator
    }
  }
}
"""

MARKET_DATA_LATEST_QUERY = """
query MarketDatas($ids: [ID!]!) {
  marketDatas(
    where: {id_in: $ids}
    orderBy: id
    orderDirection: asc
  ) {
    id
    outcomeIndex
    priceOrderbook
    condition {
      id
      questionId
      outcomeSlotCount
      resolutionTimestamp
      payoutNumerators
      payoutDenominator
    }
  }
}
"""

CONDITIONS_QUERY = """
query Conditions($ids: [ID!]!, $block: Int!) {
  conditions(
    where: {id_in: $ids}
    orderBy: id
    orderDirection: asc
    block: { number: $block }
  ) {
    id
    positionIds
    payoutNumerators
    payoutDenominator
  }
}
"""

CONDITIONS_LATEST_QUERY = """
query Conditions($ids: [ID!]!) {
  conditions(
    where: {id_in: $ids}
    orderBy: id
    orderDirection: asc
  ) {
    id
    positionIds
    payoutNumerators
    payoutDenominator
  }
}
"""


@dataclass(frozen=True)
class UserPosition:
    entity_id: str
    user: str
    token_id: str
    amount_raw: int


@dataclass(frozen=True)
class PortfolioState:
    users: list[str]
    positions_by_user: dict[str, list[UserPosition]]
    token_ids: list[str]
    snapshot_block: int


@dataclass
class MarketData:
    token_id: str
    condition_id: str | None
    question_id: str | None
    outcome_slot_count: int | None
    resolution_timestamp: int | None
    outcome_index: int | None
    price_orderbook: Decimal | None
    payout_numerators: list[int]
    payout_denominator: int | None


@dataclass
class ConditionMeta:
    condition_id: str
    position_ids: list[str]
    payout_numerators: list[int]
    payout_denominator: int | None


@dataclass(frozen=True)
class PricePoint:
    value: Decimal
    source: str


@dataclass(frozen=True)
class FlowSpec:
    stages: list[str]
    use_cached_positions: bool
    use_latest_price_data: bool
    refresh_all_prices: bool


FLOW_SPECS = {
    FLOW_REFRESH_HOLDINGS_FILL_MISSING_PRICE: FlowSpec(
        stages=FULL_REFRESH_STAGES,
        use_cached_positions=False,
        use_latest_price_data=False,
        refresh_all_prices=False,
    ),
    FLOW_REFRESH_HOLDINGS_REFRESH_PRICE: FlowSpec(
        stages=FULL_REFRESH_STAGES,
        use_cached_positions=False,
        use_latest_price_data=False,
        refresh_all_prices=True,
    ),
    FLOW_REFRESH_PRICE: FlowSpec(
        stages=PRICE_REFRESH_STAGES,
        use_cached_positions=True,
        use_latest_price_data=True,
        refresh_all_prices=True,
    ),
}


def progress(message: str) -> None:
    print(f"[aggregate] {message}", file=sys.stderr, flush=True)


def format_progress(stage: str, done: int, total: int) -> str:
    label = STAGE_DISPLAY_NAMES.get(stage, stage)
    unit = STAGE_DISPLAY_UNITS.get(stage)
    if unit is None:
        return f"{label}: {done}/{total}"
    return f"{label}: {done}/{total} {unit}"


class ProgressBoard:
    def __init__(self, stages: list[str]) -> None:
        self._stages = stages
        self._stage_index = {stage: index for index, stage in enumerate(stages)}
        self._state = {stage: (0, 0) for stage in stages}
        self._lock = threading.Lock()
        self._use_overwrite = sys.stderr.isatty()
        if self._use_overwrite:
            for stage in stages:
                progress(format_progress(stage, 0, 0))

    def update(self, stage: str, done: int, total: int) -> None:
        assert stage in self._state, stage
        assert total >= 0, total
        assert 0 <= done <= total, (done, total)
        with self._lock:
            self._state[stage] = (done, total)
            message = format_progress(stage, done, total)
            if not self._use_overwrite:
                progress(message)
                return
            line = self._stage_index[stage]
            lines_up = len(self._stages) - line
            sys.stderr.write(f"\x1b[{lines_up}A")
            sys.stderr.write(f"\r[aggregate] {message}\x1b[K")
            sys.stderr.write(f"\x1b[{lines_up}B")
            sys.stderr.flush()


PROGRESS_BOARD: ProgressBoard | None = None
OUTPUT_FLOW_LOCK = threading.Lock()


def progress_step(stage: str, done: int, total: int) -> None:
    assert PROGRESS_BOARD is not None
    assert total >= 0, total
    assert 0 <= done <= total, (done, total)
    PROGRESS_BOARD.update(stage, done, total)


def summarize_value(value: Any) -> Any:
    if isinstance(value, list):
        if len(value) <= 8:
            return value
        return {
            "count": len(value),
            "head": value[:3],
            "tail": value[-3:],
        }
    if isinstance(value, dict):
        return {key: summarize_value(item) for key, item in value.items()}
    return value


def chunked(values: list[str], size: int) -> list[list[str]]:
    return [values[i: i + size] for i in range(0, len(values), size)]


def run_parallel_indexed(
    items: list[TItem],
    max_workers: int,
    worker: Callable[[int, int, TItem], TResult],
    progress_name: str | None = None,
    progress_total: int | None = None,
    progress_count: Callable[[TItem], int] | None = None,
) -> list[tuple[int, TItem, TResult]]:
    if not items:
        if progress_name is not None:
            progress_step(progress_name, 0, progress_total or 0)
        return []
    total = len(items)
    progress_denominator = progress_total if progress_total is not None else total
    if progress_name is not None:
        progress_step(progress_name, 0, progress_denominator)
    with ThreadPoolExecutor(max_workers=min(max_workers, total)) as executor:
        future_map = {
            executor.submit(worker, index, total, item): (index, item)
            for index, item in enumerate(items, start=1)
        }
        results: list[tuple[int, TItem, TResult]] = []
        done = 0
        for future in as_completed(future_map):
            index, item = future_map[future]
            results.append((index, item, future.result()))
            done += progress_count(item) if progress_count is not None else 1
            if progress_name is not None:
                progress_step(progress_name, done, progress_denominator)
    return results


def normalize_address(value: str) -> str:
    normalized = value.strip().lower()
    assert normalized.startswith("0x"), normalized
    assert len(normalized) == 42, normalized
    assert all(ch in "0123456789abcdef" for ch in normalized[2:]), normalized
    return normalized


def load_addresses() -> list[str]:
    assert ADDRESS_FILE.exists(), str(ADDRESS_FILE)
    result: list[str] = []
    seen: set[str] = set()
    for raw_line in ADDRESS_FILE.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        normalized = normalize_address(line)
        if normalized in seen:
            continue
        seen.add(normalized)
        result.append(normalized)
    assert result, str(ADDRESS_FILE)
    progress_step("load_addresses", len(result), len(result))
    return result


def gql(
    api_key: str,
    subgraph_id: str,
    query: str,
    variables: dict[str, Any],
    label: str,
) -> dict[str, Any]:
    payload = json.dumps(
        {"query": query, "variables": variables}).encode("utf-8")
    for attempt in range(1, MAX_BAD_INDEXER_RETRIES + 1):
        request = Request(
            f"https://gateway.thegraph.com/api/{api_key}/subgraphs/id/{subgraph_id}",
            data=payload,
            headers={
                "Content-Type": "application/json",
                "User-Agent": "poly-aggregate-exposure",
            },
        )
        with urlopen(request) as response:
            body = json.loads(response.read().decode("utf-8"))
        if "errors" not in body:
            assert "data" in body, body
            return body["data"]
        errors = body.get("errors")
        is_bad_indexers = (
            isinstance(errors, list)
            and any(
                isinstance(item, dict)
                and isinstance(item.get("message"), str)
                and "bad indexers:" in item["message"].lower()
                for item in errors
            )
        )
        if is_bad_indexers and attempt < MAX_BAD_INDEXER_RETRIES:
            time.sleep(1.0)
            continue
        assert "errors" not in body, {
            "label": label,
            "subgraph_id": subgraph_id,
            "attempt": attempt,
            "variables": summarize_value(variables),
            "body": body,
        }
    assert False, label


def parse_decimal(value: str | None) -> Decimal | None:
    if value is None:
        return None
    return Decimal(value)


def parse_int(value: Any) -> int | None:
    if value is None:
        return None
    return int(value)


def normalize_payout_denominator(value: int | None) -> int | None:
    if value in {None, 0}:
        return None
    assert value > 0, value
    return value


def format_decimal(value: Decimal, places: int = 10) -> str:
    return format(value, f".{places}f")


def load_previous_output() -> dict[str, Any] | None:
    if not OUTPUT_JSON_FILE.exists():
        return None
    payload = json.loads(OUTPUT_JSON_FILE.read_text(encoding="utf-8"))
    assert isinstance(payload, dict), type(payload)
    return payload


def output_uses_normalized_schema(output: dict[str, Any] | None) -> bool:
    if not isinstance(output, dict):
        return False
    return (
        isinstance(output.get("summary"), dict)
        and isinstance(output.get("conditions"), list)
        and isinstance(output.get("tokens"), list)
        and isinstance(output.get("aggregate"), list)
        and isinstance(output.get("users"), list)
    )


def load_cached_user_positions(user: str, user_payload: dict[str, Any]) -> list[UserPosition]:
    positions_payload = user_payload.get("positions")
    assert isinstance(positions_payload, list), user_payload
    result: list[UserPosition] = []
    for index, position_row in enumerate(positions_payload, start=1):
        assert isinstance(position_row, dict), position_row
        token_id = position_row.get("token_id")
        amount_raw_value = position_row.get("amount_raw")
        assert isinstance(token_id, str), position_row
        assert isinstance(amount_raw_value, (str, int)), position_row
        result.append(
            UserPosition(
                entity_id=f"cached:{user}:{index}:{token_id}",
                user=user,
                token_id=token_id,
                amount_raw=int(amount_raw_value),
            )
        )
    result.sort(key=lambda item: item.token_id)
    return result


def load_cached_positions(previous_output: dict[str, Any]) -> tuple[list[str], dict[str, list[UserPosition]]]:
    users_payload = previous_output.get("users")
    assert isinstance(users_payload, list), type(users_payload)
    users: list[str] = []
    positions_by_user: dict[str, list[UserPosition]] = {}
    for user_payload in users_payload:
        assert isinstance(user_payload, dict), user_payload
        user_value = user_payload.get("user")
        assert isinstance(user_value, str), user_payload
        user = normalize_address(user_value)
        users.append(user)
        positions_by_user[user] = load_cached_user_positions(user, user_payload)
    return users, positions_by_user


def build_cached_condition_index(previous_output: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    if previous_output is None:
        return {}
    conditions = previous_output.get("conditions")
    if not isinstance(conditions, list):
        return {}

    result: dict[str, dict[str, Any]] = {}
    for row in conditions:
        if not isinstance(row, dict):
            continue
        condition_id = row.get("condition_id")
        if not isinstance(condition_id, str) or not condition_id:
            continue
        token_ids_value = row.get("token_ids")
        payout_numerators_value = row.get("payout_numerators")
        market_outcomes_value = row.get("market_outcomes")
        result[condition_id] = {
            "condition_id": condition_id,
            "question_id": row.get("question_id") if isinstance(row.get("question_id"), str) else None,
            "outcome_slot_count": parse_int(row.get("outcome_slot_count")),
            "resolution_timestamp": parse_int(row.get("resolution_timestamp")),
            "token_ids": [str(item) for item in token_ids_value] if isinstance(token_ids_value, list) else [],
            "payout_numerators": [int(item) for item in payout_numerators_value] if isinstance(payout_numerators_value, list) else [],
            "payout_denominator": normalize_payout_denominator(parse_int(row.get("payout_denominator"))),
            "market_question": clean_text(row.get("market_question")),
            "market_description": clean_text(row.get("market_description")),
            "market_event_title": clean_text(row.get("market_event_title")),
            "market_slug": clean_text(row.get("market_slug")),
            "market_url": clean_text(row.get("market_url")),
            "market_outcomes": [str(item) for item in market_outcomes_value] if isinstance(market_outcomes_value, list) else [],
        }
    return result


def build_cached_token_data(previous_output: dict[str, Any] | None) -> tuple[dict[str, PricePoint], dict[str, MarketData]]:
    if previous_output is None:
        return {}, {}
    tokens = previous_output.get("tokens")
    if not isinstance(tokens, list):
        return {}, {}

    condition_index = build_cached_condition_index(previous_output)
    price_map: dict[str, PricePoint] = {}
    market_map: dict[str, MarketData] = {}
    for row in tokens:
        if not isinstance(row, dict):
            continue
        token_id = row.get("token_id")
        if not isinstance(token_id, str) or not token_id:
            continue
        price = parse_decimal(row.get("price"))
        if price is not None and token_id not in price_map:
            price_map[token_id] = PricePoint(price, "cached")
        condition_id = row.get("condition_id")
        condition_row = (
            condition_index[condition_id]
            if isinstance(condition_id, str) and condition_id in condition_index
            else None
        )
        outcome_index = (
            condition_row["token_ids"].index(token_id)
            if condition_row is not None and token_id in condition_row["token_ids"]
            else None
        )
        market_map[token_id] = MarketData(
            token_id=token_id,
            condition_id=condition_id if isinstance(condition_id, str) else None,
            question_id=condition_row["question_id"] if condition_row is not None else None,
            outcome_slot_count=condition_row["outcome_slot_count"] if condition_row is not None else None,
            resolution_timestamp=condition_row["resolution_timestamp"] if condition_row is not None else None,
            outcome_index=outcome_index,
            price_orderbook=None,
            payout_numerators=list(condition_row["payout_numerators"]) if condition_row is not None else [],
            payout_denominator=condition_row["payout_denominator"] if condition_row is not None else None,
        )
    return price_map, market_map


def filter_cached_token_data(
    previous_output: dict[str, Any] | None,
    token_ids: list[str],
) -> tuple[dict[str, PricePoint], dict[str, MarketData]]:
    cached_price_map, cached_market_map = build_cached_token_data(previous_output)
    token_id_set = set(token_ids)
    filtered_price_map = {
        token_id: point
        for token_id, point in cached_price_map.items()
        if token_id in token_id_set
    }
    filtered_market_map = {
        token_id: market
        for token_id, market in cached_market_map.items()
        if token_id in token_id_set
    }
    return filtered_price_map, filtered_market_map


def clean_text(value: Any) -> str:
    return str(value or "").replace("\n", " ").strip()


def fetch_market_meta_by_condition(condition_id: str) -> dict[str, Any]:
    request = Request(
        f"{GAMMA_API_BASE}/markets?condition_ids={condition_id}&include_tag=true",
        headers={"User-Agent": "poly-aggregate-exposure"},
    )
    with urlopen(request, timeout=10) as response:
        data = json.loads(response.read().decode("utf-8"))
    assert data, condition_id
    market = data[0]
    events = market.get("events", [])
    event0 = events[0] if events else {}
    question = market.get("question")
    if question:
        market_question = clean_text(question)
    else:
        event_title = event0.get("title")
        market_question = clean_text(event_title) if event_title else ""
    outcomes_value = market.get("outcomes", "[]")
    market_outcomes = (
        json.loads(outcomes_value)
        if isinstance(outcomes_value, str)
        else (outcomes_value or [])
    )
    assert isinstance(market_outcomes, list), type(market_outcomes)
    slug = event0.get("slug") or market.get("slug")
    return {
        "condition_id": condition_id,
        "market_question": market_question,
        "market_description": clean_text(market.get("description")),
        "market_event_title": clean_text(event0.get("title")),
        "market_slug": slug or "",
        "market_url": f"https://polymarket.com/event/{slug}" if slug else "",
        "market_outcomes": market_outcomes,
    }


def build_cached_condition_market_meta(previous_output: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    condition_index = build_cached_condition_index(previous_output)
    result: dict[str, dict[str, Any]] = {}
    for condition_id, row in condition_index.items():
        meta = {
            "condition_id": condition_id,
            "market_question": row["market_question"],
            "market_description": row["market_description"],
            "market_event_title": row["market_event_title"],
            "market_slug": row["market_slug"],
            "market_url": row["market_url"],
            "market_outcomes": row["market_outcomes"],
        }
        if not any([
            meta["market_question"],
            meta["market_description"],
            meta["market_event_title"],
            meta["market_slug"],
            meta["market_url"],
            meta["market_outcomes"],
        ]):
            continue
        result[condition_id] = meta
    return result


def cached_condition_row_is_complete(row: dict[str, Any]) -> bool:
    token_ids = row["token_ids"]
    outcome_slot_count = row["outcome_slot_count"]
    if outcome_slot_count is None:
        return bool(token_ids)
    return len(token_ids) == outcome_slot_count


def build_cached_condition_metas(
    previous_output: dict[str, Any] | None,
    condition_ids: list[str],
) -> dict[str, ConditionMeta]:
    condition_index = build_cached_condition_index(previous_output)
    result: dict[str, ConditionMeta] = {}
    for condition_id in condition_ids:
        row = condition_index.get(condition_id)
        if row is None or not cached_condition_row_is_complete(row):
            continue
        result[condition_id] = ConditionMeta(
            condition_id=condition_id,
            position_ids=list(row["token_ids"]),
            payout_numerators=list(row["payout_numerators"]),
            payout_denominator=row["payout_denominator"],
        )
    return result


def write_output_json(output: dict[str, Any]) -> None:
    OUTPUT_JSON_TMP_FILE.write_text(
        json.dumps(output, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    OUTPUT_JSON_TMP_FILE.replace(OUTPUT_JSON_FILE)
    progress_step("write_json", 1, 1)


def resolve_snapshot_block(api_key: str, requested_block: int | None) -> int:
    if requested_block is not None:
        assert requested_block > 0, requested_block
        progress_step("resolve_snapshot_block", 1, 1)
        return requested_block
    progress_step("resolve_snapshot_block", 0, 2)
    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = {
            executor.submit(gql, api_key, PNL_SUBGRAPH_ID, META_QUERY, {}, "pnl.meta"): "pnl",
            executor.submit(gql, api_key, POLYMARKET_SUBGRAPH_ID, META_QUERY, {}, "polymarket.meta"): "polymarket",
        }
        blocks: dict[str, int] = {}
        done = 0
        for future in as_completed(futures):
            blocks[futures[future]] = int(future.result()["_meta"]["block"]["number"])
            done += 1
            progress_step("resolve_snapshot_block", done, 2)
    snapshot_block = min(
        blocks["pnl"], blocks["polymarket"]) - 64
    assert snapshot_block > 0, blocks
    return snapshot_block


def fetch_user_positions_shard(
    api_key: str,
    users: list[str],
    snapshot_block: int,
    shard_index: int,
    shard_count: int,
) -> list[UserPosition]:
    after = ""
    page = 0
    rows: list[UserPosition] = []
    while True:
        page += 1
        data = gql(
            api_key,
            PNL_SUBGRAPH_ID,
            USER_POSITIONS_QUERY,
            {
                "users": users,
                "after": after,
                "block": snapshot_block,
                "first": USER_POSITIONS_PAGE_LIMIT,
            },
            f"pnl.userPositions.shard={shard_index}/{shard_count}.page={page}",
        )
        batch = data["userPositions"]
        for item in batch:
            rows.append(
                UserPosition(
                    entity_id=item["id"],
                    user=normalize_address(item["user"]),
                    token_id=item["tokenId"],
                    amount_raw=int(item["amount"]),
                )
            )
        if len(batch) < USER_POSITIONS_PAGE_LIMIT:
            return rows
        after = batch[-1]["id"]


def fetch_all_user_positions(api_key: str, users: list[str], snapshot_block: int) -> list[UserPosition]:
    user_groups = chunked(users, USER_QUERY_BATCH_LIMIT)
    rows: list[UserPosition] = []
    shard_results = run_parallel_indexed(
        user_groups,
        MAX_WORKERS,
        lambda index, total, group: fetch_user_positions_shard(
            api_key,
            group,
            snapshot_block,
            index,
            total,
        ),
        progress_name="fetch_user_positions",
        progress_total=len(users),
        progress_count=len,
    )
    for _, _, shard_rows in shard_results:
        rows.extend(shard_rows)
    rows.sort(key=lambda item: item.entity_id)
    return rows


def fetch_market_data_batch(
    api_key: str,
    token_ids: list[str],
    snapshot_block: int | None,
    label: str,
) -> dict[str, MarketData]:
    variables: dict[str, Any] = {"ids": token_ids}
    query = MARKET_DATA_LATEST_QUERY
    if snapshot_block is not None:
        variables["block"] = snapshot_block
        query = MARKET_DATA_QUERY
    data = gql(
        api_key,
        POLYMARKET_SUBGRAPH_ID,
        query,
        variables,
        label,
    )
    result: dict[str, MarketData] = {}
    for item in data["marketDatas"]:
        condition = item["condition"]
        result[item["id"]] = MarketData(
            token_id=item["id"],
            condition_id=condition["id"] if condition else None,
            question_id=condition["questionId"] if condition else None,
            outcome_slot_count=parse_int(
                condition["outcomeSlotCount"]) if condition else None,
            resolution_timestamp=parse_int(
                condition["resolutionTimestamp"]) if condition else None,
            outcome_index=parse_int(item["outcomeIndex"]),
            price_orderbook=parse_decimal(item["priceOrderbook"]),
            payout_numerators=[int(x) for x in (
                condition["payoutNumerators"] or [])] if condition else [],
            payout_denominator=normalize_payout_denominator(
                parse_int(condition["payoutDenominator"])
            ) if condition else None,
        )
    return result


def fetch_market_data(
    api_key: str,
    token_ids: list[str],
    label_prefix: str,
    snapshot_block: int | None,
) -> dict[str, MarketData]:
    unique_token_ids = sorted(set(token_ids))
    if not unique_token_ids:
        return {}
    groups = chunked(unique_token_ids, ID_QUERY_BATCH_LIMIT)
    result: dict[str, MarketData] = {}
    progress_name = {
        "held": "fetch_market_data_held",
        "sibling": "fetch_market_data_sibling",
    }.get(label_prefix)
    label_suffix = "latest" if snapshot_block is None else "snapshot"
    chunk_results = run_parallel_indexed(
        groups,
        MAX_WORKERS,
        lambda index, total, group: fetch_market_data_batch(
            api_key,
            group,
            snapshot_block,
            f"{label_prefix}.marketDatas.{label_suffix}.chunk={index}/{total} ids={len(group)}",
        ),
        progress_name=progress_name,
        progress_total=len(unique_token_ids),
        progress_count=len,
    )
    for _, _, chunk_rows in chunk_results:
        result.update(chunk_rows)
    return result


def fetch_conditions_batch(
    api_key: str,
    condition_ids: list[str],
    snapshot_block: int | None,
    label: str,
) -> dict[str, ConditionMeta]:
    variables: dict[str, Any] = {"ids": condition_ids}
    query = CONDITIONS_LATEST_QUERY
    if snapshot_block is not None:
        variables["block"] = snapshot_block
        query = CONDITIONS_QUERY
    data = gql(
        api_key,
        PNL_SUBGRAPH_ID,
        query,
        variables,
        label,
    )
    result: dict[str, ConditionMeta] = {}
    for item in data["conditions"]:
        result[item["id"]] = ConditionMeta(
            condition_id=item["id"],
            position_ids=[str(x) for x in item["positionIds"]],
            payout_numerators=[int(x) for x in item["payoutNumerators"]],
            payout_denominator=normalize_payout_denominator(parse_int(item["payoutDenominator"])),
        )
    return result


def fetch_conditions(
    api_key: str,
    condition_ids: list[str],
    snapshot_block: int | None,
) -> dict[str, ConditionMeta]:
    unique_condition_ids = sorted(set(condition_ids))
    if not unique_condition_ids:
        return {}
    groups = chunked(unique_condition_ids, ID_QUERY_BATCH_LIMIT)
    result: dict[str, ConditionMeta] = {}
    label_suffix = "latest" if snapshot_block is None else "snapshot"
    chunk_results = run_parallel_indexed(
        groups,
        MAX_WORKERS,
        lambda index, total, group: fetch_conditions_batch(
            api_key,
            group,
            snapshot_block,
            f"pnl.conditions.{label_suffix}.chunk={index}/{total} ids={len(group)}",
        ),
        progress_name="fetch_conditions",
        progress_total=len(unique_condition_ids),
        progress_count=len,
    )
    for _, _, chunk_rows in chunk_results:
        result.update(chunk_rows)
    return result


def resolved_price_from_payouts(
    payout_numerators: list[int],
    payout_denominator: int | None,
    outcome_index: int | None,
) -> Decimal | None:
    if payout_denominator is None or payout_denominator == 0:
        return None
    if outcome_index is None:
        return None
    if outcome_index < 0 or outcome_index >= len(payout_numerators):
        return None
    return Decimal(payout_numerators[outcome_index]) / Decimal(payout_denominator)


def resolved_price_for_market(
    market: MarketData,
    conditions: dict[str, ConditionMeta],
) -> Decimal | None:
    condition = conditions.get(market.condition_id or "")
    if condition is not None:
        condition_resolved_price = resolved_price_from_payouts(
            condition.payout_numerators,
            condition.payout_denominator,
            market.outcome_index,
        )
        if condition_resolved_price is not None:
            return condition_resolved_price
    return resolved_price_from_payouts(
        market.payout_numerators,
        market.payout_denominator,
        market.outcome_index,
    )


def build_direct_price_map(
    markets: dict[str, MarketData],
    conditions: dict[str, ConditionMeta],
) -> dict[str, PricePoint]:
    prices: dict[str, PricePoint] = {}
    for token_id, market in markets.items():
        resolved_price = resolved_price_for_market(market, conditions)
        if resolved_price is not None:
            prices[token_id] = PricePoint(resolved_price, "resolution")
            continue
        if market.price_orderbook is not None:
            prices[token_id] = PricePoint(market.price_orderbook, "orderbook")
    return prices


def infer_sum_to_one_prices(
    prices: dict[str, PricePoint],
    conditions: dict[str, ConditionMeta],
) -> dict[str, PricePoint]:
    merged = dict(prices)
    for condition in conditions.values():
        if not condition.position_ids:
            continue
        known_total = ZERO
        missing: list[str] = []
        for token_id in condition.position_ids:
            point = merged.get(token_id)
            if point is None:
                missing.append(token_id)
                continue
            known_total += point.value
        if len(missing) != 1:
            continue
        inferred = ONE - known_total
        if ZERO <= inferred <= ONE:
            merged[missing[0]] = PricePoint(inferred, "sum_to_one")
    return merged


def group_positions_by_user(
    users: list[str],
    positions: list[UserPosition],
) -> dict[str, list[UserPosition]]:
    result: dict[str, list[UserPosition]] = {user: [] for user in users}
    for position in positions:
        result[position.user].append(position)
    for user in users:
        result[user].sort(key=lambda item: item.token_id)
    return result


def merge_price_maps(
    base_prices: dict[str, PricePoint],
    new_prices: dict[str, PricePoint],
    preserve_base: bool,
) -> dict[str, PricePoint]:
    merged = dict(base_prices)
    if preserve_base:
        for token_id, point in new_prices.items():
            merged.setdefault(token_id, point)
        return merged
    merged.update(new_prices)
    return merged


def prepare_prices(
    held_token_ids: list[str],
    market_token_ids_to_fetch: list[str],
    fetch_market_rows: Callable[[list[str], str], dict[str, MarketData]],
    fetch_condition_rows: Callable[[list[str]], dict[str, ConditionMeta]],
    existing_prices: dict[str, PricePoint] | None = None,
    existing_markets: dict[str, MarketData] | None = None,
    existing_prices_are_authoritative: bool = False,
) -> tuple[dict[str, PricePoint], dict[str, MarketData], dict[str, ConditionMeta]]:
    def mark_condition_and_sibling_fetch_skipped() -> None:
        progress_step("fetch_conditions", 0, 0)
        progress_step("fetch_market_data_sibling", 0, 0)

    def finalize(
        prices: dict[str, PricePoint],
        markets: dict[str, MarketData],
        condition_metas: dict[str, ConditionMeta],
    ) -> tuple[dict[str, PricePoint], dict[str, MarketData], dict[str, ConditionMeta]]:
        final_prices = dict(prices)
        if not existing_prices_are_authoritative:
            for token_id, point in existing_price_map.items():
                final_prices.setdefault(token_id, point)
        progress_step("prepare_prices", 1, 1)
        return final_prices, markets, condition_metas

    progress_step("prepare_prices", 0, 1)
    unique_held_token_ids = sorted(set(held_token_ids))
    unique_market_token_ids_to_fetch = sorted(set(market_token_ids_to_fetch))
    existing_price_map = dict(existing_prices or {})
    known_markets = dict(existing_markets or {})

    if unique_market_token_ids_to_fetch:
        held_markets = fetch_market_rows(unique_market_token_ids_to_fetch, "held")
    else:
        held_markets = {}
        progress_step("fetch_market_data_held", 0, 0)
    known_markets.update(held_markets)

    market_token_ids = [
        token_id for token_id in unique_held_token_ids if token_id in known_markets
    ]
    initial_prices = merge_price_maps(
        existing_price_map if existing_prices_are_authoritative else {},
        build_direct_price_map(known_markets, {}),
        existing_prices_are_authoritative,
    )
    if not market_token_ids:
        mark_condition_and_sibling_fetch_skipped()
        return finalize(initial_prices, known_markets, {})

    missing_price_tokens = [token_id for token_id in market_token_ids if token_id not in initial_prices]
    if not missing_price_tokens:
        mark_condition_and_sibling_fetch_skipped()
        return finalize(initial_prices, known_markets, {})

    tokens_with_condition = [
        token_id
        for token_id in missing_price_tokens
        if known_markets[token_id].condition_id
    ]
    if not tokens_with_condition:
        mark_condition_and_sibling_fetch_skipped()
        return finalize(initial_prices, known_markets, {})

    missing_condition_ids = sorted(
        {
            known_markets[token_id].condition_id
            for token_id in tokens_with_condition
            if known_markets[token_id].condition_id
        }
    )
    condition_metas = fetch_condition_rows(missing_condition_ids)

    sibling_token_ids = sorted(
        {
            token_id
            for condition in condition_metas.values()
            for token_id in condition.position_ids
            if token_id not in known_markets
        }
    )

    if sibling_token_ids:
        sibling_markets = fetch_market_rows(sibling_token_ids, "sibling")
    else:
        sibling_markets = {}
        progress_step("fetch_market_data_sibling", 0, 0)

    merged_markets = dict(known_markets)
    merged_markets.update(sibling_markets)

    prices = merge_price_maps(
        existing_price_map if existing_prices_are_authoritative else {},
        build_direct_price_map(merged_markets, condition_metas),
        existing_prices_are_authoritative,
    )
    prices = infer_sum_to_one_prices(prices, condition_metas)
    return finalize(prices, merged_markets, condition_metas)


def group_markets_by_condition(markets: dict[str, MarketData]) -> dict[str, list[MarketData]]:
    result: dict[str, list[MarketData]] = defaultdict(list)
    for market in markets.values():
        if market.condition_id is None:
            continue
        result[market.condition_id].append(market)
    return result


def infer_complete_condition_token_ids_from_markets(
    condition_markets: list[MarketData],
    outcome_slot_count: int | None,
) -> list[str]:
    if outcome_slot_count is None:
        return []
    token_ids_by_outcome_index: dict[int, str] = {}
    for market in condition_markets:
        if market.outcome_index is None:
            continue
        if market.outcome_index < 0 or market.outcome_index >= outcome_slot_count:
            continue
        previous_token_id = token_ids_by_outcome_index.get(market.outcome_index)
        if previous_token_id is None:
            token_ids_by_outcome_index[market.outcome_index] = market.token_id
            continue
        if previous_token_id != market.token_id:
            return []
    if len(token_ids_by_outcome_index) != outcome_slot_count:
        return []
    return [token_ids_by_outcome_index[index] for index in range(outcome_slot_count)]


def condition_markets_have_conflict(condition_markets: list[MarketData]) -> bool:
    token_ids_by_outcome_index: dict[int, str] = {}
    for market in condition_markets:
        if market.outcome_index is None or market.outcome_index < 0:
            continue
        previous_token_id = token_ids_by_outcome_index.get(market.outcome_index)
        if previous_token_id is None:
            token_ids_by_outcome_index[market.outcome_index] = market.token_id
            continue
        if previous_token_id != market.token_id:
            return True
    return False


def build_condition_output_rows(
    markets: dict[str, MarketData],
    condition_metas: dict[str, ConditionMeta],
    previous_output: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    def merge_optional_scalar(
        condition_id: str,
        field_name: str,
        current: Any,
        incoming: Any,
    ) -> Any:
        if current is not None and incoming is not None:
            assert current == incoming, (condition_id, field_name, current, incoming)
            return current
        if current is None:
            return incoming
        return current

    def merge_optional_list(
        condition_id: str,
        field_name: str,
        current: list[Any],
        incoming: list[Any],
    ) -> list[Any]:
        if current and incoming:
            assert current == incoming, (condition_id, field_name, current, incoming)
            return current
        if not current:
            return list(incoming)
        return current

    cached_condition_index = build_cached_condition_index(previous_output)
    markets_by_condition = group_markets_by_condition(markets)

    rows: list[dict[str, Any]] = []
    all_condition_ids = sorted(set(condition_metas) | set(markets_by_condition))
    for condition_id in all_condition_ids:
        cached_row = cached_condition_index.get(condition_id)
        condition_meta = condition_metas.get(condition_id)
        condition_markets = sorted(
            markets_by_condition.get(condition_id, []),
            key=lambda item: (
                sys.maxsize if item.outcome_index is None else item.outcome_index,
                item.token_id,
            ),
        )
        question_id = cached_row["question_id"] if cached_row is not None else None
        outcome_slot_count = cached_row["outcome_slot_count"] if cached_row is not None else None
        resolution_timestamp = cached_row["resolution_timestamp"] if cached_row is not None else None
        payout_numerators = list(cached_row["payout_numerators"]) if cached_row is not None else []
        payout_denominator = cached_row["payout_denominator"] if cached_row is not None else None
        for market in condition_markets:
            question_id = merge_optional_scalar(
                condition_id, "question_id", question_id, market.question_id
            )
            outcome_slot_count = merge_optional_scalar(
                condition_id, "outcome_slot_count", outcome_slot_count, market.outcome_slot_count
            )
            resolution_timestamp = merge_optional_scalar(
                condition_id, "resolution_timestamp", resolution_timestamp, market.resolution_timestamp
            )
            payout_numerators = merge_optional_list(
                condition_id, "payout_numerators", payout_numerators, market.payout_numerators
            )
            payout_denominator = merge_optional_scalar(
                condition_id, "payout_denominator", payout_denominator, market.payout_denominator
            )
        if condition_meta is not None:
            payout_numerators = merge_optional_list(
                condition_id, "payout_numerators", payout_numerators, condition_meta.payout_numerators
            )
            payout_denominator = merge_optional_scalar(
                condition_id, "payout_denominator", payout_denominator, condition_meta.payout_denominator
            )
        inferred_market_token_ids = infer_complete_condition_token_ids_from_markets(
            condition_markets,
            outcome_slot_count,
        )
        token_ids = (
            list(condition_meta.position_ids)
            if condition_meta is not None
            else (
                list(cached_row["token_ids"])
                if cached_row is not None and cached_condition_row_is_complete(cached_row)
                else list(inferred_market_token_ids)
            )
        )
        if not token_ids:
            token_ids = [market.token_id for market in condition_markets]
        if outcome_slot_count is not None and len(token_ids) > outcome_slot_count:
            assert inferred_market_token_ids, (condition_id, outcome_slot_count, token_ids)
            token_ids = inferred_market_token_ids

        rows.append(
            {
                "condition_id": condition_id,
                "question_id": question_id,
                "outcome_slot_count": outcome_slot_count,
                "resolution_timestamp": resolution_timestamp,
                "payout_numerators": payout_numerators,
                "payout_denominator": payout_denominator,
                "token_ids": token_ids,
                "market_question": cached_row["market_question"] if cached_row is not None else "",
                "market_description": cached_row["market_description"] if cached_row is not None else "",
                "market_event_title": cached_row["market_event_title"] if cached_row is not None else "",
                "market_slug": cached_row["market_slug"] if cached_row is not None else "",
                "market_url": cached_row["market_url"] if cached_row is not None else "",
                "market_outcomes": list(cached_row["market_outcomes"]) if cached_row is not None else [],
            }
        )
    return rows


def build_token_output_rows(
    price_map: dict[str, PricePoint],
    markets: dict[str, MarketData],
    condition_metas: dict[str, ConditionMeta],
) -> list[dict[str, Any]]:
    token_owner_map: dict[str, str] = {}
    for condition_id, condition_meta in condition_metas.items():
        for token_id in condition_meta.position_ids:
            previous_owner = token_owner_map.get(token_id)
            assert previous_owner in {None, condition_id}, (token_id, previous_owner, condition_id)
            token_owner_map[token_id] = condition_id
    token_ids = sorted(set(markets) | set(price_map) | set(token_owner_map))
    rows: list[dict[str, Any]] = []
    for token_id in token_ids:
        market = markets.get(token_id)
        price_point = price_map.get(token_id)
        rows.append(
            {
                "token_id": token_id,
                "condition_id": market.condition_id if market is not None else token_owner_map.get(token_id),
                "price": format_decimal(price_point.value, 10) if price_point is not None else None,
            }
        )
    rows.sort(
        key=lambda item: (
            item["condition_id"] or "",
            item["token_id"],
        )
    )
    return rows


def filter_and_normalize_aggregate_rows(output: dict[str, Any]) -> dict[str, Any]:
    progress_step("filter_and_normalize", 0, 1)
    raw_rows = output["aggregate"]
    retained_rows = [
        dict(row)
        for row in raw_rows
        if Decimal(row["aggregate_weight"]) >= AGGREGATE_WEIGHT_THRESHOLD
    ]
    retained_weight_before_normalize = sum(
        (Decimal(row["aggregate_weight"]) for row in retained_rows),
        ZERO,
    )
    if retained_rows and retained_weight_before_normalize > ZERO:
        for row in retained_rows:
            normalized_weight = Decimal(
                row["aggregate_weight"]) / retained_weight_before_normalize
            row["aggregate_weight_raw"] = row["aggregate_weight"]
            row["aggregate_weight"] = format_decimal(normalized_weight, 10)
        retained_rows.sort(key=lambda item: Decimal(
            item["aggregate_weight"]), reverse=True)

    output["summary"]["token_count_before_filter"] = len(raw_rows)
    output["summary"]["token_count_after_filter"] = len(retained_rows)
    output["summary"]["token_count"] = len(retained_rows)
    output["summary"]["filtered_out_token_count"] = len(
        raw_rows) - len(retained_rows)
    output["summary"]["aggregate_weight_threshold"] = format_decimal(
        AGGREGATE_WEIGHT_THRESHOLD, 10)
    output["summary"]["retained_weight_before_normalize"] = format_decimal(
        retained_weight_before_normalize, 10
    )
    output["summary"]["aggregate_weight_sum_after_normalize"] = format_decimal(
        sum((Decimal(row["aggregate_weight"]) for row in retained_rows), ZERO),
        10,
    )
    output["aggregate"] = retained_rows
    progress_step("filter_and_normalize", 1, 1)
    return output


def enrich_top_conditions(
    output: dict[str, Any],
    previous_output: dict[str, Any] | None,
) -> dict[str, Any]:
    token_rows = output["tokens"]
    condition_rows = output["conditions"]
    token_condition_map = {
        row["token_id"]: row.get("condition_id")
        for row in token_rows
        if isinstance(row, dict) and isinstance(row.get("token_id"), str)
    }
    condition_row_map = {
        row["condition_id"]: row
        for row in condition_rows
        if isinstance(row, dict) and isinstance(row.get("condition_id"), str)
    }
    condition_ids = sorted(
        {
            token_condition_map[row["token_id"]]
            for row in output["aggregate"]
            if row["token_id"] in token_condition_map and token_condition_map[row["token_id"]]
        }
    )
    condition_meta_map = build_cached_condition_market_meta(previous_output)
    missing_condition_ids = [
        condition_id for condition_id in condition_ids if condition_id not in condition_meta_map
    ]
    if missing_condition_ids:
        condition_results = run_parallel_indexed(
            missing_condition_ids,
            MARKET_META_MAX_WORKERS,
            lambda _index, _total, condition_id: fetch_market_meta_by_condition(
                condition_id),
            progress_name="fetch_gamma_market_meta",
        )
        for _, condition_id, market_meta in condition_results:
            condition_meta_map[condition_id] = market_meta
    else:
        progress_step("fetch_gamma_market_meta", 0, 0)

    for condition_id in condition_ids:
        row = condition_row_map.get(condition_id)
        if row is None:
            continue
        if condition_id in condition_meta_map:
            row.update(condition_meta_map[condition_id])

    output["summary"]["top_market_meta_count"] = sum(
        1
        for condition_id in condition_ids
        if condition_id in condition_meta_map and condition_id in condition_row_map
    )
    return output


def compute_output(
    users: list[str],
    positions_by_user: dict[str, list[UserPosition]],
    price_map: dict[str, PricePoint],
    markets: dict[str, MarketData],
    condition_metas: dict[str, ConditionMeta],
    previous_output: dict[str, Any] | None,
) -> dict[str, Any]:
    progress_step("compute_output", 0, 1)
    resolved_token_ids = {
        token_id
        for token_id, market in markets.items()
        if (
            market.resolution_timestamp is not None
            or resolved_price_for_market(market, condition_metas) is not None
        )
    }
    for condition_meta in condition_metas.values():
        if (
            condition_meta.payout_denominator is not None
            and condition_meta.payout_denominator != 0
            and bool(condition_meta.payout_numerators)
        ):
            resolved_token_ids.update(condition_meta.position_ids)
    active_price_map = {
        token_id: point
        for token_id, point in price_map.items()
        if token_id not in resolved_token_ids
    }
    active_markets = {
        token_id: market
        for token_id, market in markets.items()
        if token_id not in resolved_token_ids
    }
    active_condition_metas: dict[str, ConditionMeta] = {}
    for condition_id, condition_meta in condition_metas.items():
        active_position_ids = [
            token_id
            for token_id in condition_meta.position_ids
            if token_id not in resolved_token_ids
        ]
        if not active_position_ids:
            continue
        active_condition_metas[condition_id] = ConditionMeta(
            condition_id=condition_meta.condition_id,
            position_ids=active_position_ids,
            payout_numerators=list(condition_meta.payout_numerators),
            payout_denominator=condition_meta.payout_denominator,
        )
    user_outputs: list[dict[str, Any]] = []
    aggregate_weight_sum: dict[str, Decimal] = defaultdict(lambda: ZERO)
    holder_count: dict[str, int] = defaultdict(int)
    position_count_total = 0
    user_count_decimal = Decimal(len(users))
    zero_value_users: list[str] = []
    ignored_token_ids: set[str] = set()
    ignored_position_count = 0
    priced_position_count = 0

    for user in users:
        user_positions = [
            position
            for position in positions_by_user[user]
            if position.token_id not in resolved_token_ids
        ]
        position_count_total += len(user_positions)
        position_rows = [
            {"token_id": position.token_id, "amount_raw": str(position.amount_raw)}
            for position in user_positions
        ]
        token_entries: list[tuple[UserPosition, Decimal]] = []
        total_value = ZERO
        for position in user_positions:
            price_point = active_price_map.get(position.token_id)
            market = active_markets.get(position.token_id)
            if price_point is None or market is None:
                ignored_token_ids.add(position.token_id)
                ignored_position_count += 1
                continue
            amount = Decimal(position.amount_raw) / UNIT
            value = amount * price_point.value
            token_entries.append((position, value))
            total_value += value
            holder_count[position.token_id] += 1
            priced_position_count += 1

        user_outputs.append({"user": user, "positions": position_rows})
        if total_value == ZERO:
            zero_value_users.append(user)
            continue

        for position, value in token_entries:
            weight = value / total_value
            aggregate_weight_sum[position.token_id] += weight

    aggregate_rows: list[dict[str, Any]] = []
    for token_id, weight_sum in aggregate_weight_sum.items():
        aggregate_rows.append(
            {
                "token_id": token_id,
                "holder_count": holder_count[token_id],
                "holder_ratio": format_decimal(
                    Decimal(holder_count[token_id]) / user_count_decimal, 10
                ),
                "aggregate_weight": format_decimal(weight_sum / user_count_decimal, 10),
            }
        )
    aggregate_rows.sort(key=lambda item: Decimal(
        item["aggregate_weight"]), reverse=True)
    token_rows = build_token_output_rows(
        active_price_map,
        active_markets,
        active_condition_metas,
    )
    condition_rows = build_condition_output_rows(
        active_markets,
        active_condition_metas,
        previous_output,
    )

    result = {
        "summary": {
            "user_count": len(users),
            "position_count": position_count_total,
            "priced_position_count": priced_position_count,
            "ignored_position_count": ignored_position_count,
            "ignored_token_count": len(ignored_token_ids),
            "token_count": len(aggregate_rows),
            "token_record_count": len(token_rows),
            "condition_count": len(condition_rows),
            "zero_value_user_count": len(zero_value_users),
            "zero_value_users": zero_value_users,
        },
        "conditions": condition_rows,
        "tokens": token_rows,
        "aggregate": aggregate_rows,
        "users": user_outputs,
    }
    progress_step("compute_output", 1, 1)
    return result


def portfolio_token_ids(positions_by_user: dict[str, list[UserPosition]]) -> list[str]:
    return sorted(
        {
            position.token_id
            for positions in positions_by_user.values()
            for position in positions
        }
    )


def load_cached_portfolio_state(previous_output: dict[str, Any]) -> PortfolioState:
    assert output_uses_normalized_schema(previous_output), str(OUTPUT_JSON_FILE)
    users, positions_by_user = load_cached_positions(previous_output)
    summary = previous_output.get("summary")
    assert isinstance(summary, dict), previous_output
    snapshot_block_value = summary.get("snapshot_block")
    assert isinstance(snapshot_block_value, int), summary
    return PortfolioState(
        users=users,
        positions_by_user=positions_by_user,
        token_ids=portfolio_token_ids(positions_by_user),
        snapshot_block=snapshot_block_value,
    )


def load_live_portfolio_state(
    api_key: str,
    requested_snapshot_block: int | None,
) -> PortfolioState:
    users = load_addresses()
    snapshot_block = resolve_snapshot_block(api_key, requested_snapshot_block)
    positions = fetch_all_user_positions(api_key, users, snapshot_block)
    positions_by_user = group_positions_by_user(users, positions)
    return PortfolioState(
        users=users,
        positions_by_user=positions_by_user,
        token_ids=portfolio_token_ids(positions_by_user),
        snapshot_block=snapshot_block,
    )


def load_portfolio_state(
    api_key: str,
    previous_output: dict[str, Any] | None,
    spec: FlowSpec,
    requested_snapshot_block: int | None,
) -> PortfolioState:
    if spec.use_cached_positions:
        assert previous_output is not None, str(OUTPUT_JSON_FILE)
        return load_cached_portfolio_state(previous_output)
    return load_live_portfolio_state(api_key, requested_snapshot_block)


def select_market_token_ids_to_fetch(
    token_ids: list[str],
    cached_price_map: dict[str, PricePoint],
    cached_market_map: dict[str, MarketData],
    refresh_all_prices: bool,
) -> list[str]:
    if refresh_all_prices:
        return token_ids
    return [
        token_id
        for token_id in token_ids
        if token_id not in cached_price_map or token_id not in cached_market_map
    ]


def split_preserved_resolution_prices(
    token_ids: list[str],
    cached_price_map: dict[str, PricePoint],
    cached_market_map: dict[str, MarketData],
    refresh_all_prices: bool,
) -> tuple[list[str], dict[str, PricePoint], dict[str, MarketData]]:
    if not refresh_all_prices:
        return token_ids, {}, {}
    preserved_token_ids = {
        token_id
        for token_id in token_ids
        if token_id in cached_market_map
        and token_id in cached_price_map
        and resolved_price_from_payouts(
            cached_market_map[token_id].payout_numerators,
            cached_market_map[token_id].payout_denominator,
            cached_market_map[token_id].outcome_index,
        )
        is not None
    }
    active_token_ids = [token_id for token_id in token_ids if token_id not in preserved_token_ids]
    preserved_price_map = {
        token_id: cached_price_map[token_id]
        for token_id in preserved_token_ids
    }
    preserved_market_map = {
        token_id: cached_market_map[token_id]
        for token_id in preserved_token_ids
    }
    return active_token_ids, preserved_price_map, preserved_market_map


def load_price_state(
    api_key: str,
    previous_output: dict[str, Any] | None,
    portfolio: PortfolioState,
    spec: FlowSpec,
) -> tuple[dict[str, PricePoint], dict[str, MarketData], dict[str, ConditionMeta]]:
    cached_price_map, cached_market_map = filter_cached_token_data(
        previous_output,
        portfolio.token_ids,
    )
    active_token_ids, preserved_price_map, preserved_market_map = split_preserved_resolution_prices(
        portfolio.token_ids,
        cached_price_map,
        cached_market_map,
        spec.refresh_all_prices,
    )
    active_cached_price_map = {
        token_id: point
        for token_id, point in cached_price_map.items()
        if token_id in active_token_ids
    }
    active_cached_market_map = {
        token_id: market
        for token_id, market in cached_market_map.items()
        if token_id in active_token_ids
    }
    market_token_ids_to_fetch = select_market_token_ids_to_fetch(
        active_token_ids,
        active_cached_price_map,
        active_cached_market_map,
        spec.refresh_all_prices,
    )
    price_snapshot_block = None if spec.use_latest_price_data else portfolio.snapshot_block
    price_map, market_map, condition_map = prepare_prices(
        held_token_ids=active_token_ids,
        market_token_ids_to_fetch=market_token_ids_to_fetch,
        fetch_market_rows=lambda token_ids, label_prefix: fetch_market_data(
            api_key,
            token_ids,
            label_prefix,
            price_snapshot_block,
        ),
        fetch_condition_rows=lambda condition_ids: fetch_conditions(
            api_key,
            condition_ids,
            price_snapshot_block,
        ),
        existing_prices=active_cached_price_map,
        existing_markets=active_cached_market_map,
        existing_prices_are_authoritative=not spec.refresh_all_prices,
    )
    price_map.update(preserved_price_map)
    market_map.update(preserved_market_map)
    condition_ids = sorted(
        {
            market.condition_id
            for market in market_map.values()
            if market.condition_id is not None
        }
    )
    merged_condition_map = build_cached_condition_metas(previous_output, condition_ids)
    merged_condition_map.update(condition_map)
    missing_condition_ids = [
        condition_id
        for condition_id in condition_ids
        if condition_id not in merged_condition_map
    ]
    if missing_condition_ids:
        merged_condition_map.update(
            fetch_conditions(
                api_key,
                missing_condition_ids,
                price_snapshot_block,
            )
        )
    remaining_missing_condition_ids = [
        condition_id
        for condition_id in condition_ids
        if condition_id not in merged_condition_map
    ]
    if remaining_missing_condition_ids:
        markets_by_condition = group_markets_by_condition(market_map)
        conflicted_condition_ids = [
            condition_id
            for condition_id in remaining_missing_condition_ids
            if condition_markets_have_conflict(markets_by_condition.get(condition_id, []))
        ]
        # Missing condition rows are common for foreign instruments that are not
        # indexed by Polymarket's condition subgraph. Only fail on conflicting
        # market rows that cannot represent a valid outcome mapping.
        assert not conflicted_condition_ids, conflicted_condition_ids
    return price_map, market_map, merged_condition_map


def run_output_flow(
    api_key: str,
    flow: str,
    requested_snapshot_block: int | None = None,
) -> dict[str, Any]:
    with OUTPUT_FLOW_LOCK:
        assert flow in FLOW_SPECS, flow
        spec = FLOW_SPECS[flow]
        previous_output = load_previous_output()
        global PROGRESS_BOARD
        PROGRESS_BOARD = ProgressBoard(spec.stages)

        portfolio = load_portfolio_state(
            api_key,
            previous_output,
            spec,
            requested_snapshot_block,
        )
        price_map, market_map, condition_metas = load_price_state(
            api_key,
            previous_output,
            portfolio,
            spec,
        )

        output = compute_output(
            portfolio.users,
            portfolio.positions_by_user,
            price_map,
            market_map,
            condition_metas,
            previous_output,
        )
        output["summary"]["snapshot_block"] = portfolio.snapshot_block
        output["summary"]["price_updated_at_unix_sec"] = int(time.time())
        output = filter_and_normalize_aggregate_rows(output)
        output = enrich_top_conditions(output, previous_output)
        write_output_json(output)
        return output


def build_http_handler(api_key: str) -> type[BaseHTTPRequestHandler]:
    class RebuildHandler(BaseHTTPRequestHandler):
        def _write_json(self, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _write_html(self, html_path: Path) -> None:
            body = html_path.read_text(encoding="utf-8").encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _read_json_body(self) -> dict[str, Any]:
            length_text = self.headers.get("Content-Length", "0")
            length = int(length_text)
            raw = self.rfile.read(length)
            if not raw:
                return {}
            payload = json.loads(raw.decode("utf-8"))
            assert isinstance(payload, dict), type(payload)
            return payload

        def do_GET(self) -> None:
            path = urlparse(self.path).path
            if path in {"/", "/rebuild.html"}:
                assert OUTPUT_HTML_FILE.exists(), str(OUTPUT_HTML_FILE)
                self._write_html(OUTPUT_HTML_FILE)
                return
            if path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
                return
            if path == "/api/output":
                output = load_previous_output()
                assert output is not None, str(OUTPUT_JSON_FILE)
                assert output_uses_normalized_schema(output), str(OUTPUT_JSON_FILE)
                self._write_json(output)
                return
            assert False, path

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            if path == "/api/refresh-holdings":
                payload = self._read_json_body()
                snapshot_block_value = payload.get("snapshot_block")
                snapshot_block = (
                    int(snapshot_block_value)
                    if snapshot_block_value is not None
                    else None
                )
                output = run_output_flow(
                    api_key,
                    FLOW_REFRESH_HOLDINGS_REFRESH_PRICE,
                    snapshot_block,
                )
                self._write_json(output)
                return
            if path == "/api/refresh-price":
                self._read_json_body()
                output = run_output_flow(api_key, FLOW_REFRESH_PRICE)
                self._write_json(output)
                return
            assert False, path

        def log_message(self, _format: str, *_args: Any) -> None:
            return

    return RebuildHandler


def serve_http(api_key: str, host: str, port: int) -> None:
    server = ThreadingHTTPServer((host, port), build_http_handler(api_key))
    print(f"[aggregate] serving http://{host}:{port}")
    server.serve_forever()


def main() -> None:
    api_key = os.environ.get("THE_GRAPH_API_KEY", DEFAULT_API_KEY).strip()
    assert api_key, api_key

    previous_output = load_previous_output()
    if previous_output is None or not output_uses_normalized_schema(previous_output):
        run_output_flow(api_key, FLOW_REFRESH_HOLDINGS_FILL_MISSING_PRICE, None)
    print(
        json.dumps(
            {
                "frontend_url": f"http://localhost:{DEFAULT_SERVE_PORT}/rebuild.html",
                "listen": f"http://{DEFAULT_SERVE_HOST}:{DEFAULT_SERVE_PORT}",
                "address_file": str(ADDRESS_FILE),
                "json_file": str(OUTPUT_JSON_FILE),
                "html_file": str(OUTPUT_HTML_FILE),
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    serve_http(api_key, DEFAULT_SERVE_HOST, DEFAULT_SERVE_PORT)


if __name__ == "__main__":
    main()
