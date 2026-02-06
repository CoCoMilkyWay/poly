#!/usr/bin/env python3
import json
import sys
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent

# (名字, subgraph_id, deployment_id)
SUBGRAPHS = [
    ("Polymarket", "81Dm16JjuFSrqz813HysXoUPvzTwE7fsfPk2RTf66nyC",
     "QmdyCguLEisTtQFveEkvMhTH7UzjyhnrF9kpvhYeG4QX8a"),
    ("Polymarket Activity Polygon", "Bx1W4S7kDVxs9gC3s2G6DS8kdNBJNVhMviCtin2DiBp",
     "Qmf3qPUsfQ8et6E3QNBmuXXKqUJi91mo5zbsaTkQrSnMAP"),
    ("Polymarket Profit and Loss", "6c58N5U4MtQE2Y8njfVrrAfRykzfqajMGeTMEvMmskVz",
     "QmZAYiMeZiWC7ZjdWepek7hy1jbcW3ngimBF9ibTiTtwQU"),
]

INTROSPECTION_QUERY = """
query IntrospectionQuery {
  __schema {
    types {
      ...FullType
    }
  }
}

fragment FullType on __Type {
  kind
  name
  description
  fields(includeDeprecated: true) {
    name
    description
    args {
      ...InputValue
    }
    type {
      ...TypeRef
    }
    isDeprecated
    deprecationReason
  }
  inputFields {
    ...InputValue
  }
  interfaces {
    ...TypeRef
  }
  enumValues(includeDeprecated: true) {
    name
    description
    isDeprecated
    deprecationReason
  }
  possibleTypes {
    ...TypeRef
  }
}

fragment InputValue on __InputValue {
  name
  description
  type {
    ...TypeRef
  }
  defaultValue
}

fragment TypeRef on __Type {
  kind
  name
  ofType {
    kind
    name
    ofType {
      kind
      name
      ofType {
        kind
        name
        ofType {
          kind
          name
          ofType {
            kind
            name
            ofType {
              kind
              name
              ofType {
                kind
                name
              }
            }
          }
        }
      }
    }
  }
}
"""

API_KEY = "1d7a83f3e6778cd93dfbae707bb192de"


def fetch_schema(subgraph_id):
    """通过 introspection query 拉取 schema"""
    url = f"https://gateway.thegraph.com/api/{API_KEY}/subgraphs/id/{subgraph_id}"
    payload = json.dumps({"query": INTROSPECTION_QUERY}).encode()
    headers = {
        "Content-Type": "application/json",
        "User-Agent": "Mozilla/5.0",
    }
    req = urllib.request.Request(url, data=payload, headers=headers)
    try:
        with urllib.request.urlopen(req) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        print(f"  URL: {url}")
        print(f"  Error: {e.code} {e.reason}")
        print(f"  Body: {e.read().decode()}")
        raise


def resolve_type(t):
    """递归解析类型，返回类型字符串"""
    if t is None:
        return "?"
    kind = t.get("kind")
    name = t.get("name")
    of_type = t.get("ofType")

    if kind == "NON_NULL":
        return resolve_type(of_type) + "!"
    elif kind == "LIST":
        return "[" + resolve_type(of_type) + "]"
    elif name:
        return name
    return "?"


def convert_schema(data, output_file=None):
    assert "data" in data, f"API 错误: {data.get('errors', data)}"
    types = data["data"]["__schema"]["types"]

    # 建立类型索引
    type_map = {t["name"]: t for t in types if t.get("name")}

    # 分类
    entities = []  # 核心实体
    orderby_enums = {}  # entity_name -> enum
    enums = []
    scalars = []

    skip_prefixes = ("__",)
    builtin_scalars = {"String", "Int", "Float", "Boolean", "ID"}
    entity_names = set()

    for t in types:
        name = t.get("name", "")
        kind = t.get("kind")

        if any(name.startswith(p) for p in skip_prefixes):
            continue

        if kind == "OBJECT" and name not in ("Query", "Subscription"):
            entities.append(t)
            entity_names.add(name)
        elif kind == "ENUM":
            if name.endswith("_orderBy"):
                orderby_enums[name[:-8]] = t  # 去掉 _orderBy 后缀
            elif not name.endswith("_filter"):
                enums.append(t)
        elif kind == "SCALAR" and name not in builtin_scalars:
            scalars.append(t)

    lines = []

    # 自定义 Scalars
    if scalars:
        lines.append("# === SCALARS ===\n")
        for s in sorted(scalars, key=lambda x: x["name"]):
            lines.append(f"scalar {s['name']}")
        lines.append("")

    # 核心实体
    lines.append("# === ENTITIES ===\n")
    for entity in sorted(entities, key=lambda x: x["name"]):
        name = entity["name"]
        desc = entity.get("description")

        if desc:
            lines.append(f'"""{desc}"""')
        lines.append(f"type {name} {{")

        fields = entity.get("fields") or []
        for field in fields:
            fname = field["name"]
            fdesc = field.get("description", "")
            ftype = resolve_type(field["type"])

            # 获取基础类型名（去掉 !, [] 等）
            base_type = ftype.replace("!", "").replace(
                "[", "").replace("]", "")

            if fdesc:
                lines.append(f"  # {fdesc}")
            lines.append(f"  {fname}: {ftype}")

            # 展开子字段
            if base_type in entity_names and base_type in type_map:
                sub_entity = type_map[base_type]
                sub_fields = sub_entity.get("fields") or []
                for sf in sub_fields:
                    sf_name = sf["name"]
                    sf_type = resolve_type(sf["type"])
                    sf_base = sf_type.replace("!", "").replace(
                        "[", "").replace("]", "")
                    # 只展开一层，不再递归
                    if sf_base not in entity_names:
                        lines.append(f"    .{sf_name}: {sf_type}")

        lines.append("}")

        # 输出对应的 orderBy
        if name in orderby_enums:
            enum = orderby_enums[name]
            values = [ev["name"] for ev in (enum.get("enumValues") or [])]
            lines.append(f"  # orderBy: {', '.join(values)}")

        lines.append("")

    # 其他 Enums
    important_enums = {"OrderDirection", "Aggregation_interval"}
    lines.append("# === ENUMS ===\n")
    for enum in sorted(enums, key=lambda x: x["name"]):
        if enum["name"] in important_enums:
            lines.append(f"enum {enum['name']} {{")
            for ev in enum.get("enumValues") or []:
                lines.append(f"  {ev['name']}")
            lines.append("}\n")

    output = "\n".join(lines)

    if output_file:
        with open(output_file, "w") as f:
            f.write(output)
        print(f"写入: {output_file}")
    else:
        print(output)


def pull_and_convert(name, subgraph_id, deployment_id):
    """拉取并转换单个 subgraph"""
    print(f"拉取: {name} ({subgraph_id})")
    data = fetch_schema(subgraph_id)
    output_file = SCRIPT_DIR / (name + ".graphql")
    convert_schema(data, output_file)


def pull_all():
    """拉取并转换所有 subgraphs"""
    for name, subgraph_id, deployment_id in SUBGRAPHS:
        pull_and_convert(name, subgraph_id, deployment_id)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        # 单个转换 (本地文件)
        input_name = sys.argv[1]
        input_file = SCRIPT_DIR / input_name
        with open(input_file, "r") as f:
            data = json.load(f)
        output_file = SCRIPT_DIR / (input_name + ".graphql")
        convert_schema(data, output_file)
    else:
        # 拉取全部
        pull_all()
