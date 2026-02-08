#!/usr/bin/env python3
"""DuckDB 直接操作工具 — 后端必须停掉才能用"""

import json
from pathlib import Path
import duckdb

ROOT = Path(__file__).parent
DB_PATH = ROOT / json.loads((ROOT / "config.json").read_text())["db_path"]

# ============================================================================
# 在这里写要执行的操作
# ============================================================================

def main(conn):
    status(conn)

    # show(conn, "SELECT * FROM sync_state")
    # exec(conn, "DELETE FROM sync_state WHERE source='Profit and Loss' AND entity='Condition'")


# ============================================================================

READONLY = True

def show(conn, sql):
    print(conn.sql(sql))

def exec(conn, sql):
    assert not READONLY, "当前是只读模式，把 READONLY 改成 False"
    conn.sql(sql)
    print("OK:", sql[:80])

def status(conn):
    tables = conn.sql("SELECT table_name FROM information_schema.tables WHERE table_schema='main' ORDER BY table_name").fetchall()
    print(f"=== 表 ({len(tables)}) ===")
    for (t,) in tables:
        count = conn.sql(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
        print(f"  {t}: {count:,} rows")

    print("\n=== sync_state ===")
    show(conn, "SELECT * FROM sync_state ORDER BY source, entity")

    print("\n=== entity_stats_meta ===")
    show(conn, "SELECT source, entity, total_requests, success_requests, total_rows_synced, success_rate FROM entity_stats_meta ORDER BY source, entity")

    print("\n=== condition positionIds ===")
    show(conn, """
        SELECT
            COUNT(*) as total,
            COUNT(positionIds) as has_pos_ids,
            COUNT(*) - COUNT(positionIds) as null_pos_ids,
            COUNT(resolutionTimestamp) as has_res_ts,
            COUNT(*) - COUNT(resolutionTimestamp) as null_res_ts
        FROM condition
    """)

if __name__ == "__main__":
    assert DB_PATH.exists(), f"数据库不存在: {DB_PATH}"
    conn = duckdb.connect(str(DB_PATH), read_only=READONLY)
    try:
        main(conn)
    finally:
        conn.close()
