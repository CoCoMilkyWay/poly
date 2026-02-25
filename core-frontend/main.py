import json
from backend_api import backend_get
from fastapi import FastAPI, Query, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pathlib import Path

_cfg = json.loads((Path(__file__).parent.parent / "config.json").read_text())
_nodes = {n["name"]: n for n in _cfg["rpc_nodes"]}
_sync_chunk_blocks = 100_000
_sync_chunk_basics = int(_cfg["stage1_rpc_sync_chunk_basics"])
assert _sync_chunk_basics > 0
assert _sync_chunk_blocks % _sync_chunk_basics == 0
ACTIVE_RPC_NODE = {
    **_nodes[_cfg["active_rpc"]],
    "chunk": _sync_chunk_blocks // _sync_chunk_basics,
    "threads": int(_cfg["stage1_rpc_query_threads"]),
}

app = FastAPI(title="Polymarket Explorer")
templates = Jinja2Templates(directory=Path(__file__).parent / "templates")


@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    tables = await backend_get("/api/tables")
    sync_state = await backend_get("/api/sync-state")
    return templates.TemplateResponse("index.html", {
        "request": request,
        "tables": tables,
        "sync_state": sync_state,
        "rpc_node": ACTIVE_RPC_NODE,
    })


@app.get("/api/health")
async def api_health():
    return await backend_get("/api/health")


@app.get("/api/tables")
async def api_tables():
    return await backend_get("/api/tables")


@app.get("/api/sync-state")
async def api_sync_state():
    return await backend_get("/api/sync-state")


@app.get("/api/query")
async def api_query(q: str = Query(...)):
    return await backend_get("/api/query", {"q": q})


@app.get("/api/table-sample")
async def api_table_sample(table: str = Query(...)):
    return await backend_get("/api/table-sample", {"table": table})


@app.post("/api/export-all")
async def api_export_all():
    import asyncio
    feather_tables = [
        "transfer", "condition_preparation", "condition_resolution",
        "split", "merge", "redemption", "fpmm", "fpmm_trade", "fpmm_funding",
        "order_filled", "token_map", "neg_risk_market", "neg_risk_question", "convert"
    ]
    export_dir = Path(__file__).parent.parent / "data" / "export"
    export_dir.mkdir(parents=True, exist_ok=True)

    async def export_one(table_name: str):
        output = str(export_dir / f"{table_name}.csv")
        resp = await backend_get("/api/export-csv", {"table": table_name, "output": output, "limit": 1000})
        return table_name if resp.get("rows", 0) > 0 else None

    results = await asyncio.gather(*[export_one(t) for t in feather_tables])
    return {"exported": [r for r in results if r], "path": str(export_dir)}


@app.get("/api/rebuild-status")
async def api_rebuild_status():
    return await backend_get("/api/rebuild-status")


@app.get("/api/replay-users")
async def api_replay_users(limit: int = Query(200)):
    return await backend_get("/api/replay-users", {"limit": limit})


@app.get("/api/replay")
async def api_replay(user: str = Query(...)):
    return await backend_get("/api/replay", {"user": user})


@app.get("/api/replay-positions")
async def api_replay_positions(user: str = Query(...), sk: int = Query(...)):
    return await backend_get("/api/replay-positions", {"user": user, "sk": sk})


@app.get("/api/replay-trades")
async def api_replay_trades(user: str = Query(...), sk: int = Query(...), radius: int = Query(20)):
    return await backend_get("/api/replay-trades", {"user": user, "sk": sk, "radius": radius})
