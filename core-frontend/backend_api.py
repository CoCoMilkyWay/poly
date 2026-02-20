import httpx

BACKEND_URL = "http://127.0.0.1:8001"

_client: httpx.AsyncClient | None = None


async def get_client() -> httpx.AsyncClient:
    global _client
    if _client is None:
        _client = httpx.AsyncClient(base_url=BACKEND_URL, timeout=None)
    return _client


async def backend_get(path: str, params: dict = None):
    client = await get_client()
    resp = await client.get(path, params=params)
    return resp.json() if resp.text else {}


async def backend_post(path: str, data: dict = None):
    client = await get_client()
    resp = await client.post(path, json=data)
    return resp.json() if resp.text else {}
