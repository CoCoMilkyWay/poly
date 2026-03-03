#!/usr/bin/env python3
"""Polymarket 链下查询: 市场 / 用户

GET /markets  参数: clob_token_ids, condition_ids, question_ids, ...

NegRisk 与否:
1. 合约地址不同:negRisk 走 NegRisk CTF Exchange,普通走 CTF Exchange,地址不一样
2. Token 是 wrapped 的:negRisk 的 clobTokenId 是经过 adapter 包装的,不是原始 CTF token
3. Redeem 方式不同:negRisk 赢家 redeem 要走 adapter 解包,普通的直接 redeem
4. Resolution 联动:negRisk 一个 Yes 了其他自动 No;普通的各自独立 resolve
"""

import json
import urllib.request

GAMMA = "https://gamma-api.polymarket.com"


def fetch_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def _parse_json_field(m, key):
    v = m.get(key, '[]')
    return json.loads(v) if isinstance(v, str) else (v or [])


def _print_market(m, highlight_token=None):
    tokens = _parse_json_field(m, 'clobTokenIds')
    outcomes = _parse_json_field(m, 'outcomes')
    prices = _parse_json_field(m, 'outcomePrices')
    events = m.get('events', [])
    slug = events[0]['slug'] if events else m.get('slug', '?')

    print(m)
    event_title = events[0].get('title', '?') if events else '?'
    print(f"  event:       {event_title}")
    print(f"  question:    {m.get('question', '?')}")
    print(f"  url:         https://polymarket.com/event/{slug}")
    print(f"  description: {m.get('description', '?')}")
    print(f"  startDate:   {m.get('startDateIso', '?')}")
    print(f"  endDate:     {m.get('endDateIso', '?')}")
    print(f"  closed:      {m.get('closed', '?')}  (closedTime: {m.get('closedTime', '?')})")
    print(f"  resolution:  {m.get('umaResolutionStatus', '?')}  (source: {m.get('resolutionSource', '?')})")
    print(f"  result:      {', '.join(f'{o}={p}' for o, p in zip(outcomes, prices))}")
    print(f"  volume:      ${m.get('volumeNum', 0):,.0f}")
    print(f"  negRisk:     {m.get('negRisk', False)}")
    print(f"  conditionId: {m.get('conditionId', '?')}")
    print(f"  questionID:  {m.get('questionID', '?')}")
    tags = [t.get('label', '?') for t in m.get('tags', [])]
    if tags:
        print(f"  tags:        {', '.join(tags)}")
    for outcome, tid in zip(outcomes, tokens):
        marker = " ←" if tid == highlight_token else ""
        print(f"    {outcome:12s}  {tid}{marker}")
    print()


def lookup_by_token(token_id):
    data = fetch_json(
        f"{GAMMA}/markets?clob_token_ids={token_id}&include_tag=true")
    assert data and len(data) == 1, f"找不到市场: clob_token_ids={token_id}"
    _print_market(data[0], highlight_token=token_id)


def lookup_by_condition(condition_id):
    data = fetch_json(
        f"{GAMMA}/markets?condition_ids={condition_id}&include_tag=true")
    assert data, f"找不到市场: condition_ids={condition_id}"
    for m in data:
        _print_market(m)


def lookup_by_question(question_id):
    data = fetch_json(
        f"{GAMMA}/markets?question_ids={question_id}&include_tag=true")
    assert data, f"找不到市场: question_ids={question_id}"
    for m in data:
        _print_market(m)


def lookup_by_slug(slug):
    data = fetch_json(f"{GAMMA}/events?slug={slug}&include_tag=true")
    assert data and len(data) == 1, f"找不到事件: slug={slug}"
    event = data[0]
    markets = event.get('markets', [])
    assert markets, f"事件无市场: slug={slug}"
    for m in markets:
        _print_market(m)


def lookup_user(address):
    data = fetch_json(f"{GAMMA}/public-profile?address={address}")
    assert data, f"未注册 / 无 profile: {address}"
    print(f"  name:        {data.get('name', '?')}")
    print(f"  pseudonym:   {data.get('pseudonym', '?')}")
    print(f"  bio:         {data.get('bio', '?')}")
    print(f"  proxyWallet: {data.get('proxyWallet', '?')}")
    print(f"  url:         https://polymarket.com/profile/{address}")


if __name__ == "__main__":
    # ---- 改这里 ----
    # lookup_by_slug("will-the-iranian-regime-fall-by-june-30")
    # lookup_by_token("46463242715089624889628251669699043467690195456787426146949493058059414797372")
    lookup_by_condition("0xb2eecb8d14e871c5b82a3b037fc5f8b703c218e41aa578c8e870244585b9db78")
    # lookup_by_question("0x94a72cbcb99d9bd213077561715cd2c240fd274d366397dedb9bbb0f50b68a04")
    # lookup_user("0x1514989043233940203f7457cae4542b1bf42624")
