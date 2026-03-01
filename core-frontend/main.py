import json
from backend_api import backend_get
from fastapi import FastAPI, Query, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pathlib import Path

_cfg = json.loads((Path(__file__).parent.parent / "config.json").read_text())
_nodes = {n["name"]: n for n in _cfg["rpc_nodes"]}
_stage1_chunk_blocks = 100_000
_stage1_chunk_basics = int(_cfg["stage1_rpc_chunk_basics"])
assert _stage1_chunk_basics > 0
assert _stage1_chunk_blocks % _stage1_chunk_basics == 0
ACTIVE_RPC_NODE = {
    **_nodes[_cfg["active_rpc"]],
    "chunk": _stage1_chunk_blocks // _stage1_chunk_basics,
    "threads": int(_cfg["stage1_rpc_threads"]),
}

app = FastAPI(title="Polymarket Explorer")
templates = Jinja2Templates(directory=Path(__file__).parent / "templates")


@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    tables = await backend_get("/api/tables")
    stage1_state = await backend_get("/api/stage1-status")
    return templates.TemplateResponse("index.html", {
        "request": request,
        "tables": tables,
        "stage1_state": stage1_state,
        "rpc_node": ACTIVE_RPC_NODE,
    })


@app.get("/api/health")
async def api_health():
    return await backend_get("/api/health")


@app.get("/api/tables")
async def api_tables():
    return await backend_get("/api/tables")


@app.get("/api/stage1-status")
async def api_stage1_status():
    return await backend_get("/api/stage1-status")


@app.get("/api/stage2-status")
async def api_stage2_status():
    return await backend_get("/api/stage2-status")


@app.get("/api/stage3-status")
async def api_stage3_status():
    return await backend_get("/api/stage3-status")


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


@app.get("/api/stage2-data")
async def api_stage2_data():
    return await backend_get("/api/stage2-data")


@app.get("/api/stage3-users")
async def api_stage3_users(limit: int = Query(200)):
    return await backend_get("/api/stage3-users", {"limit": limit})


@app.get("/api/stage3-data")
async def api_stage3_data(user: str = Query(...)):
    return await backend_get("/api/stage3-data", {"user": user})


@app.get("/api/stage3-positions")
async def api_stage3_positions(user: str = Query(...), sk: int = Query(...)):
    return await backend_get("/api/stage3-positions", {"user": user, "sk": sk})


@app.get("/api/stage3-events")
async def api_stage3_events(user: str = Query(...), sk: int = Query(...), radius: int = Query(20)):
    return await backend_get("/api/stage3-events", {"user": user, "sk": sk, "radius": radius})
