#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

import duckdb

DEFAULT_FILTERS = [
    "ALL.exp100 > 10000",
    "ALL.tok100 > 5",
    "ALL.tok100 < 50",
    "ALL.vol100 > 10000",
    "ALL.hp100 < 500000",
    "ALL.hp100 > 50000",
]
DEFAULT_SORT_EXPR = "ALL.shp1k"
DEFAULT_SORT_ASC = False
DEFAULT_POOL_SIZE = 50


@dataclass(frozen=True)
class FieldBinding:
    tag_id: int
    column: str
    alias: str


@dataclass(frozen=True)
class PoolMember:
    bucket: int
    candidate_count: int
    rank: int
    addr: str
    sort_value: Optional[float]
    month_avg_tok: int
    month_avg_exp: int
    month_avg_hp: int


@dataclass(frozen=True)
class BucketSummary:
    bucket: int
    candidate_count: int
    selected_count: int
    retained_count: int
    entered_count: int
    exited_count: int
    replacement_count: int
    replacement_rate: float
    jaccard: float
    avg_sort_value: float
    min_sort_value: float
    max_sort_value: float
    members: tuple[str, ...]
    entered: tuple[str, ...]
    exited: tuple[str, ...]


def normalize_token(raw: str) -> str:
    out: list[str] = []
    prev_sep = False
    for ch in raw.lower():
        if ch.isalnum():
            out.append(ch)
            prev_sep = False
            continue
        if out and not prev_sep:
            out.append("_")
            prev_sep = True
    while out and out[-1] == "_":
        out.pop()
    return "".join(out)


def feature_token_to_column(feature_token: str) -> Optional[str]:
    token = feature_token.lower()
    if token.endswith("1k"):
        base = token[:-2]
        window = "1000"
    elif token.endswith("1000"):
        base = token[:-4]
        window = "1000"
    elif token.endswith("100"):
        base = token[:-3]
        window = "100"
    elif token.endswith("10"):
        base = token[:-2]
        window = "10"
    else:
        return None
    if not base:
        return None
    if base == "tok":
        return {
            "10": "token_avg_10w",
            "100": "token_avg_100w",
            "1000": "token_avg_1000w",
        }[window]
    if base == "exp":
        return {
            "10": "exposure_avg_10w",
            "100": "exposure_avg_100w",
            "1000": "exposure_avg_1000w",
        }[window]
    if base == "vol":
        return {
            "10": "volume_10w",
            "100": "volume_avg_100w",
            "1000": "volume_avg_1000w",
        }[window]
    if base == "hp":
        return {
            "10": "holding_period_avg_10w",
            "100": "holding_period_avg_100w",
            "1000": "holding_period_avg_1000w",
        }[window]
    if base == "shp":
        return {
            "100": "sharpe_100w",
            "1000": "sharpe_1000w",
        }[window]
    return None


def resolve_industry_tag_id(tag_to_industry_id: dict[str, int], raw_tag: str) -> Optional[int]:
    normalized = normalize_token(raw_tag)
    if not normalized:
        return None
    if normalized == "all":
        return -1
    if normalized == "unknown":
        return 13
    return tag_to_industry_id.get(normalized)


class ExprTranslator:
    def __init__(
        self,
        resolve_tag: Callable[[str], Optional[int]],
        binding_map: dict[str, FieldBinding],
        bindings: list[FieldBinding],
    ) -> None:
        self.resolve_tag = resolve_tag
        self.binding_map = binding_map
        self.bindings = bindings

    def translate(self, expr: str) -> str:
        sql: list[str] = []
        i = 0
        while i < len(expr):
            ch = expr[i]
            if ch.isspace():
                sql.append(" ")
                i += 1
                continue
            if ch.isdigit() or (ch == "." and i + 1 < len(expr) and expr[i + 1].isdigit()):
                start = i
                seen_dot = False
                if ch == ".":
                    seen_dot = True
                    i += 1
                while i < len(expr):
                    c = expr[i]
                    if c.isdigit():
                        i += 1
                        continue
                    if c == "." and not seen_dot:
                        seen_dot = True
                        i += 1
                        continue
                    break
                sql.append(expr[start:i])
                continue
            if ch.isalpha() or ch == "_":
                token_start = i
                i += 1
                while i < len(expr) and (expr[i].isalnum() or expr[i] == "_"):
                    i += 1
                lhs = expr[token_start:i]
                dot_pos = i
                while dot_pos < len(expr) and expr[dot_pos].isspace():
                    dot_pos += 1
                if dot_pos < len(expr) and expr[dot_pos] == ".":
                    rhs_start = dot_pos + 1
                    while rhs_start < len(expr) and expr[rhs_start].isspace():
                        rhs_start += 1
                    assert rhs_start < len(expr), "stage3-filter: missing feature token after '.'"
                    assert expr[rhs_start].isalpha() or expr[rhs_start] == "_", (
                        "stage3-filter: missing feature token after '.'"
                    )
                    rhs_end = rhs_start + 1
                    while rhs_end < len(expr) and (expr[rhs_end].isalnum() or expr[rhs_end] == "_"):
                        rhs_end += 1
                    rhs = expr[rhs_start:rhs_end]
                    tag_id = self.resolve_tag(lhs)
                    assert tag_id is not None, f"stage3-filter: unknown industry '{lhs}'"
                    column = feature_token_to_column(rhs)
                    assert column is not None, f"stage3-filter: unknown feature token '{rhs}'"
                    assert "sharpe" not in column or tag_id == -1, (
                        f"stage3-filter: Sharpe ratio (shp) is only supported for ALL industry, not for '{lhs}'"
                    )
                    sql.append(self.register_field(tag_id, column))
                    i = rhs_end
                    continue
                kw = lhs.lower()
                if kw == "and":
                    sql.append(" AND ")
                    continue
                if kw == "or":
                    sql.append(" OR ")
                    continue
                if kw == "not":
                    sql.append(" NOT ")
                    continue
                if kw == "true":
                    sql.append("TRUE")
                    continue
                if kw == "false":
                    sql.append("FALSE")
                    continue
                assert False, f"stage3-filter: unsupported token '{lhs}'"
            if ch in "()+-*/":
                sql.append(ch)
                i += 1
                continue
            if ch in "><=!":
                if i + 1 < len(expr):
                    two = expr[i:i + 2]
                    if two in (">=", "<=", "!="):
                        sql.append(two)
                        i += 2
                        continue
                    if two == "==":
                        sql.append("=")
                        i += 2
                        continue
                assert ch != "!", "stage3-filter: unsupported operator '!'"
                sql.append(ch)
                i += 1
                continue
            assert False, "stage3-filter: unsupported character in expression"
        return "".join(sql).strip()

    def register_field(self, tag_id: int, column: str) -> str:
        key = f"{tag_id}:{column}"
        if key in self.binding_map:
            return self.binding_map[key].alias
        alias = f"f{len(self.bindings)}"
        binding = FieldBinding(tag_id=tag_id, column=column, alias=alias)
        self.binding_map[key] = binding
        self.bindings.append(binding)
        return alias


def load_tag_to_industry_id(repo_root: Path) -> dict[str, int]:
    tag_md = repo_root / "core-backend/src/stage0/TAG.md"
    assert tag_md.exists(), f"tag file not found: {tag_md}"
    tag_to_industry_id: dict[str, int] = {}
    current_id = -1
    next_id = 0
    for raw_line in tag_md.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("## "):
            level1 = line[3:].strip()
            assert level1, "empty primary tag in TAG.md"
            assert next_id <= 12, "unexpected primary tag count"
            current_id = next_id
            next_id += 1
            key = normalize_token(level1)
            assert key, "empty normalized primary tag"
            tag_to_industry_id[key] = current_id
            continue
        if line.startswith("- "):
            assert current_id >= 0, "secondary tag before primary tag"
            rest = line[2:]
            hash_pos = rest.find("#")
            if hash_pos != -1:
                rest = rest[:hash_pos]
            level2 = rest.strip()
            assert level2, "empty secondary tag in TAG.md"
            key = normalize_token(level2)
            assert key, "empty normalized secondary tag"
            tag_to_industry_id[key] = current_id
    assert next_id == 13, f"unexpected primary tag count: {next_id}"
    tag_to_industry_id["unknown"] = 13
    return tag_to_industry_id


def load_stage3_db_path_from_config(repo_root: Path) -> Path:
    config_path = repo_root / "config.json"
    assert config_path.exists(), f"config file not found: {config_path}"
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    db_path = (repo_root / config["db_path_stage3"]).resolve()
    return db_path


def resolve_db_path(repo_root: Path, explicit_path: str) -> Path:
    if explicit_path:
        db_path = Path(explicit_path).expanduser().resolve()
        assert db_path.exists(), f"duckdb not found: {db_path}"
        return db_path
    db_path = load_stage3_db_path_from_config(repo_root)
    assert db_path.exists(), f"duckdb not found: {db_path}"
    return db_path


def read_bucket_bounds(conn: duckdb.DuckDBPyConnection) -> tuple[int, int]:
    row = conn.execute(
        """
        SELECT
            MIN(block_bucket) AS bucket_min,
            MAX(block_bucket) AS bucket_max
        FROM feature_tensor_state
        WHERE tag_id = -1
        """
    ).fetchone()
    assert row is not None, "feature_tensor_state query returned no rows"
    assert row[0] is not None and row[1] is not None, "feature_tensor_state has no tag_id=-1 rows"
    return int(row[0]), int(row[1])


def build_top_pool_sql(
    tag_to_industry_id: dict[str, int],
    start_bucket: int,
    end_bucket: int,
    filters: list[str],
    sort_expr: str,
    sort_asc: bool,
    pool_size: int,
) -> str:
    binding_map: dict[str, FieldBinding] = {}
    bindings: list[FieldBinding] = []
    translator = ExprTranslator(
        resolve_tag=lambda raw: resolve_industry_tag_id(tag_to_industry_id, raw),
        binding_map=binding_map,
        bindings=bindings,
    )
    sort_sql = translator.translate(sort_expr.strip())
    assert sort_sql, "stage3-filter: sort_expr is required"
    where_parts = [translator.translate(expr.strip()) for expr in filters if expr.strip()]
    used_tag_ids = sorted({-1, *[binding.tag_id for binding in bindings]})

    select_parts = [
        "block_bucket",
        "user_addr",
        "MAX(CASE WHEN tag_id = -1 THEN token_avg_100w END) AS month_avg_tok",
        "MAX(CASE WHEN tag_id = -1 THEN exposure_avg_100w END) AS month_avg_exp",
        "MAX(CASE WHEN tag_id = -1 THEN holding_period_avg_100w END) AS month_avg_hp",
    ]
    for binding in bindings:
        select_parts.append(
            f"MAX(CASE WHEN tag_id = {binding.tag_id} THEN {binding.column} END) AS {binding.alias}"
        )
    where_sql = ""
    if where_parts:
        where_sql = "WHERE " + " AND ".join(f"({part})" for part in where_parts)
    tag_sql = ",".join(str(tag_id) for tag_id in used_tag_ids)
    order_dir = "ASC" if sort_asc else "DESC"

    sql = f"""
    WITH bucket_universe AS (
        SELECT DISTINCT block_bucket
        FROM feature_tensor_state
        WHERE tag_id = -1
          AND block_bucket BETWEEN {start_bucket} AND {end_bucket}
    ),
    user_features AS (
        SELECT
            {", ".join(select_parts)}
        FROM feature_tensor_state
        WHERE block_bucket BETWEEN {start_bucket} AND {end_bucket}
          AND tag_id IN ({tag_sql})
        GROUP BY block_bucket, user_addr
    ),
    filtered AS (
        SELECT
            block_bucket,
            user_addr,
            lower(hex(user_addr)) AS addr,
            CAST(({sort_sql}) AS DOUBLE) AS sort_value,
            COALESCE(month_avg_tok, 0) AS month_avg_tok,
            COALESCE(month_avg_exp, 0) AS month_avg_exp,
            COALESCE(month_avg_hp, 0) AS month_avg_hp
        FROM user_features
        {where_sql}
    ),
    candidate_counts AS (
        SELECT
            block_bucket,
            COUNT(*) AS candidate_count
        FROM filtered
        GROUP BY block_bucket
    ),
    ranked AS (
        SELECT
            block_bucket,
            addr,
            sort_value,
            month_avg_tok,
            month_avg_exp,
            month_avg_hp,
            ROW_NUMBER() OVER (
                PARTITION BY block_bucket
                ORDER BY sort_value {order_dir} NULLS LAST, addr ASC
            ) AS rn
        FROM filtered
    )
    SELECT
        b.block_bucket,
        COALESCE(c.candidate_count, 0) AS candidate_count,
        r.rn,
        r.addr,
        r.sort_value,
        r.month_avg_tok,
        r.month_avg_exp,
        r.month_avg_hp
    FROM bucket_universe b
    LEFT JOIN candidate_counts c
      ON b.block_bucket = c.block_bucket
    LEFT JOIN ranked r
      ON b.block_bucket = r.block_bucket
     AND r.rn <= {pool_size}
    ORDER BY b.block_bucket ASC, r.rn ASC NULLS LAST
    """
    return sql


def fetch_pool_members(
    conn: duckdb.DuckDBPyConnection,
    sql: str,
) -> tuple[list[int], dict[int, int], dict[int, list[PoolMember]]]:
    rows = conn.execute(sql).fetchall()
    assert rows, "no buckets found in selected range"
    buckets: list[int] = []
    candidate_count_by_bucket: dict[int, int] = {}
    members_by_bucket: dict[int, list[PoolMember]] = {}
    seen_buckets: set[int] = set()
    for row in rows:
        bucket = int(row[0])
        candidate_count = int(row[1])
        rn = row[2]
        addr = row[3]
        sort_value = row[4]
        month_avg_tok = row[5]
        month_avg_exp = row[6]
        month_avg_hp = row[7]
        if bucket not in seen_buckets:
            buckets.append(bucket)
            seen_buckets.add(bucket)
        candidate_count_by_bucket[bucket] = candidate_count
        members_by_bucket.setdefault(bucket, [])
        if rn is None or addr is None:
            continue
        members_by_bucket[bucket].append(
            PoolMember(
                bucket=bucket,
                candidate_count=candidate_count,
                rank=int(rn),
                addr=f"0x{addr}",
                sort_value=None if sort_value is None else float(sort_value),
                month_avg_tok=int(month_avg_tok),
                month_avg_exp=int(month_avg_exp),
                month_avg_hp=int(month_avg_hp),
            )
        )
    return buckets, candidate_count_by_bucket, members_by_bucket


def build_bucket_summaries(
    buckets: list[int],
    candidate_count_by_bucket: dict[int, int],
    members_by_bucket: dict[int, list[PoolMember]],
) -> list[BucketSummary]:
    summaries: list[BucketSummary] = []
    prev_members: set[str] | None = None
    for bucket in buckets:
        members = members_by_bucket.get(bucket, [])
        member_addrs = tuple(member.addr for member in members)
        current_members = set(member_addrs)
        valid_sort_values = [member.sort_value for member in members if member.sort_value is not None]
        if valid_sort_values:
            avg_sort_value = sum(valid_sort_values) / len(valid_sort_values)
            min_sort_value = min(valid_sort_values)
            max_sort_value = max(valid_sort_values)
        else:
            avg_sort_value = 0.0
            min_sort_value = 0.0
            max_sort_value = 0.0
        if prev_members is None:
            retained = 0
            entered = 0
            exited = 0
            replacement = 0
            replacement_rate = 0.0
            jaccard = 1.0
            entered_addrs: tuple[str, ...] = ()
            exited_addrs: tuple[str, ...] = ()
        else:
            retained_members = current_members & prev_members
            entered_members = current_members - prev_members
            exited_members = prev_members - current_members
            retained = len(retained_members)
            entered = len(entered_members)
            exited = len(exited_members)
            replacement = min(entered, exited)
            replacement_rate = replacement / max(len(current_members), len(prev_members), 1)
            union_size = len(current_members | prev_members)
            jaccard = retained / union_size if union_size > 0 else 1.0
            entered_addrs = tuple(sorted(entered_members))
            exited_addrs = tuple(sorted(exited_members))
        summaries.append(
            BucketSummary(
                bucket=bucket,
                candidate_count=candidate_count_by_bucket[bucket],
                selected_count=len(members),
                retained_count=retained,
                entered_count=entered,
                exited_count=exited,
                replacement_count=replacement,
                replacement_rate=replacement_rate,
                jaccard=jaccard,
                avg_sort_value=avg_sort_value,
                min_sort_value=min_sort_value,
                max_sort_value=max_sort_value,
                members=member_addrs,
                entered=entered_addrs,
                exited=exited_addrs,
            )
        )
        prev_members = current_members
    return summaries


def write_summary_csv(path: Path, summaries: list[BucketSummary]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "bucket",
                "candidate_count",
                "selected_count",
                "retained_count",
                "entered_count",
                "exited_count",
                "replacement_count",
                "replacement_rate",
                "jaccard",
                "avg_sort_value",
                "min_sort_value",
                "max_sort_value",
                "members",
                "entered",
                "exited",
            ]
        )
        for row in summaries:
            writer.writerow(
                [
                    row.bucket,
                    row.candidate_count,
                    row.selected_count,
                    row.retained_count,
                    row.entered_count,
                    row.exited_count,
                    row.replacement_count,
                    f"{row.replacement_rate:.8f}",
                    f"{row.jaccard:.8f}",
                    f"{row.avg_sort_value:.8f}",
                    f"{row.min_sort_value:.8f}",
                    f"{row.max_sort_value:.8f}",
                    "|".join(row.members),
                    "|".join(row.entered),
                    "|".join(row.exited),
                ]
            )


def write_members_csv(path: Path, buckets: list[int], members_by_bucket: dict[int, list[PoolMember]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "bucket",
                "candidate_count",
                "rank",
                "addr",
                "sort_value",
                "month_avg_tok",
                "month_avg_exp",
                "month_avg_hp",
            ]
        )
        for bucket in buckets:
            for member in members_by_bucket.get(bucket, []):
                writer.writerow(
                    [
                        member.bucket,
                        member.candidate_count,
                        member.rank,
                        member.addr,
                        "" if member.sort_value is None else f"{member.sort_value:.8f}",
                        member.month_avg_tok,
                        member.month_avg_exp,
                        member.month_avg_hp,
                    ]
                )


def print_run_config(
    db_path: Path,
    start_bucket: int,
    end_bucket: int,
    pool_size: int,
    filters: list[str],
    sort_expr: str,
    sort_asc: bool,
) -> None:
    print(f"db_path: {db_path}")
    print(f"bucket_range: {start_bucket} -> {end_bucket}")
    print(f"pool_size: {pool_size}")
    print(f"sort_expr: {sort_expr}")
    print(f"sort_dir: {'ASC' if sort_asc else 'DESC'}")
    print("filters:")
    for expr in filters:
        print(f"  - {expr}")


def print_analysis_summary(pool_size: int, summaries: list[BucketSummary]) -> None:
    assert summaries, "no bucket summaries"
    candidate_counts = [row.candidate_count for row in summaries]
    selected_counts = [row.selected_count for row in summaries]
    transitions = summaries[1:]
    unique_players = set()
    for row in summaries:
        unique_players.update(row.members)
    cumulative_entries = sum(row.entered_count for row in transitions)
    cumulative_exits = sum(row.exited_count for row in transitions)
    cumulative_replacements = sum(row.replacement_count for row in transitions)
    avg_replacement_rate = (
        sum(row.replacement_rate for row in transitions) / len(transitions) if transitions else 0.0
    )
    avg_jaccard = sum(row.jaccard for row in transitions) / len(transitions) if transitions else 1.0
    full_pool_buckets = sum(1 for row in summaries if row.selected_count == pool_size)

    print()
    print("summary:")
    print(f"  buckets: {len(summaries)}")
    print(f"  first_bucket: {summaries[0].bucket}")
    print(f"  last_bucket: {summaries[-1].bucket}")
    print(f"  full_pool_buckets: {full_pool_buckets}/{len(summaries)}")
    print(
        "  candidate_count avg/min/max: "
        f"{sum(candidate_counts) / len(candidate_counts):.2f} / {min(candidate_counts)} / {max(candidate_counts)}"
    )
    print(
        "  selected_count avg/min/max: "
        f"{sum(selected_counts) / len(selected_counts):.2f} / {min(selected_counts)} / {max(selected_counts)}"
    )
    print(f"  cumulative_entries: {cumulative_entries}")
    print(f"  cumulative_exits: {cumulative_exits}")
    print(f"  cumulative_replacements: {cumulative_replacements}")
    print(f"  avg_replacement_rate: {avg_replacement_rate:.6f}")
    print(f"  avg_jaccard: {avg_jaccard:.6f}")
    print(f"  unique_players_seen: {len(unique_players)}")

    if not transitions:
        return

    most_unstable = sorted(
        transitions,
        key=lambda row: (-row.replacement_count, -(row.entered_count + row.exited_count), row.jaccard, row.bucket),
    )[:10]
    print()
    print("top_unstable_transitions:")
    for row in most_unstable:
        print(
            f"  bucket={row.bucket} cand={row.candidate_count} size={row.selected_count} "
            f"repl={row.replacement_count} enter={row.entered_count} exit={row.exited_count} "
            f"keep={row.retained_count} jaccard={row.jaccard:.6f}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", default="", help="duckdb path")
    parser.add_argument("--start-bucket", type=int, default=None)
    parser.add_argument("--end-bucket", type=int, default=None)
    parser.add_argument("--pool-size", type=int, default=DEFAULT_POOL_SIZE)
    parser.add_argument("--filter", action="append", dest="filters", default=None)
    parser.add_argument("--sort-expr", default=DEFAULT_SORT_EXPR)
    parser.add_argument("--sort-asc", action="store_true", default=DEFAULT_SORT_ASC)
    parser.add_argument("--summary-csv", default="")
    parser.add_argument("--members-csv", default="")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[2]
    db_path = resolve_db_path(repo_root, args.db)
    filters = list(args.filters) if args.filters is not None else list(DEFAULT_FILTERS)
    assert args.pool_size > 0, "pool_size must be > 0"
    assert args.sort_expr.strip(), "sort_expr must not be empty"
    tag_to_industry_id = load_tag_to_industry_id(repo_root)

    conn = duckdb.connect(str(db_path), read_only=True)
    bucket_min, bucket_max = read_bucket_bounds(conn)
    start_bucket = bucket_min if args.start_bucket is None else args.start_bucket
    end_bucket = bucket_max if args.end_bucket is None else args.end_bucket
    assert start_bucket <= end_bucket, "start_bucket must be <= end_bucket"

    sql = build_top_pool_sql(
        tag_to_industry_id=tag_to_industry_id,
        start_bucket=start_bucket,
        end_bucket=end_bucket,
        filters=filters,
        sort_expr=args.sort_expr,
        sort_asc=args.sort_asc,
        pool_size=args.pool_size,
    )
    buckets, candidate_count_by_bucket, members_by_bucket = fetch_pool_members(conn, sql)
    summaries = build_bucket_summaries(
        buckets=buckets,
        candidate_count_by_bucket=candidate_count_by_bucket,
        members_by_bucket=members_by_bucket,
    )

    print_run_config(
        db_path=db_path,
        start_bucket=buckets[0],
        end_bucket=buckets[-1],
        pool_size=args.pool_size,
        filters=filters,
        sort_expr=args.sort_expr,
        sort_asc=args.sort_asc,
    )
    print_analysis_summary(args.pool_size, summaries)

    if args.summary_csv:
        write_summary_csv(Path(args.summary_csv).expanduser().resolve(), summaries)
        print()
        print(f"summary_csv: {Path(args.summary_csv).expanduser().resolve()}")
    if args.members_csv:
        write_members_csv(Path(args.members_csv).expanduser().resolve(), buckets, members_by_bucket)
        print(f"members_csv: {Path(args.members_csv).expanduser().resolve()}")


if __name__ == "__main__":
    main()
