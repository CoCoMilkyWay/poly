#!/usr/bin/env python3

import json
import tempfile
import webbrowser
from collections import Counter
from collections import deque
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


def _extract_category(market: dict) -> str:
    category = market.get("category")
    if not category:
        events = market.get("events", [])
        if events:
            category = events[0].get("category")
    if not category:
        return "__MISSING__"
    return str(category)


def _build_window_series(rows, window_size: int):
    assert window_size > 0, "window_size must be > 0"
    blocks = []
    block_category_counts = []
    event_queue = deque()
    live_counter = Counter()
    right = 0
    n = len(rows)
    for block in tqdm(sorted({r["first_seen_block"] for r in rows}),
                      desc="Building window series", unit="blk"):
        while right < n and rows[right]["first_seen_block"] <= block:
            r = rows[right]
            event_queue.append((r["first_seen_block"], r["category"]))
            live_counter[r["category"]] += 1
            right += 1
        window_left = block - window_size + 1
        while event_queue and event_queue[0][0] < window_left:
            old_block, old_category = event_queue.popleft()
            _ = old_block
            live_counter[old_category] -= 1
            if live_counter[old_category] == 0:
                del live_counter[old_category]
        blocks.append(block)
        block_category_counts.append(dict(live_counter))
    return blocks, block_category_counts


def _plot_window_series(blocks, block_category_counts, categories) -> None:
    traces = []
    for category in categories:
        y = [counts.get(category, 0) for counts in block_category_counts]
        traces.append({
            "x": blocks,
            "y": y,
            "type": "scatter",
            "mode": "lines",
            "name": category,
        })
    payload = {
        "traces": traces,
        "layout": {
            "title": "Stage0 Condition Category Count in Sliding Block Window",
            "xaxis": {"title": "block_number"},
            "yaxis": {"title": "condition_count_in_window"},
            "hovermode": "x unified",
            "template": "plotly_white",
        },
    }
    html = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>db_category_window_plot</title>
  <script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
</head>
<body>
  <div id="plot" style="width:100%;height:92vh;"></div>
  <script>
    const payload = {json.dumps(payload, ensure_ascii=False)};
    Plotly.newPlot("plot", payload.traces, payload.layout, {{responsive: true}});
  </script>
</body>
</html>
"""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".html", prefix="db_category_plot_", delete=False,
                                     encoding="utf-8") as f:
        f.write(html)
        temp_html = Path(f.name).resolve()
    print(f"opening_plot: {temp_html}")
    ok = webbrowser.open(temp_html.as_uri())
    assert ok, "failed to open browser"


def summarize_category(db_path: str = "", window_size: int = 1000000) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    db = Path(db_path).resolve() if db_path else load_stage0_db_path(repo_root)

    conn = duckdb.connect(str(db), read_only=True)
    total = conn.execute("SELECT COUNT(*) FROM pm_condition_static").fetchone()[0]
    rows = conn.execute(
        """
        SELECT
          lower(hex(s.condition_id)) AS cid,
          s.market_json::VARCHAR AS market_json,
          c.first_seen_block AS first_seen_block
        FROM pm_condition_static s
        JOIN pm_condition_scan_class c ON s.condition_id = c.condition_id
        ORDER BY c.first_seen_block ASC, cid ASC
        """
    ).fetchall()
    assert len(rows) == total, "row count mismatch"

    cat_counter = Counter()
    sample_condition = {}
    parsed_rows = []
    missing_counter = 0
    for row in tqdm(rows, total=total, desc="Scanning categories", unit="cond"):
        condition_id_hex = str(row[0])
        market = json.loads(row[1])
        first_seen_block = int(row[2])
        category = _extract_category(market)
        if category == "__MISSING__":
            missing_counter += 1
        cat_counter[category] += 1
        if category not in sample_condition:
            sample_condition[category] = "0x" + condition_id_hex
        parsed_rows.append({
            "condition_id": "0x" + condition_id_hex,
            "category": category,
            "first_seen_block": first_seen_block,
        })

    print(f"db: {db}")
    print(f"total_conditions: {total}")
    print(f"distinct_categories: {len(cat_counter)}")
    print(f"missing_category_rows: {missing_counter}")
    print(f"window_size: {window_size}")
    print()
    for category, cnt in cat_counter.most_common():
        print(f"{cnt:8d}  {category:20s}  {sample_condition[category]}")

    categories = [
        category for category, _ in cat_counter.most_common()
        if category not in {"-", "__MISSING__"}
    ]
    blocks, block_category_counts = _build_window_series(parsed_rows, window_size)
    _plot_window_series(blocks, block_category_counts, categories)


if __name__ == "__main__":
    # ---- 改这里 ----
    summarize_category(
        window_size=100000,
    )
    # summarize_category("/absolute/path/to/condition.duckdb", window_size=100000)
