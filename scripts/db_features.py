#!/usr/bin/env python3

import json
import tempfile
import webbrowser
from pathlib import Path

import duckdb
import numpy as np

HIST_BINS = 1000
RANGE_Q_LOW = 0.03
RANGE_Q_HIGH = 0.97

TAG_NAMES = {
    0: "Crypto_Price",
    1: "Crypto_Market",
    2: "Sports_Basketball",
    3: "Sports_Football",
    4: "Sports_Soccer",
    5: "Sports_Individual",
    6: "Politics_US",
    7: "Politics_World",
    8: "Economy_Finance",
    9: "Tech",
    10: "Entertainment",
    11: "Weather",
    12: "Society",
    13: "Unknown",
}

FEATURE_SPECS = [
    {
        "key": "sharpe",
        "column": "sharpe_100w",
        "title": "sharpe_100w",
        "x_label": "sharpe_100w",
    },
    {
        "key": "volume",
        "column": "volume_avg_100w",
        "title": "volume_avg_100w",
        "x_label": "log10(1 + volume_avg_100w / 1e6 USD)",
    },
    {
        "key": "exposure",
        "column": "exposure_avg_100w",
        "title": "exposure_avg_100w",
        "x_label": "log10(1 + exposure_avg_100w / 1e6 USD)",
    },
    {
        "key": "token",
        "column": "token_avg_100w",
        "title": "token_avg_100w",
        "x_label": "token_avg_100w",
    },
    {
        "key": "holding",
        "column": "holding_period_avg_100w",
        "title": "holding_period_avg_100w",
        "x_label": "log10(1 + holding_period_avg_100w / days)",
    },
]


def load_stage3_db_path(repo_root: Path) -> Path:
    config_path = repo_root / "config.json"
    assert config_path.exists(), f"config file not found: {config_path}"
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    db_rel = config["db_path_stage3"]
    db_path = (repo_root / db_rel).resolve()
    assert db_path.exists(), f"stage3 db not found: {db_path}"
    return db_path


def transform_values(feature_key: str, raw_values: np.ndarray) -> np.ndarray:
    values = raw_values.astype(np.float64, copy=False)
    if feature_key in ("volume", "exposure"):
        values = np.maximum(values, 0.0) / 1_000_000.0
        values = np.log10(values + 1.0)
    elif feature_key == "holding":
        values = np.maximum(values, 0.0) / 43200.0
        values = np.log10(values + 1.0)
    elif feature_key == "token":
        values = np.maximum(values, 0.0)
    return values


def _split_groups(
    feature_key: str,
    group_ids: np.ndarray,
    raw_values: np.ndarray,
) -> dict[int, np.ndarray]:
    groups: dict[int, np.ndarray] = {}
    unique_group_ids = np.unique(group_ids).astype(np.int64).tolist()
    unique_group_ids.sort()
    for group_id in unique_group_ids:
        group_values = raw_values[group_ids == group_id]
        group_values = group_values[np.isfinite(group_values)]
        group_values = transform_values(feature_key, group_values)
        group_values = group_values[np.isfinite(group_values)]
        assert group_values.size > 0, f"empty group values for group_id={group_id}"
        groups[int(group_id)] = group_values
    assert groups, "no groups loaded"
    return groups


def load_time_bin_data(
    conn: duckdb.DuckDBPyConnection,
    feature_key: str,
    column: str,
) -> tuple[dict[int, np.ndarray], dict[int, str]]:
    meta_sql = f"""
        SELECT
            CAST(block_bucket / 50 AS INTEGER) AS bin_id,
            MIN(block_bucket) AS bucket_min,
            MAX(block_bucket) AS bucket_max
        FROM feature_tensor_state
        WHERE tag_id = -1
          AND {column} IS NOT NULL
        GROUP BY 1
        ORDER BY 1
    """
    meta_rows = conn.execute(meta_sql).fetchall()
    assert meta_rows, f"no time bins for column={column}"
    group_labels: dict[int, str] = {}
    for bin_id, bucket_min, bucket_max in meta_rows:
        start_w = int(bucket_min) * 10
        end_w = (int(bucket_max) + 1) * 10
        group_labels[int(bin_id)] = f"{start_w}W-{end_w}W"

    value_sql = f"""
        SELECT
            CAST(block_bucket / 50 AS INTEGER) AS group_id,
            CAST({column} AS DOUBLE) AS value
        FROM feature_tensor_state
        WHERE tag_id = -1
          AND {column} IS NOT NULL
    """
    arrays = conn.execute(value_sql).fetchnumpy()
    group_ids = np.asarray(arrays["group_id"], dtype=np.int64)
    raw_values = np.asarray(arrays["value"], dtype=np.float64)
    group_data = _split_groups(feature_key, group_ids, raw_values)
    return group_data, group_labels


def load_industry_data(
    conn: duckdb.DuckDBPyConnection,
    feature_key: str,
    column: str,
) -> tuple[dict[int, np.ndarray], dict[int, str]]:
    value_sql = f"""
        SELECT
            tag_id AS group_id,
            CAST({column} AS DOUBLE) AS value
        FROM feature_tensor_state
        WHERE tag_id >= 0
          AND {column} IS NOT NULL
    """
    arrays = conn.execute(value_sql).fetchnumpy()
    group_ids = np.asarray(arrays["group_id"], dtype=np.int64)
    raw_values = np.asarray(arrays["value"], dtype=np.float64)
    group_data = _split_groups(feature_key, group_ids, raw_values)

    group_labels: dict[int, str] = {}
    for group_id in sorted(group_data.keys()):
        group_labels[group_id] = TAG_NAMES.get(group_id, f"Tag_{group_id}")
    return group_data, group_labels


def _moving_average(y: np.ndarray) -> np.ndarray:
    kernel = np.array([1.0, 2.0, 3.0, 2.0, 1.0], dtype=np.float64)
    kernel = kernel / kernel.sum()
    return np.convolve(y, kernel, mode="same")


def _build_colors_time(n: int) -> list[str]:
    colors = []
    for i in range(n):
        ratio = i / max(n - 1, 1)
        hue = int(240 - 220 * ratio)
        colors.append(f"hsl({hue},70%,45%)")
    return colors


def _build_colors_industry(n: int) -> list[str]:
    base = [
        "#1f77b4",
        "#ff7f0e",
        "#2ca02c",
        "#d62728",
        "#9467bd",
        "#8c564b",
        "#e377c2",
        "#7f7f7f",
        "#bcbd22",
        "#17becf",
        "#4e79a7",
        "#f28e2b",
        "#59a14f",
        "#e15759",
    ]
    colors = []
    for i in range(n):
        colors.append(base[i % len(base)])
    return colors


def build_density_overlay_payload(
    group_data: dict[int, np.ndarray],
    group_labels: dict[int, str],
    title: str,
    subtitle: str,
    x_label: str,
    palette_mode: str,
) -> dict:
    assert group_data, "group_data is empty"
    all_values = np.concatenate([group_data[k] for k in sorted(group_data.keys())])
    assert all_values.size > 0, "all_values is empty"
    lo, hi = np.quantile(all_values, [RANGE_Q_LOW, RANGE_Q_HIGH])
    lo = float(lo)
    hi = float(hi)
    assert np.isfinite(lo), "min value is not finite"
    assert np.isfinite(hi), "max value is not finite"
    assert hi > lo, "invalid distribution range: q98 <= q2"

    sorted_ids = sorted(group_data.keys())
    if palette_mode == "time":
        colors = _build_colors_time(len(sorted_ids))
    else:
        colors = _build_colors_industry(len(sorted_ids))

    traces = []
    for idx, group_id in enumerate(sorted_ids):
        values = np.clip(group_data[group_id], lo, hi)
        hist, edges = np.histogram(
            values,
            bins=HIST_BINS,
            range=(lo, hi),
            density=True,
        )
        hist = _moving_average(hist)
        x = (edges[:-1] + edges[1:]) * 0.5
        label = group_labels.get(group_id, str(group_id))
        traces.append(
            {
                "type": "scatter",
                "mode": "lines",
                "name": f"{label} (n={values.size})",
                "x": x.tolist(),
                "y": hist.tolist(),
                "line": {"color": colors[idx], "width": 2},
                "opacity": 0.85,
                "hovertemplate": (
                    f"group={label}<br>{x_label}=%{{x:.4f}}"
                    "<br>density=%{y:.6f}<extra></extra>"
                ),
            }
        )

    layout = {
        "title": {"text": f"{title}<br><sup>{subtitle}</sup>"},
        "template": "plotly_white",
        "xaxis": {"title": x_label},
        "yaxis": {"title": "density"},
        "hovermode": "x unified",
        "legend": {"x": 1.02, "y": 1.0, "xanchor": "left", "yanchor": "top"},
        "margin": {"l": 80, "r": 280, "t": 90, "b": 70},
    }
    return {"traces": traces, "layout": layout}


def build_dashboard_html(page_title: str, chart_payloads: list[dict]) -> str:
    assert chart_payloads, "chart_payloads is empty"
    sections = []
    for idx in range(len(chart_payloads)):
        sections.append(
            f'<section class="chart-card"><div id="plot_{idx}" class="plot-box"></div></section>'
        )
    payload = {"charts": chart_payloads}
    sections_html = "\n".join(sections)
    return f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>{page_title}</title>
  <script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
  <style>
    body {{
      margin: 0;
      font-family: Arial, sans-serif;
      background: #f8f9fb;
    }}
    .page-title {{
      padding: 18px 24px 8px 24px;
      font-size: 22px;
      font-weight: 700;
    }}
    .page-subtitle {{
      padding: 0 24px 12px 24px;
      color: #5f6368;
      font-size: 14px;
    }}
    .chart-card {{
      margin: 14px 20px;
      padding: 6px;
      background: #ffffff;
      border: 1px solid #e6e8ee;
      border-radius: 8px;
    }}
    .plot-box {{
      width: 100%;
      height: 520px;
    }}
  </style>
</head>
<body>
  <div class="page-title">{page_title}</div>
  <div class="page-subtitle">全部分布均基于全量数据计算(无采样)，range=q2~q98，tail clip到边界</div>
  {sections_html}
  <script>
    const payload = {json.dumps(payload, ensure_ascii=False)};
    payload.charts.forEach((chart, idx) => {{
      Plotly.newPlot(`plot_${{idx}}`, chart.traces, chart.layout, {{responsive: true}});
    }});
  </script>
</body>
</html>
"""


def write_temp_html(page_tag: str, html: str) -> Path:
    fp = tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        prefix=f"poly_{page_tag}_",
        suffix=".html",
        delete=False,
    )
    fp.write(html)
    fp.close()
    return Path(fp.name)


def main() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    db_path = load_stage3_db_path(repo_root)
    conn = duckdb.connect(str(db_path), read_only=True)

    drift_charts: list[dict] = []
    for spec in FEATURE_SPECS:
        time_data, time_labels = load_time_bin_data(
            conn,
            feature_key=spec["key"],
            column=spec["column"],
        )
        time_payload = build_density_overlay_payload(
            group_data=time_data,
            group_labels=time_labels,
            title=f"Drift by Time Bin: {spec['title']}",
            subtitle=(
                "scope=tag_id:-1 (all industries), "
                "bin_size=500W blocks, "
                "distribution=full data"
            ),
            x_label=spec["x_label"],
            palette_mode="time",
        )
        drift_charts.append(time_payload)
        print(f"prepared drift plot: {spec['column']}")

    industry_charts: list[dict] = []
    for spec in FEATURE_SPECS:
        industry_data, industry_labels = load_industry_data(
            conn,
            feature_key=spec["key"],
            column=spec["column"],
        )
        industry_payload = build_density_overlay_payload(
            group_data=industry_data,
            group_labels=industry_labels,
            title=f"Distribution by Industry: {spec['title']}",
            subtitle=(
                "scope=all buckets, "
                "groups=industry tag_id(0..13), "
                "distribution=full data"
            ),
            x_label=spec["x_label"],
            palette_mode="industry",
        )
        industry_charts.append(industry_payload)
        print(f"prepared industry plot: {spec['column']}")

    drift_html = build_dashboard_html(
        "Feature Distribution Drift (Time Bins)",
        drift_charts,
    )
    industry_html = build_dashboard_html(
        "Feature Distribution by Industry (Global Time)",
        industry_charts,
    )

    drift_path = write_temp_html("drift", drift_html)
    industry_path = write_temp_html("industry", industry_html)

    webbrowser.open(drift_path.as_uri())
    webbrowser.open(industry_path.as_uri())

    print(f"opened drift page: {drift_path}")
    print(f"opened industry page: {industry_path}")


if __name__ == "__main__":
    main()
