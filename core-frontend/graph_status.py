"""The Graph 节点状态查询"""
import json
import yaml
import asyncio
from pathlib import Path
from typing import Optional, AsyncGenerator
import httpx
from functools import lru_cache

from backend_api import BACKEND_API

# 常量
CHAIN_RPCS = {
    "matic": "https://polygon-rpc.com",
    "polygon": "https://polygon-rpc.com",
    "mainnet": "https://eth.llamarpc.com",
    "arbitrum-one": "https://arb1.arbitrum.io/rpc",
    "optimism": "https://mainnet.optimism.io",
    "base": "https://mainnet.base.org",
}
NETWORK_NAMES = {
    "matic": "Polygon", "polygon": "Polygon",
    "mainnet": "Ethereum", "arbitrum-one": "Arbitrum",
    "optimism": "Optimism", "base": "Base",
}
IPFS_GATEWAY = "https://ipfs.network.thegraph.com/api/v0/cat"
NETWORK_SUBGRAPH_ID = "DZz4kDTdmzWLWsV373w2bSmoar3umKKH9y82SUKr5qmp"

_manifest_cache = {}


@lru_cache(maxsize=1)
def get_config():
    config_path = Path(__file__).parent.parent / "config.json"
    with open(config_path) as f:
        return json.load(f)


async def fetch_subgraph_meta(client: httpx.AsyncClient, subgraph_id: str, api_key: str) -> dict:
    """查询 subgraph 的 _meta 信息"""
    query = '{"query":"{_meta{block{number hash timestamp}deployment hasIndexingErrors}}"}'
    url = f"https://gateway.thegraph.com/api/subgraphs/id/{subgraph_id}"
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}"
    }
    try:
        resp = await client.post(url, content=query, headers=headers, timeout=15)
        data = resp.json()
        if "data" in data and "_meta" in data["data"]:
            return data["data"]["_meta"]
        if "errors" in data:
            return {"error": data["errors"][0].get("message", "Unknown error")}
        return {"error": "Invalid response"}
    except Exception as e:
        return {"error": str(e)}


async def fetch_deployment_indexers(client: httpx.AsyncClient, deployment_ipfs: str, api_key: str) -> dict:
    """查询 Network Subgraph 获取 deployment 的 indexer 分配情况"""
    query = '''
    {
      subgraphDeployments(where: {ipfsHash: "%s"}, first: 1) {
        id
        ipfsHash
        stakedTokens
        signalledTokens
        indexerAllocations(first: 50, orderBy: allocatedTokens, orderDirection: desc, where: {status: Active}) {
          id
          allocatedTokens
          indexer {
            id
            stakedTokens
            defaultDisplayName
          }
        }
      }
    }
    ''' % deployment_ipfs
    
    url = f"https://gateway.thegraph.com/api/subgraphs/id/{NETWORK_SUBGRAPH_ID}"
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}"
    }
    
    try:
        resp = await client.post(url, json={"query": query}, headers=headers, timeout=15)
        data = resp.json()
        if "data" in data and data["data"].get("subgraphDeployments"):
            deps = data["data"]["subgraphDeployments"]
            if not deps:
                return {"indexer_count": 0, "indexers": []}
            dep = deps[0]
            allocations = dep.get("indexerAllocations", [])
            return {
                "indexer_count": len(allocations),
                "total_stake": dep.get("stakedTokens", "0"),
                "signal": dep.get("signalledTokens", "0"),
                "indexers": [
                    {
                        "id": a["indexer"]["id"],
                        "name": a["indexer"].get("defaultDisplayName") or a["indexer"]["id"][:10] + "...",
                        "allocated": a["allocatedTokens"],
                    }
                    for a in allocations[:10]
                ]
            }
        if "errors" in data:
            return {"error": data["errors"][0].get("message", "Unknown"), "indexer_count": 0, "indexers": []}
        return {"indexer_count": 0, "indexers": []}
    except Exception as e:
        return {"error": str(e), "indexer_count": 0, "indexers": []}


async def fetch_chain_block(client: httpx.AsyncClient, network: str, cache: dict) -> Optional[int]:
    """查询指定链的最新 block"""
    if network in cache:
        return cache[network]
    
    rpc_url = CHAIN_RPCS.get(network)
    if not rpc_url:
        return None
    
    payload = {
        "jsonrpc": "2.0",
        "method": "eth_blockNumber",
        "params": [],
        "id": 1
    }
    try:
        resp = await client.post(rpc_url, json=payload, timeout=10)
        data = resp.json()
        if "result" in data:
            block = int(data["result"], 16)
            cache[network] = block
            return block
    except:
        pass
    return None


async def fetch_entity_stats(client: httpx.AsyncClient) -> dict:
    """获取后端 entity 实时统计(包含 count)"""
    try:
        resp = await client.get(f"{BACKEND_API}/api/entity-stats", timeout=5)
        if resp.status_code == 200:
            return resp.json()
    except:
        pass
    return {}


async def fetch_manifest(client: httpx.AsyncClient, deployment_cid: str) -> Optional[dict]:
    """从 IPFS 获取 subgraph manifest"""
    if deployment_cid in _manifest_cache:
        return _manifest_cache[deployment_cid]
    
    try:
        url = f"{IPFS_GATEWAY}?arg={deployment_cid}"
        resp = await client.get(url, timeout=15)
        if resp.status_code == 200:
            manifest = yaml.safe_load(resp.text)
            _manifest_cache[deployment_cid] = manifest
            return manifest
    except:
        pass
    
    # 备用: 公共 IPFS gateway
    try:
        url = f"https://ipfs.io/ipfs/{deployment_cid}"
        resp = await client.get(url, timeout=15)
        if resp.status_code == 200:
            manifest = yaml.safe_load(resp.text)
            _manifest_cache[deployment_cid] = manifest
            return manifest
    except:
        pass
    
    return None


def parse_contract_nodes(manifest: dict) -> tuple[list, list]:
    """解析 manifest 中的 dataSources 和 templates，返回 (nodes, output_entities)"""
    if not manifest:
        return [], []
    
    nodes = []
    all_entities = set()
    
    # 静态合约 (dataSources)
    for ds in manifest.get("dataSources", []):
        source = ds.get("source", {})
        entities = ds.get("mapping", {}).get("entities", [])
        all_entities.update(entities)
        nodes.append({
            "name": ds.get("name", "unknown"),
            "type": "static",
            "network": ds.get("network", "unknown"),
            "network_display": NETWORK_NAMES.get(ds.get("network", ""), ds.get("network", "unknown")),
            "address": source.get("address", ""),
            "start_block": source.get("startBlock", 0),
            "entities": entities,
        })
    
    # 动态合约 (templates)
    for tpl in manifest.get("templates", []):
        entities = tpl.get("mapping", {}).get("entities", [])
        all_entities.update(entities)
        nodes.append({
            "name": tpl.get("name", "unknown"),
            "type": "dynamic",
            "network": tpl.get("network", "unknown"),
            "network_display": NETWORK_NAMES.get(tpl.get("network", ""), tpl.get("network", "unknown")),
            "address": "",
            "start_block": 0,
            "entities": entities,
        })
    
    return nodes, sorted(all_entities)


def compute_node_stats(nodes: list, indexed_block: int, chain_block_cache: dict):
    """计算每个合约节点的统计信息"""
    progress_list = []
    for node in nodes:
        network = node.get("network", "")
        chain_head = chain_block_cache.get(network)
        
        node["indexed"] = indexed_block
        node["head"] = chain_head
        node["behind"] = (chain_head - indexed_block) if chain_head and indexed_block else 0
        
        # 动态合约没有 start_block，不计算 progress
        if node.get("type") == "dynamic":
            continue
        
        start_block = node.get("start_block", 0)
        
        if chain_head and indexed_block:
            total_range = chain_head - start_block
            indexed_range = indexed_block - start_block
            if total_range > 0:
                node["progress"] = round(indexed_range / total_range * 100, 2)
            else:
                node["progress"] = 100.0
            progress_list.append(node["progress"])
        else:
            node["progress"] = 0
    
    return round(sum(progress_list) / len(progress_list), 2) if progress_list else 0


async def get_graph_status_stream() -> AsyncGenerator[dict, None]:
    """流式获取 graph 状态，每一步 yield 进度"""
    config = get_config()
    api_key = config.get("api_key", "")
    sources = config.get("sources", {})
    
    enabled_sources = {k: v for k, v in sources.items() if v.get("enabled", False)}
    total = len(enabled_sources)
    
    if total == 0:
        yield {"type": "status", "msg": "没有启用的数据源"}
        yield {"type": "done", "data": {"sources": {}}}
        return
    
    result = {"sources": {}}
    chain_block_cache = {}
    backend_entity_stats = {}
    
    async with httpx.AsyncClient() as client:
        # Step 0: 获取后端 entity stats(避免触发后端 COUNT(*))
        backend_entity_stats = await fetch_entity_stats(client)
        # Step 1: 并发查询所有 subgraph 的 meta
        yield {"type": "status", "msg": f"查询 {total} 个 subgraph meta..."}
        
        async def fetch_meta_task(name, src):
            subgraph_id = src.get("subgraph_id", "")
            meta = await fetch_subgraph_meta(client, subgraph_id, api_key)
            return name, src, meta
        
        meta_tasks = [fetch_meta_task(name, src) for name, src in enabled_sources.items()]
        meta_results = await asyncio.gather(*meta_tasks)
        
        # Step 2: 收集所有需要查询的 deployment CID
        yield {"type": "status", "msg": "获取 manifest 和 indexer 信息..."}
        
        deployment_tasks = []
        for name, src, meta in meta_results:
            if not meta.get("error") and meta.get("deployment"):
                deployment_cid = meta["deployment"]
                # 并发: manifest + indexer
                deployment_tasks.append((name, src, meta, deployment_cid))
        
        # 并发查询所有 manifest 和 indexer
        async def fetch_deployment_info(name, src, meta, deployment_cid):
            manifest_task = fetch_manifest(client, deployment_cid)
            indexer_task = fetch_deployment_indexers(client, deployment_cid, api_key)
            manifest, indexer_info = await asyncio.gather(manifest_task, indexer_task)
            return name, src, meta, deployment_cid, manifest, indexer_info
        
        if deployment_tasks:
            dep_results = await asyncio.gather(*[
                fetch_deployment_info(*t) for t in deployment_tasks
            ])
        else:
            dep_results = []
        
        # 构建 deployment 信息映射
        dep_info_map = {}
        networks_needed = set()
        for name, src, meta, deployment_cid, manifest, indexer_info in dep_results:
            contract_nodes, output_entities = parse_contract_nodes(manifest)
            dep_info_map[name] = {
                "deployment_cid": deployment_cid,
                "manifest": manifest,
                "contract_nodes": contract_nodes,
                "output_entities": output_entities,
                "indexer_info": indexer_info,
            }
            for node in contract_nodes:
                networks_needed.add(node.get("network", ""))
        
        # Step 3: 并发查询所有需要的 chain head
        networks_needed.discard("")
        if networks_needed:
            yield {"type": "status", "msg": f"查询 {len(networks_needed)} 条链的区块高度..."}
            
            async def fetch_chain_task(network):
                block = await fetch_chain_block(client, network, chain_block_cache)
                return network, block
            
            chain_results = await asyncio.gather(*[fetch_chain_task(n) for n in networks_needed])
            for network, block in chain_results:
                if block:
                    chain_block_cache[network] = block
        
        # Step 4: 组装结果
        yield {"type": "status", "msg": "计算统计数据..."}
        
        for name, src, meta in meta_results:
            subgraph_id = src.get("subgraph_id", "")
            # entities 现在是 dict: {entity_name: table_name}
            entity_table_map = src.get("entities", {})
            configured_entities = list(entity_table_map.keys())
            
            if name in dep_info_map:
                info = dep_info_map[name]
                deployment_cid = info["deployment_cid"]
                contract_nodes = info["contract_nodes"]
                output_entities = info["output_entities"]
                indexer_info = info["indexer_info"]
            else:
                deployment_cid = None
                contract_nodes = []
                output_entities = []
                indexer_info = {"indexer_count": 0, "indexers": []}
            
            indexed_block = meta.get("block", {}).get("number", 0) if not meta.get("error") else 0
            avg_progress = compute_node_stats(contract_nodes, indexed_block, chain_block_cache)
            
            # 计算每个 configured entity 的本地记录数(按 source/entity)
            entity_stats = {}
            for entity in configured_entities:
                key = f"{name}/{entity}"
                stat = backend_entity_stats.get(key, {})
                entity_stats[entity] = stat.get("count", 0) if isinstance(stat, dict) else 0
            
            result["sources"][name] = {
                "source_name": name,  # 用于前端构建 entity stats key
                "subgraph_id": subgraph_id,
                "deployment_cid": deployment_cid,
                "meta": meta,
                "contract_nodes": contract_nodes,
                "output_entities": output_entities,
                "configured_entities": configured_entities,  # 配置的 entities
                "entity_stats": entity_stats,  # 每个 entity 的本地记录数
                "indexer_info": indexer_info,
                "stats": {
                    "progress": avg_progress,
                }
            }
    
    yield {"type": "status", "msg": "完成"}
    yield {"type": "done", "data": result}
