# API

## File Hierarchy

```
core-backend/src/
├── main.cpp                 # 入口, 启动 API Server
├── core/
│   ├── config.hpp           # Config::load() 加载 config.json
│   └── database.hpp         # Database 类, DuckDB 封装 + flock 进程锁
└── api/
    ├── api_server.hpp       # ApiServer, boost::beast HTTP 监听
    └── api_session.hpp      # ApiSession, 请求路由和处理

core-frontend/
├── main.py                  # FastAPI app, 路由定义
├── backend_api.py           # 异步 httpx 客户端, 调用后端 API
└── templates/
    └── index.html           # Jinja2 模板
```

## Backend API (C++, :8001)

| 端点              | 方法 | 参数          | 返回                                  |
| ----------------- | ---- | ------------- | ------------------------------------- |
| `/api/health`     | GET  | -             | `{"status":"ok"}`                     |
| `/api/tables`     | GET  | -             | `[{"name":"xxx","count":123},...]`    |
| `/api/sync-state` | GET  | -             | `{"last_block":12345}`                |
| `/api/query`      | GET  | `q=SELECT...` | `[{row1},{row2},...]` (只允许 SELECT) |

## Frontend API (Python, :8000)

| 端点              | 方法 | 说明      |
| ----------------- | ---- | --------- |
| `/`               | GET  | HTML 页面 |
| `/api/health`     | GET  | 透传后端  |
| `/api/tables`     | GET  | 透传后端  |
| `/api/sync-state` | GET  | 透传后端  |
| `/api/query`      | GET  | 透传后端  |

## Database (DuckDB)

进程锁机制：

```cpp
Database db("data/polymarket.duckdb");

{
  Database::WriteLock lock(db);  // flock(LOCK_EX)
  db.execute("INSERT ...");
}  // 自动 flock(LOCK_UN)

// 读操作不需要锁
auto rows = db.query_json("SELECT ...");
```
