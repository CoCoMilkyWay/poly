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
