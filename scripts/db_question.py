#!/usr/bin/env python3

import json
from pathlib import Path

import duckdb
from tqdm import tqdm

def load_stage0_db_path(repo_root: Path) -> Path:
    config_path = repo_root / "config.json"
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    db_rel = config["db_path_stage0"]
    db_path = (repo_root / db_rel).resolve()
    assert db_path.exists(), f"stage0 db not found: {db_path}"
    return db_path


def _extract_question(market: dict) -> str:
    question = market.get("question")
    if question:
        return str(question)
    events = market.get("events", [])
    if events:
        title = events[0].get("title")
        if title:
            return str(title)
    return "__MISSING__"


def dump_condition_question_pairs(db_path: str = "") -> None:
    repo_root = Path(__file__).resolve().parents[1]
    db = Path(db_path).resolve() if db_path else load_stage0_db_path(repo_root)

    conn = duckdb.connect(str(db), read_only=True)
    total = conn.execute("SELECT COUNT(*) FROM pm_condition_static").fetchone()[0]
    rows = conn.execute(
        """
        SELECT
          lower(hex(condition_id)) AS cid,
          market_json::VARCHAR AS market_json
        FROM pm_condition_static
        ORDER BY cid ASC
        """
    ).fetchall()
    assert len(rows) == total, "row count mismatch"

    print(f"db: {db}")
    print(f"total_conditions: {total}")
    print("condition_id | question")
    for cid_hex, market_json in tqdm(rows, total=total, desc="Scanning questions", unit="cond"):
        market = json.loads(market_json)
        question = _extract_question(market).replace("\n", " ").strip()
        print(f"{question}")


if __name__ == "__main__":
    # ---- 改这里 ----
    dump_condition_question_pairs()
    # dump_condition_question_pairs("/absolute/path/to/condition.duckdb")
