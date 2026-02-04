from fastapi import FastAPI, Query, Request
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.templating import Jinja2Templates
from pathlib import Path
import httpx
import json

from graph_status import get_graph_status_stream

app = FastAPI(title="Polymarket Data Explorer")

# 模板目录
templates = Jinja2Templates(directory=Path(__file__).parent / "templates")

# C++ backend API
BACKEND_API = "http://127.0.0.1:8001"

# 创建 httpx 客户端，禁用代理(trust_env=False 忽略环境变量代理, 直接访问C++后端)
_client = httpx.Client(timeout=10, trust_env=False)


def backend_query(sql: str):
    """通过 C++ backend 执行查询"""
    try:
        resp = _client.get(f"{BACKEND_API}/api/sql", params={"q": sql})
        return resp.json() if resp.text else {"error": "Empty response"}
    except Exception as e:
        return {"error": str(e)}


def backend_stats():
    """获取统计信息"""
    try:
        resp = _client.get(f"{BACKEND_API}/api/stats")
        return resp.json() if resp.text else {"error": "Empty response"}
    except Exception as e:
        return {"error": str(e)}


def backend_sync_state():
    """获取同步状态"""
    try:
        resp = _client.get(f"{BACKEND_API}/api/sync")
        return resp.json() if resp.text else []
    except Exception as e:
        return []


@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    """主页"""
    try:
        stats = backend_stats()
        sync_state_data = backend_sync_state()
        # 转换为 tuple 格式以兼容模板
        sync_state = [
            (r.get("source"), r.get("entity"), r.get("last_id"), 
             r.get("last_sync_at"), r.get("total_synced"))
            for r in sync_state_data
        ] if isinstance(sync_state_data, list) else []
    except Exception as e:
        stats = {"error": str(e)}
        sync_state = []
    
    return templates.TemplateResponse("index.html", {
        "request": request,
        "stats": stats,
        "sync_state": sync_state,
    })


@app.get("/api/stats")
async def api_stats():
    """API: 获取统计信息"""
    return backend_stats()


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
    """简单 SQL 转义"""
    return s.replace("'", "''") if s else ""


@app.get("/api/positions")
async def api_positions(
    user: str = Query(None, description="用户地址"),
    token_id: str = Query(None, description="Token ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询用户持仓"""
    sql = "SELECT * FROM user_position WHERE 1=1"
    
    if user:
        sql += f" AND user_addr = '{escape_sql(user)}'"
    if token_id:
        sql += f" AND token_id = '{escape_sql(token_id)}'"
    
    sql += f" ORDER BY id LIMIT {limit} OFFSET {offset}"
    return backend_query(sql)


@app.get("/api/orders")
async def api_orders(
    maker: str = Query(None, description="Maker 地址"),
    taker: str = Query(None, description="Taker 地址"),
    market_id: str = Query(None, description="Market ID"),
    start_time: int = Query(None, description="开始时间戳"),
    end_time: int = Query(None, description="结束时间戳"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询订单成交"""
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
    return backend_query(sql)


@app.get("/api/splits")
async def api_splits(
    stakeholder: str = Query(None, description="用户地址"),
    condition_id: str = Query(None, description="Condition ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询 Split 事件"""
    sql = "SELECT * FROM split WHERE 1=1"
    
    if stakeholder:
        sql += f" AND stakeholder = '{escape_sql(stakeholder)}'"
    if condition_id:
        sql += f" AND condition_id = '{escape_sql(condition_id)}'"
    
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    return backend_query(sql)


@app.get("/api/merges")
async def api_merges(
    stakeholder: str = Query(None, description="用户地址"),
    condition_id: str = Query(None, description="Condition ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询 Merge 事件"""
    sql = "SELECT * FROM merge WHERE 1=1"
    
    if stakeholder:
        sql += f" AND stakeholder = '{escape_sql(stakeholder)}'"
    if condition_id:
        sql += f" AND condition_id = '{escape_sql(condition_id)}'"
    
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    return backend_query(sql)


@app.get("/api/redemptions")
async def api_redemptions(
    redeemer: str = Query(None, description="用户地址"),
    condition_id: str = Query(None, description="Condition ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询 Redemption 事件"""
    sql = "SELECT * FROM redemption WHERE 1=1"
    
    if redeemer:
        sql += f" AND redeemer = '{escape_sql(redeemer)}'"
    if condition_id:
        sql += f" AND condition_id = '{escape_sql(condition_id)}'"
    
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    return backend_query(sql)


@app.get("/api/sql")
async def api_sql(
    q: str = Query(..., description="SQL 查询语句")
):
    """API: 执行自定义 SQL 查询(只读)"""
    # 安全检查：只允许 SELECT
    q_upper = q.strip().upper()
    assert q_upper.startswith("SELECT"), "只允许 SELECT 查询"
    return backend_query(q)
