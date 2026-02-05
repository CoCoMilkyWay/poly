from backend_api import BACKEND_API
from graph_status import get_graph_status_stream
from fastapi import FastAPI, Query, Request
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.templating import Jinja2Templates
from pathlib import Path
import httpx
import json

app = FastAPI(title="Polymarket Data Explorer")
templates = Jinja2Templates(directory=Path(__file__).parent / "templates")

# httpx 客户端，禁用代理直连 C++ backend
_client = httpx.Client(timeout=10, trust_env=False)


def backend_get(path: str, params: dict = None, default=None):
    """通用 backend GET 请求"""
    resp = _client.get(f"{BACKEND_API}{path}", params=params)
    return resp.json() if resp.text else (default if default is not None else {})


@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    """主页"""
    stats = backend_get("/api/stats", default={})
    sync_state_data = backend_get("/api/sync", default=[])
    sync_state = [
        (r.get("source"), r.get("entity"), r.get("last_id"), r.get("last_sync_at"))
        for r in sync_state_data
    ] if isinstance(sync_state_data, list) else []

    return templates.TemplateResponse("index.html", {
        "request": request,
        "stats": stats,
        "sync_state": sync_state,
    })


@app.get("/api/stats")
async def api_stats():
    """API: 获取统计信息"""
    return backend_get("/api/stats")


@app.get("/api/entity-stats")
async def api_entity_stats():
    """API: 获取 entity 实时统计"""
    return backend_get("/api/entity-stats")


@app.get("/api/entity-latest")
async def api_entity_latest(entity: str = Query(...)):
    """API: 获取某个 entity 最近一条记录（用于 hover）"""
    return backend_get("/api/entity-latest", {"entity": entity})


@app.get("/api/indexer-fails")
async def api_indexer_fails(source: str = Query(...), entity: str = Query(...)):
    """API: 获取某个 source/entity 的 indexer 失败计数"""
    return backend_get("/api/indexer-fails", {"source": source, "entity": entity}, default=[])


@app.get("/api/graph-status-stream")
async def api_graph_status_stream():
    """API: 流式获取 The Graph 节点状态 (SSE)"""
    async def event_generator():
        async for event in get_graph_status_stream():
            yield f"data: {json.dumps(event)}\n\n"

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"}
    )


def escape_sql(s: str) -> str:
    """SQL 转义 + 注入防护"""
    if not s:
        return ""
    # 禁止危险字符
    assert ';' not in s, "Semicolon not allowed in input"
    assert '--' not in s, "SQL comment not allowed in input"
    assert '/*' not in s, "SQL comment not allowed in input"
    # 转义单引号
    return s.replace("'", "''")


def sql_query(sql: str):
    """执行 SQL 查询"""
    return backend_get("/api/sql", {"q": sql})


@app.get("/api/positions")
async def api_positions(
    user: str = Query(None), token_id: str = Query(None),
    limit: int = Query(100, le=1000), offset: int = Query(0)
):
    """查询用户持仓"""
    sql = "SELECT * FROM user_position WHERE 1=1"
    if user:
        sql += f" AND user_addr = '{escape_sql(user)}'"
    if token_id:
        sql += f" AND token_id = '{escape_sql(token_id)}'"
    sql += f" ORDER BY id LIMIT {limit} OFFSET {offset}"
    return sql_query(sql)


@app.get("/api/orders")
async def api_orders(
    maker: str = Query(None), taker: str = Query(None), market_id: str = Query(None),
    start_time: int = Query(None), end_time: int = Query(None),
    limit: int = Query(100, le=1000), offset: int = Query(0)
):
    """查询订单成交"""
    sql = "SELECT * FROM enriched_order_filled WHERE 1=1"
    if maker:
        sql += f" AND maker = '{escape_sql(maker)}'"
    if taker:
        sql += f" AND taker = '{escape_sql(taker)}'"
    if market_id:
        sql += f" AND market_id = '{escape_sql(market_id)}'"
    if start_time:
        sql += f" AND timestamp >= {int(start_time)}"
    if end_time:
        sql += f" AND timestamp <= {int(end_time)}"
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    return sql_query(sql)


@app.get("/api/merges")
async def api_merges(
    stakeholder: str = Query(None), condition_id: str = Query(None),
    limit: int = Query(100, le=1000), offset: int = Query(0)
):
    """查询 Merge 事件"""
    sql = "SELECT * FROM merge WHERE 1=1"
    if stakeholder:
        sql += f" AND stakeholder = '{escape_sql(stakeholder)}'"
    if condition_id:
        sql += f" AND condition_id = '{escape_sql(condition_id)}'"
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    return sql_query(sql)


@app.get("/api/redemptions")
async def api_redemptions(
    redeemer: str = Query(None), condition_id: str = Query(None),
    limit: int = Query(100, le=1000), offset: int = Query(0)
):
    """查询 Redemption 事件"""
    sql = "SELECT * FROM redemption WHERE 1=1"
    if redeemer:
        sql += f" AND redeemer = '{escape_sql(redeemer)}'"
    if condition_id:
        sql += f" AND condition_id = '{escape_sql(condition_id)}'"
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    return sql_query(sql)


@app.get("/api/sql")
async def api_sql(q: str = Query(...)):
    """执行自定义 SQL 查询(只读)"""
    assert q.strip().upper().startswith("SELECT"), "只允许 SELECT 查询"
    return sql_query(q)


def _load_export_tables():
    """从 config.json 读取所有 entity -> table 映射"""
    config_path = Path(__file__).parent.parent / "config.json"
    with open(config_path) as f:
        config = json.load(f)
    tables = []
    for source in config.get("sources", {}).values():
        for table in source.get("entities", {}).values():
            if table not in tables:
                tables.append(table)
    return tables


def _get_order_column(table: str) -> str:
    """获取表的排序字段：优先 timestamp 类字段，否则用 id"""
    schema = backend_get(
        "/api/sql", {"q": f"SELECT column_name FROM information_schema.columns WHERE table_name = '{table}'"}, default=[])
    cols = {r.get("column_name", "").lower() for r in schema}
    # 优先级：timestamp > creation_timestamp > last_seen_timestamp > id
    for c in ["timestamp", "creation_timestamp", "last_seen_timestamp", "last_active_day"]:
        if c in cols:
            return c
    return "id"


@app.get("/api/export")
async def api_export(limit: int = Query(10000, le=100000)):
    """导出每个 entity 最近 N 条到 data/export/"""
    import csv

    export_dir = Path(__file__).parent.parent / "data" / "export"
    export_dir.mkdir(parents=True, exist_ok=True)

    tables = _load_export_tables()
    exported_tables = 0
    total_rows = 0

    for table in tables:
        order_col = _get_order_column(table)
        sql = f"SELECT * FROM {table} ORDER BY {order_col} DESC LIMIT {limit}"
        rows = sql_query(sql)
        if not rows or isinstance(rows, dict) and "error" in rows:
            continue

        file_path = export_dir / f"{table}.csv"
        with open(file_path, "w", newline="", encoding="utf-8") as f:
            if rows:
                writer = csv.DictWriter(f, fieldnames=rows[0].keys())
                writer.writeheader()
                writer.writerows(rows)

        exported_tables += 1
        total_rows += len(rows)

    return {"exported_tables": exported_tables, "total_rows": total_rows, "path": str(export_dir)}
