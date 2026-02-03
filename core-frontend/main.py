from fastapi import FastAPI, Query, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pathlib import Path
import duckdb
import json

app = FastAPI(title="Polymarket Data Explorer")

# 模板目录
templates = Jinja2Templates(directory=Path(__file__).parent / "templates")

# 数据库路径
DB_PATH = Path(__file__).parent.parent / "data" / "polymarket.duckdb"


def get_db():
    """获取数据库连接"""
    return duckdb.connect(str(DB_PATH), read_only=True)


@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    """主页"""
    # 获取统计信息
    stats = {}
    try:
        conn = get_db()
        
        # 各表行数
        tables = [
            "split", "merge", "redemption", "neg_risk_conversion",
            "user_position", "neg_risk_event", "condition",
            "order_filled_event", "orderbook", "market_data",
            "market_open_interest", "global_open_interest",
            "fixed_product_market_maker", "fpmm_transaction"
        ]
        
        for table in tables:
            try:
                result = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()
                stats[table] = result[0] if result else 0
            except:
                stats[table] = 0
        
        # 同步状态
        try:
            sync_state = conn.execute(
                "SELECT subgraph_id, entity_name, last_id, last_sync_at FROM sync_state ORDER BY last_sync_at DESC"
            ).fetchall()
        except:
            sync_state = []
        
        conn.close()
    except Exception as e:
        stats = {"error": str(e)}
        sync_state = []
    
    return templates.TemplateResponse("index.html", {
        "request": request,
        "stats": stats,
        "sync_state": sync_state
    })


@app.get("/api/stats")
async def api_stats():
    """API: 获取统计信息"""
    conn = get_db()
    
    stats = {}
    tables = [
        "split", "merge", "redemption", "user_position",
        "order_filled_event", "orderbook"
    ]
    
    for table in tables:
        try:
            result = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()
            stats[table] = result[0] if result else 0
        except:
            stats[table] = 0
    
    conn.close()
    return stats


@app.get("/api/positions")
async def api_positions(
    user: str = Query(None, description="用户地址"),
    token_id: str = Query(None, description="Token ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询用户持仓"""
    conn = get_db()
    
    sql = "SELECT * FROM user_position WHERE 1=1"
    params = []
    
    if user:
        sql += " AND user_addr = ?"
        params.append(user)
    
    if token_id:
        sql += " AND token_id = ?"
        params.append(token_id)
    
    sql += f" ORDER BY id LIMIT {limit} OFFSET {offset}"
    
    try:
        result = conn.execute(sql, params).fetchall()
        columns = ["id", "user_addr", "token_id", "amount", "avg_price", "realized_pnl", "total_bought"]
        data = [dict(zip(columns, row)) for row in result]
    except Exception as e:
        data = {"error": str(e)}
    
    conn.close()
    return data


@app.get("/api/orders")
async def api_orders(
    maker: str = Query(None, description="Maker 地址"),
    taker: str = Query(None, description="Taker 地址"),
    start_time: int = Query(None, description="开始时间戳"),
    end_time: int = Query(None, description="结束时间戳"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询订单成交"""
    conn = get_db()
    
    sql = "SELECT * FROM order_filled_event WHERE 1=1"
    params = []
    
    if maker:
        sql += " AND maker = ?"
        params.append(maker)
    
    if taker:
        sql += " AND taker = ?"
        params.append(taker)
    
    if start_time:
        sql += " AND timestamp >= ?"
        params.append(start_time)
    
    if end_time:
        sql += " AND timestamp <= ?"
        params.append(end_time)
    
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    
    try:
        result = conn.execute(sql, params).fetchall()
        columns = ["id", "transaction_hash", "timestamp", "order_hash", "maker", "taker",
                   "maker_asset_id", "taker_asset_id", "maker_amount_filled", "taker_amount_filled", "fee"]
        data = [dict(zip(columns, row)) for row in result]
    except Exception as e:
        data = {"error": str(e)}
    
    conn.close()
    return data


@app.get("/api/splits")
async def api_splits(
    stakeholder: str = Query(None, description="用户地址"),
    condition: str = Query(None, description="Condition ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询 Split 事件"""
    conn = get_db()
    
    sql = "SELECT * FROM split WHERE 1=1"
    params = []
    
    if stakeholder:
        sql += " AND stakeholder = ?"
        params.append(stakeholder)
    
    if condition:
        sql += " AND condition = ?"
        params.append(condition)
    
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    
    try:
        result = conn.execute(sql, params).fetchall()
        columns = ["id", "timestamp", "stakeholder", "condition", "amount"]
        data = [dict(zip(columns, row)) for row in result]
    except Exception as e:
        data = {"error": str(e)}
    
    conn.close()
    return data


@app.get("/api/merges")
async def api_merges(
    stakeholder: str = Query(None, description="用户地址"),
    condition: str = Query(None, description="Condition ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询 Merge 事件"""
    conn = get_db()
    
    sql = "SELECT * FROM merge WHERE 1=1"
    params = []
    
    if stakeholder:
        sql += " AND stakeholder = ?"
        params.append(stakeholder)
    
    if condition:
        sql += " AND condition = ?"
        params.append(condition)
    
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    
    try:
        result = conn.execute(sql, params).fetchall()
        columns = ["id", "timestamp", "stakeholder", "condition", "amount"]
        data = [dict(zip(columns, row)) for row in result]
    except Exception as e:
        data = {"error": str(e)}
    
    conn.close()
    return data


@app.get("/api/redemptions")
async def api_redemptions(
    redeemer: str = Query(None, description="用户地址"),
    condition: str = Query(None, description="Condition ID"),
    limit: int = Query(100, le=1000),
    offset: int = Query(0)
):
    """API: 查询 Redemption 事件"""
    conn = get_db()
    
    sql = "SELECT * FROM redemption WHERE 1=1"
    params = []
    
    if redeemer:
        sql += " AND redeemer = ?"
        params.append(redeemer)
    
    if condition:
        sql += " AND condition = ?"
        params.append(condition)
    
    sql += f" ORDER BY timestamp DESC LIMIT {limit} OFFSET {offset}"
    
    try:
        result = conn.execute(sql, params).fetchall()
        columns = ["id", "timestamp", "redeemer", "condition", "index_sets", "payout"]
        data = [dict(zip(columns, row)) for row in result]
    except Exception as e:
        data = {"error": str(e)}
    
    conn.close()
    return data


@app.get("/api/sql")
async def api_sql(
    q: str = Query(..., description="SQL 查询语句")
):
    """API: 执行自定义 SQL 查询（只读）"""
    # 安全检查：只允许 SELECT
    q_upper = q.strip().upper()
    assert q_upper.startswith("SELECT"), "只允许 SELECT 查询"
    
    conn = get_db()
    
    try:
        result = conn.execute(q).fetchall()
        columns = [desc[0] for desc in conn.description()]
        data = [dict(zip(columns, row)) for row in result]
    except Exception as e:
        data = {"error": str(e)}
    
    conn.close()
    return data
