#!/usr/bin/env python3

import json
from pathlib import Path

import duckdb


def normalize_condition_id(raw: str) -> str:
    cid = raw.strip().lower()
    if cid.startswith("0x"):
        cid = cid[2:]
    assert len(cid) == 64, "condition_id must be 32-byte hex"
    assert all(c in "0123456789abcdef" for c in cid), "condition_id must be hex"
    return cid


def load_stage0_db_path(repo_root: Path) -> Path:
    config_path = repo_root / "config.json"
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    db_rel = config["db_path_stage0"]
    db_path = (repo_root / db_rel).resolve()
    assert db_path.exists(), f"stage0 db not found: {db_path}"
    return db_path


def query_condition(db_path: Path, condition_id_hex_no_prefix: str) -> None:
    conn = duckdb.connect(str(db_path), read_only=True)

    rows = conn.execute(
        """
        SELECT
          lower(hex(s.condition_id)) AS condition_id_hex,
          s.market_json::VARCHAR AS market_json,
          c.class,
          c.first_seen_block,
          c.first_seen_ms
        FROM pm_condition_static s
        LEFT JOIN pm_condition_scan_class c
          ON s.condition_id = c.condition_id
        WHERE lower(hex(s.condition_id)) = ?
        """,
        [condition_id_hex_no_prefix],
    ).fetchall()

    assert len(rows) <= 1, "condition_id should be unique"
    assert rows, f"condition_id not found in stage0 db: 0x{condition_id_hex_no_prefix}"

    row = rows[0]
    market_obj = json.loads(row[1])

    print(f"condition_id:   0x{row[0]}")
    print(f"class:          {row[2]}")
    print(f"first_seen_blk: {row[3]}")
    print(f"first_seen_ms:  {row[4]}")
    print("market_json:")
    print(json.dumps(market_obj, ensure_ascii=False, indent=2))


def lookup_by_condition(condition_id: str, db_path: str = "") -> None:
    condition_id_hex_no_prefix = normalize_condition_id(condition_id)
    repo_root = Path(__file__).resolve().parents[1]
    db = Path(db_path).resolve() if db_path else load_stage0_db_path(repo_root)
    query_condition(db, condition_id_hex_no_prefix)


if __name__ == "__main__":
    # ---- 改这里 ----
    lookup_by_condition("0xf73eed12df505d39083594f9681d40f26956d480054fa812f2915e21f0f3b809")
    # lookup_by_condition("0x...", "/absolute/path/to/condition.duckdb")
