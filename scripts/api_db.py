#!/usr/bin/env python3
"""DuckDB 直接操作工具 — 后端必须停掉才能用"""

import json
from pathlib import Path
import duckdb

ROOT = Path(__file__).parent.parent
DB_PATH = ROOT / json.loads((ROOT / "config.json").read_text())["db_path"]

# ============================================================================
# 在这里写要执行的操作
# ============================================================================


def addr(b):
    # DuckDB returns BLOBs as b'x{hex}' — prepend '0' to get '0x{hex}'
    return ("0" + b.decode("ascii")) if isinstance(b, (bytes, bytearray)) else str(b)


def main(conn):
    research2(conn)


def research2(conn):
    # 把 split 次数多的地址定义为 FPMM 合约（>100次 split 明显是合约）
    # 看这些"FPMM地址"在 transfer 里占多少
    for threshold in [10, 100, 1000]:
        r = conn.sql(f"""
            WITH fpmm AS (
                SELECT stakeholder FROM split
                GROUP BY stakeholder HAVING COUNT(*) > {threshold}
            )
            SELECT
                COUNT(*) FILTER (WHERE from_addr IN (SELECT stakeholder FROM fpmm)) as from_fpmm,
                COUNT(*) FILTER (WHERE to_addr   IN (SELECT stakeholder FROM fpmm)) as to_fpmm,
                COUNT(*) as total
            FROM transfer
        """).fetchone()
        fpmm_addr_cnt = conn.sql(f"""
            SELECT COUNT(*) FROM (
                SELECT stakeholder FROM split GROUP BY stakeholder HAVING COUNT(*) > {threshold}
            )
        """).fetchone()[0]
        print(f"split>{threshold}次 的地址({fpmm_addr_cnt:,}个):  "
              f"from={r[0]:,}({r[0]/r[2]*100:.1f}%)  "
              f"to={r[1]:,}({r[1]/r[2]*100:.1f}%)  "
              f"total={r[2]:,}")

    # 这些高频 split 地址到底是不是合约？随机取1个查链上
    print("\n高频split地址（取 split>1000 的前5个）:")
    rows = conn.sql("""
        SELECT stakeholder, COUNT(*) as splits,
               (SELECT COUNT(*) FROM transfer WHERE from_addr = s.stakeholder) as out_xfers,
               (SELECT COUNT(*) FROM transfer WHERE to_addr   = s.stakeholder) as in_xfers
        FROM split s
        GROUP BY stakeholder HAVING COUNT(*) > 1000
        ORDER BY splits DESC LIMIT 5
    """).fetchall()
    for b, splits, out_x, in_x in rows:
        print(f"  {addr(b)}  splits={splits:,}  out_xfers={out_x:,}  in_xfers={in_x:,}")


def research(conn):
    # transfer 概况
    r = conn.sql("SELECT MIN(block_number), MAX(block_number), COUNT(*) FROM transfer").fetchone()
    print(f"transfer: {r[2]:,} rows  block {r[0]:,} ~ {r[1]:,}")

    # split 里出现最多的 stakeholder（FPMM 合约会大量 split）
    print("\n=== split stakeholder top 20 ===")
    rows = conn.sql("""
        SELECT stakeholder, COUNT(*) as cnt
        FROM split GROUP BY stakeholder ORDER BY cnt DESC LIMIT 20
    """).fetchall()
    for b, cnt in rows:
        print(f"  {addr(b)}  {cnt:,}")

    # transfer.from_addr 出现最多的地址（FPMM 会频繁往外转 token）
    print("\n=== transfer from_addr top 20 ===")
    rows = conn.sql("""
        SELECT from_addr, COUNT(*) as cnt
        FROM transfer GROUP BY from_addr ORDER BY cnt DESC LIMIT 20
    """).fetchall()
    for b, cnt in rows:
        print(f"  {addr(b)}  {cnt:,}")

    # transfer.to_addr 出现最多的地址（FPMM 也频繁接收 token）
    print("\n=== transfer to_addr top 20 ===")
    rows = conn.sql("""
        SELECT to_addr, COUNT(*) as cnt
        FROM transfer GROUP BY to_addr ORDER BY cnt DESC LIMIT 20
    """).fetchall()
    for b, cnt in rows:
        print(f"  {addr(b)}  {cnt:,}")

    # 统计：transfer 里的 from_addr 有多少也在 split.stakeholder（FPMM 特征）
    r = conn.sql("""
        SELECT COUNT(*) FROM transfer
        WHERE from_addr IN (SELECT DISTINCT stakeholder FROM split)
    """).fetchone()[0]
    print(f"\ntransfer.from_addr in split.stakeholder: {r:,} / ", end="")
    total = conn.sql("SELECT COUNT(*) FROM transfer").fetchone()[0]
    print(f"{total:,} = {r/total*100:.1f}%")

    r2 = conn.sql("""
        SELECT COUNT(*) FROM transfer
        WHERE to_addr IN (SELECT DISTINCT stakeholder FROM split)
    """).fetchone()[0]
    print(f"transfer.to_addr   in split.stakeholder: {r2:,} / {total:,} = {r2/total*100:.1f}%")


# ============================================================================

READONLY = True


def show(conn, sql):
    print(conn.sql(sql))


def exec(conn, sql):
    assert not READONLY, "当前是只读模式，把 READONLY 改成 False"
    conn.sql(sql)
    print("OK:", sql[:80])


def status(conn):
    tables = conn.sql(
        "SELECT table_name FROM information_schema.tables WHERE table_schema='main' ORDER BY table_name").fetchall()
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
