from backend_api import BACKEND_API
from graph_status import get_graph_status_stream
from fastapi import FastAPI, Query, Request
from fastapi.responses import HTMLResponse, Response, StreamingResponse
from fastapi.templating import Jinja2Templates
from pathlib import Path
import httpx
import json

app = FastAPI(title="Polymarket Data Explorer")
templates = Jinja2Templates(directory=Path(__file__).parent / "templates")

# httpx 客户端，禁用代理直连 C++ backend
_client = httpx.Client(timeout=None, trust_env=False)


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
    """API: 获取某个 entity 最近一条记录(用于 hover)"""
    return backend_get("/api/entity-latest", {"entity": entity})


@app.get("/api/indexer-fails")
async def api_indexer_fails(source: str = Query(...), entity: str = Query(...)):
    """API: 获取某个 source/entity 的 indexer 失败计数"""
    return backend_get("/api/indexer-fails", {"source": source, "entity": entity}, default=[])


@app.get("/api/replay-users")
async def api_replay_users(limit: int = Query(200)):
    resp = _client.get(f"{BACKEND_API}/api/replay-users", params={"limit": limit})
    return Response(content=resp.content, media_type="application/json")


@app.get("/api/replay-positions")
async def api_replay_positions(user: str = Query(...), ts: int = Query(...)):
    resp = _client.get(f"{BACKEND_API}/api/replay-positions", params={"user": user, "ts": ts})
    return Response(content=resp.content, media_type="application/json")


@app.get("/api/replay")
async def api_replay(user: str = Query(...)):
    resp = _client.get(f"{BACKEND_API}/api/replay", params={"user": user})
    return Response(content=resp.content, media_type="application/json")


@app.get("/api/rebuild-all")
async def api_rebuild_all():
    """API: 触发全量重建"""
    return backend_get("/api/rebuild-all")


@app.get("/api/rebuild-status")
async def api_rebuild_status():
    """API: 获取重建进度"""
    return backend_get("/api/rebuild-status")


@app.get("/api/rebuild-check-persist")
async def api_rebuild_check_persist():
    return backend_get("/api/rebuild-check-persist")


@app.get("/api/rebuild-load")
async def api_rebuild_load():
    return backend_get("/api/rebuild-load")



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


@app.get("/api/sync-progress")
async def api_sync_progress():
    return backend_get("/api/sync-progress")


@app.post("/api/fill-token-ids")
async def api_fill_token_ids():
    resp = _client.post(f"{BACKEND_API}/api/fill-token-ids")
    return resp.json()


@app.get("/api/export")
async def api_export(limit: int = Query(100, le=1000), order: str = Query("desc")):
    """从本地 DB 导出 entity 数据到 CSV
    order: desc=最新数据, asc=最早数据
    """
    resp = _client.get(
        f"{BACKEND_API}/api/export-raw", params={"limit": limit, "order": order})
    return resp.json()
