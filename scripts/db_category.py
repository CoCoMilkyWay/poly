#!/usr/bin/env python3
"""
从 tag/tags/*.txt 读取分类结果, 按 start_block 画滑动窗口时序图。
"""

import json
import webbrowser
from collections import Counter
from collections import deque
from pathlib import Path

from tqdm import tqdm


def _parse_tag_files(tags_dir: Path) -> list[dict]:
    """从 tags/*.txt 解析出 (tag_name, start_block) 列表"""
    assert tags_dir.exists(), f"tags dir not found: {tags_dir}"

    parsed_rows = []
    for tag_file in sorted(tags_dir.glob("*.txt")):
        tag_name = tag_file.stem  # 文件名即 tag 名
        with tag_file.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith("="):
                    continue
                # 格式: start_block  confidence  question
                parts = line.split(None, 2)
                if len(parts) < 2:
                    continue
                try:
                    start_block = int(parts[0])
                except ValueError:
                    continue
                parsed_rows.append({
                    "tag": tag_name,
                    "start_block": start_block,
                })

    # 按 start_block 排序
    parsed_rows.sort(key=lambda x: x["start_block"])
    return parsed_rows


def _build_window_series(rows: list[dict], window_size: int):
    assert window_size > 0, "window_size must be > 0"
    blocks = []
    block_tag_counts = []
    event_queue = deque()
    live_counter = Counter()
    right = 0
    n = len(rows)

    unique_blocks = sorted({r["start_block"] for r in rows})
    for block in tqdm(unique_blocks, desc="Building window series", unit="blk"):
        # 添加新事件
        while right < n and rows[right]["start_block"] <= block:
            r = rows[right]
            event_queue.append((r["start_block"], r["tag"]))
            live_counter[r["tag"]] += 1
            right += 1
        # 移除窗口外的旧事件
        window_left = block - window_size + 1
        while event_queue and event_queue[0][0] < window_left:
            _, old_tag = event_queue.popleft()
            live_counter[old_tag] -= 1
            if live_counter[old_tag] == 0:
                del live_counter[old_tag]
        blocks.append(block)
        block_tag_counts.append(dict(live_counter))

    return blocks, block_tag_counts


def _plot_window_series(blocks, block_tag_counts, tags: list[str]) -> None:
    traces = []
    for tag in tags:
        y = [counts.get(tag, 0) for counts in block_tag_counts]
        traces.append({
            "x": blocks,
            "y": y,
            "type": "scatter",
            "mode": "lines",
            "name": tag,
        })
    payload = {
        "traces": traces,
        "layout": {
            "title": "Tag Distribution in Sliding Block Window",
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
  <title>tag_window_plot</title>
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
    script_dir = Path(__file__).resolve().parent
    html_path = script_dir / "tag" / "tag_plot.html"
    html_path.write_text(html, encoding="utf-8")
    print(f"saved: {html_path}")
    webbrowser.open(html_path.as_uri())


def plot_tags(window_size: int = 200000) -> None:
    script_dir = Path(__file__).resolve().parent
    tags_dir = script_dir / "tag" / "tags"

    print(f"Reading tags from: {tags_dir}")
    parsed_rows = _parse_tag_files(tags_dir)
    print(f"Total samples: {len(parsed_rows)}")

    # 统计每个 tag 的数量
    tag_counter = Counter(r["tag"] for r in parsed_rows)
    print(f"Distinct tags: {len(tag_counter)}")
    print(f"Window size: {window_size}")
    print()
    for tag, cnt in tag_counter.most_common():
        print(f"{cnt:8d}  {tag}")

    # 构建时序数据
    tags = [tag for tag, _ in tag_counter.most_common()]
    blocks, block_tag_counts = _build_window_series(parsed_rows, window_size)
    _plot_window_series(blocks, block_tag_counts, tags)


if __name__ == "__main__":
    plot_tags(window_size=int(0.5*3600*24*30*1))
