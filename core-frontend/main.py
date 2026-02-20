import json
from backend_api import backend_get
from fastapi import FastAPI, Query, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pathlib import Path

_cfg = json.loads((Path(__file__).parent.parent / "config.json").read_text())
_nodes = {n["name"]: n for n in _cfg["rpc_nodes"]}
ACTIVE_RPC_NODE = _nodes[_cfg["active_rpc"]]

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


@app.post("/api/export-all")
async def api_export_all():
    table_order_by = {
        "order_filled": "block_number DESC, log_index DESC",
        "split": "block_number DESC, log_index DESC",
        "merge": "block_number DESC, log_index DESC",
        "redemption": "block_number DESC, log_index DESC",
        "convert": "block_number DESC, log_index DESC",
        "transfer": "block_number DESC, log_index DESC",
        "condition": "resolution_block DESC",
        "token_map": None,
        "neg_risk_market": None,
        "neg_risk_question": None,
        "fpmm": "block_number DESC",
        "fpmm_trade": "block_number DESC, log_index DESC",
        "fpmm_funding": "block_number DESC, log_index DESC",
    }
    export_dir = Path(__file__).parent.parent / "data" / "export"
    export_dir.mkdir(parents=True, exist_ok=True)

    results = []

    for table_name, order_by in table_order_by.items():
        col_query = f"SELECT column_name FROM information_schema.columns WHERE table_name = '{table_name}' ORDER BY ordinal_position"
        cols_result = await backend_get("/api/query", {"q": col_query})
        headers = [c["column_name"] for c in cols_result]

        if order_by:
            query = f"SELECT * FROM {table_name} ORDER BY {order_by} LIMIT 1000"
        else:
            query = f"SELECT * FROM {table_name} LIMIT 1000"
        rows = await backend_get("/api/query", {"q": query})

        if rows:
            lines = [",".join(headers)]
            for row in rows:
                vals = []
                for h in headers:
                    val = row[h]
                    if val is None:
                        vals.append("")
                    elif isinstance(val, str) and ("," in val or '"' in val or "\n" in val):
                        vals.append('"' + val.replace('"', '""') + '"')
                    else:
                        vals.append(str(val))
                lines.append(",".join(vals))

            csv_content = "\n".join(lines)
            file_path = export_dir / f"{table_name}.csv"
            file_path.write_text(csv_content)
            results.append(table_name)

    return {"exported": results, "path": str(export_dir)}
