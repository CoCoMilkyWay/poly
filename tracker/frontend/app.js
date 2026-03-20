const backendParam = new URLSearchParams(window.location.search).get("backend");
const savedBackend = window.localStorage.getItem("tracker.backend");
const BACKEND_BASE =
  backendParam || savedBackend || `${window.location.protocol}//${window.location.hostname}:8871`;
window.localStorage.setItem("tracker.backend", BACKEND_BASE);

const EVENT_TYPE_LABELS = [
  "order_buy",
  "order_sell",
  "split",
  "merge",
  "redeem",
  "convert",
];

const state = {
  payload: null,
  meta: null,
  selectedUser: "",
  selectedSnapshot: "",
  mode: "aggregate-current",
  historyCache: new Map(),
  progress: null,
};

const usersListEl = document.getElementById("users-list");
const tradeListEl = document.getElementById("trade-list");
const holdingsListEl = document.getElementById("holdings-list");
const holdingsSummaryEl = document.getElementById("holdings-summary");
const queryCountsEl = document.getElementById("query-counts");
const modeSelectEl = document.getElementById("mode-select");
const userSelectEl = document.getElementById("user-select");
const snapshotSelectEl = document.getElementById("snapshot-select");
const backendUrlEl = document.getElementById("backend-url");
const resyncButtonEl = document.getElementById("resync-button");

backendUrlEl.textContent = BACKEND_BASE;

function fmtNumber(value) {
  return new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 2 }).format(Number(value || 0));
}

function shortAddr(value) {
  if (!value || value.length < 12) {
    return value || "";
  }
  return `${value.slice(0, 8)}...${value.slice(-6)}`;
}

function setEmpty(container, text) {
  container.innerHTML = `<div class="empty">${text}</div>`;
}

async function fetchJson(url, options) {
  const response = await fetch(url, options);
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return response.json();
}

function tokenMeta(tokenId) {
  // tokens 现在只存 token_id → condition_id 映射
  const condId = state.meta?.tokens?.[tokenId];
  return condId ? { cond: condId } : null;
}

function conditionMeta(conditionId) {
  return conditionId ? state.meta?.conditions?.[conditionId] || null : null;
}

function getTokenIdx(conditionId, tokenId) {
  const cond = conditionMeta(conditionId);
  if (!cond?.tids) return null;
  const idx = cond.tids.indexOf(tokenId);
  return idx >= 0 ? idx : null;
}

function getTokenPrice(conditionId, tokenId) {
  const cond = conditionMeta(conditionId);
  const idx = getTokenIdx(conditionId, tokenId);
  if (idx === null || !cond?.prices) return null;
  const price = cond.prices[idx];
  return typeof price === "number" && price >= 0 ? price : null;
}

function stableRowsFromBalances(stableBalances) {
  const rows = [];
  const specs = [
    ["stable:usdc", "USDC", 1, stableBalances?.usdc_raw],
    ["stable:usdc_e", "USDC.e", 2, stableBalances?.usdc_e_raw],
    ["stable:usdt", "USDT", 3, stableBalances?.usdt_raw],
    ["stable:wrapped", "WrappedUSDCe", 4, stableBalances?.wrapped_raw],
  ];
  for (const [tokenId, label, collateral, amountRaw] of specs) {
    const amount = Number(amountRaw || 0);
    if (!amount) {
      continue;
    }
    rows.push({
      asset_type: "stable",
      token_id: tokenId,
      label,
      collateral,
      amount_raw: String(amountRaw),
      price: 1_000_000,
      value_usd: amount / 1e6,
      q: label,
      desc: "",
      outcomes: [],
    });
  }
  return rows;
}

function buildSnapshotRows(snapshot) {
  if (!snapshot) {
    return [];
  }

  const rows = [];
  for (const position of snapshot.positions || []) {
    const metaToken = tokenMeta(position.token_id);
    const condId = metaToken?.cond || null;
    const metaCondition = conditionMeta(condId);
    const tokenIdx = getTokenIdx(condId, position.token_id);
    const price = getTokenPrice(condId, position.token_id);
    const valueUsd = price === null
      ? 0
      : (Number(position.amount_raw || 0) / 1e6) * (price / 1e6);
    rows.push({
      asset_type: "token",
      token_id: position.token_id,
      condition_id: condId,
      token_idx: tokenIdx,
      collateral: metaCondition?.coll ?? null,
      amount_raw: position.amount_raw,
      price,
      value_usd: valueUsd,
      q: metaCondition?.q || position.token_id,
      desc: metaCondition?.desc || "",
      outcomes: metaCondition?.outcomes || [],
      outcome_text: tokenIdx === null ? "" : (metaCondition?.outcomes?.[tokenIdx] || ""),
    });
  }

  rows.push(...stableRowsFromBalances(snapshot.stable_balances));
  rows.sort((a, b) => {
    if ((b.value_usd || 0) !== (a.value_usd || 0)) {
      return (b.value_usd || 0) - (a.value_usd || 0);
    }
    return String(a.token_id).localeCompare(String(b.token_id));
  });

  const totalValue = rows.reduce((sum, row) => sum + Number(row.value_usd || 0), 0);
  for (const row of rows) {
    row.weight = totalValue > 0 ? Number(row.value_usd || 0) / totalValue : 0;
  }
  return rows;
}

function enrichHistoryEvent(user, blockNumber, event) {
  const metaCondition = conditionMeta(event.condition_id);
  const tokenIdx = typeof event.token_idx === "number" ? event.token_idx : null;
  return {
    ...event,
    user,
    block_number: Number(blockNumber),
    type_name: EVENT_TYPE_LABELS[event.type] || "unknown",
    q: metaCondition?.q || event.condition_id || "",
    desc: metaCondition?.desc || "",
    outcomes: metaCondition?.outcomes || [],
    outcome_text: tokenIdx === null ? "" : (metaCondition?.outcomes?.[tokenIdx] || ""),
    collateral_label: ({
      1: "USDC",
      2: "USDC.e",
      3: "USDT",
      4: "WrappedUSDCe",
    })[event.collateral] || "",
  };
}

function historyRowsForSelectedUser() {
  if (!state.selectedUser || !state.historyCache.has(state.selectedUser)) {
    return [];
  }
  const history = state.historyCache.get(state.selectedUser);
  const rows = [];
  const blocks = Object.keys(history?.events || {}).sort().reverse();
  for (const block of blocks) {
    for (const event of history.events[block] || []) {
      rows.push(enrichHistoryEvent(state.selectedUser, block, event));
    }
    if (rows.length >= 120) {
      break;
    }
  }
  return rows;
}

function renderUserList() {
  const payload = state.payload;
  if (!payload?.users?.length) {
    setEmpty(usersListEl, "暂无用户数据");
    return;
  }

  usersListEl.innerHTML = payload.users.map((row) => {
    const activeClass = row.user === state.selectedUser ? "active" : "";
    return `
      <div class="user-row ${activeClass}" data-user="${row.user}">
        <div class="user-row-head">
          <div class="mono">${shortAddr(row.user)}</div>
          <div>${fmtNumber(row.total_value_usd)} USD</div>
        </div>
        <div class="metric-pair">
          <span>snapshot ${row.snapshot_block || 0}</span>
          <span>token ${fmtNumber(row.token_value_usd || 0)}</span>
          <span>stable ${fmtNumber(row.stable_value_usd || 0)}</span>
        </div>
      </div>
    `;
  }).join("");

  usersListEl.querySelectorAll(".user-row").forEach((node) => {
    node.addEventListener("click", async () => {
      state.selectedUser = node.dataset.user || "";
      userSelectEl.value = state.selectedUser;
      await ensureHistoryLoaded(state.selectedUser);
      resetSnapshotSelection();
      renderAll();
    });
  });
}

function renderTradeList() {
  const payload = state.payload;
  if (!payload) {
    setEmpty(tradeListEl, "暂无交易数据");
    return;
  }

  const rows = state.selectedUser ? historyRowsForSelectedUser() : (payload.recent_events || []);
  if (!rows.length) {
    setEmpty(tradeListEl, "暂无交易记录");
    return;
  }

  tradeListEl.innerHTML = rows.slice(0, 120).map((row) => {
    const negative = Number(row.amount || 0) < 0 ? "out" : "";
    const priceText = typeof row.price === "number" ? fmtNumber(row.price / 1e6) : "-";
    const question = row.q || row.condition_id || "-";
    const outcome = row.outcome_text || `idx ${row.token_idx ?? "-"}`;
    return `
      <div class="trade-row">
        <div class="trade-row-head">
          <div>
            <div class="trade-kind ${negative}">${row.type_name || "-"}</div>
            <div class="mono">${shortAddr(row.user || "")}</div>
          </div>
          <div class="mono">blk ${row.block_number || "-"}</div>
        </div>
        <div class="metric-pair">
          <span>${question}</span>
          <span>${outcome}</span>
        </div>
        <div class="metric-pair">
          <span>amt ${row.amount ?? "-"}</span>
          <span>${row.collateral_label || "-"}</span>
        </div>
        <div class="metric-pair">
          <span>${row.condition_id || "-"}</span>
          <span>price ${priceText}</span>
        </div>
      </div>
    `;
  }).join("");
}

function currentHoldingsRows() {
  const payload = state.payload;
  if (!payload) {
    return { rows: [], summary: [] };
  }

  if (state.mode === "aggregate-current") {
    return {
      rows: payload.aggregate || [],
      summary: [
        `min snapshot ${payload.summary?.min_snapshot_block || 0}`,
        `last applied ${payload.summary?.last_applied_block || 0}`,
        `head ${payload.summary?.head_block || 0}`,
      ],
    };
  }

  const userRow = (payload.users || []).find((row) => row.user === state.selectedUser);
  if (!userRow) {
    return { rows: [], summary: [] };
  }

  if (state.mode === "user-current") {
    return {
      rows: userRow.positions || [],
      summary: [
        shortAddr(userRow.user),
        `snapshot ${userRow.snapshot_block || 0}`,
        `total ${fmtNumber(userRow.total_value_usd || 0)} USD`,
      ],
    };
  }

  const history = state.historyCache.get(state.selectedUser);
  const snapshot = history?.snapshots?.[state.selectedSnapshot];
  if (!snapshot) {
    return { rows: [], summary: [] };
  }

  return {
    rows: buildSnapshotRows(snapshot),
    summary: [
      shortAddr(state.selectedUser),
      `snapshot ${snapshot.block_number || 0}`,
      `captured ${snapshot.captured_at_unix_sec || 0}`,
    ],
  };
}

function renderHoldings() {
  const { rows, summary } = currentHoldingsRows();
  holdingsSummaryEl.innerHTML = summary.map((text) => `<div class="summary-pill">${text}</div>`).join("");

  if (!rows.length) {
    setEmpty(holdingsListEl, "暂无持仓数据");
    return;
  }

  holdingsListEl.innerHTML = rows.map((row) => {
    const value = Number(row.value_usd || 0);
    const width = Math.max(0, Math.min(100, Number(row.weight || 0) * 100));
    const stable = row.asset_type === "stable" ? "stable" : "";
    const title = row.asset_type === "stable" ? (row.label || row.token_id || "-") : (row.q || row.token_id || "-");
    const subtitle = row.asset_type === "stable"
      ? (row.label || "")
      : `${row.outcome_text || "-"} | idx ${row.token_idx ?? "-"}`;
    const tooltip = [row.q || "", row.desc || ""].filter(Boolean).join(" | ");
    const priceText = typeof row.price === "number" ? fmtNumber(row.price / 1e6) : "-";
    return `
      <div class="holding-row" title="${tooltip}">
        <div class="holding-head">
          <div>
            <div class="holding-title">${title}</div>
            <div class="holding-subtitle">${subtitle}</div>
          </div>
          <div>${fmtNumber(value)} USD</div>
        </div>
        <div class="bar-track">
          <div class="bar-fill ${stable}" style="width:${width}%"></div>
        </div>
        <div class="holding-metrics">
          <span>${row.token_id || row.label || "-"}</span>
          <span>amt ${row.amount_raw || "-"}</span>
          <span>price ${priceText}</span>
          <span>${fmtNumber(width)}%</span>
        </div>
      </div>
    `;
  }).join("");
}

function renderQueryCounts() {
  const progressData = state.progress || [];
  const headBlock = state.payload?.summary?.head_block || 0;

  let html = `
    <table class="progress-table">
      <thead>
        <tr><th>API</th><th>done/total</th><th>[pend]</th></tr>
      </thead>
      <tbody>
  `;
  for (const api of progressData) {
    html += `
      <tr>
        <td>${api.name}</td>
        <td>${fmtNumber(api.done)}/${fmtNumber(api.total)}</td>
        <td>[${api.pending}]</td>
      </tr>
    `;
  }
  html += `
      </tbody>
    </table>
    <div class="head-block">head: ${fmtNumber(headBlock)}</div>
  `;
  queryCountsEl.innerHTML = html;
}

function renderUserOptions() {
  const users = state.payload?.users || [];
  userSelectEl.innerHTML = users.map((row) => `
    <option value="${row.user}">${shortAddr(row.user)}</option>
  `).join("");

  if (!state.selectedUser && users.length) {
    state.selectedUser = users[0].user;
  }
  if (state.selectedUser && !users.some((row) => row.user === state.selectedUser)) {
    state.selectedUser = users[0]?.user || "";
  }
  userSelectEl.value = state.selectedUser || "";
}

function resetSnapshotSelection() {
  const history = state.historyCache.get(state.selectedUser);
  const snapshotKeys = Object.keys(history?.snapshots || {}).sort().reverse();
  snapshotSelectEl.innerHTML = snapshotKeys.map((key) => {
    const snapshot = history.snapshots[key];
    return `<option value="${key}">${snapshot.block_number}</option>`;
  }).join("");

  if (snapshotKeys.length) {
    if (!snapshotKeys.includes(state.selectedSnapshot)) {
      state.selectedSnapshot = snapshotKeys[0];
    }
  } else {
    state.selectedSnapshot = "";
  }
  snapshotSelectEl.value = state.selectedSnapshot;
}

async function ensureHistoryLoaded(user) {
  if (!user || state.historyCache.has(user)) {
    return;
  }
  const payload = await fetchJson(`${BACKEND_BASE}/api/history?user=${encodeURIComponent(user)}`);
  state.historyCache.set(user, payload);
}

async function refreshState() {
  const [payload, meta] = await Promise.all([
    fetchJson(`${BACKEND_BASE}/api/state`),
    fetchJson(`${BACKEND_BASE}/api/meta`),
  ]);
  state.payload = payload;
  state.meta = meta;
  renderUserOptions();
  await ensureHistoryLoaded(state.selectedUser);
  resetSnapshotSelection();
  renderAll();
}

function renderAll() {
  renderUserList();
  renderTradeList();
  renderHoldings();
  renderQueryCounts();
}

modeSelectEl.addEventListener("change", () => {
  state.mode = modeSelectEl.value;
  renderAll();
});

userSelectEl.addEventListener("change", async () => {
  state.selectedUser = userSelectEl.value;
  await ensureHistoryLoaded(state.selectedUser);
  resetSnapshotSelection();
  renderAll();
});

snapshotSelectEl.addEventListener("change", () => {
  state.selectedSnapshot = snapshotSelectEl.value;
  renderAll();
});

resyncButtonEl.addEventListener("click", async () => {
  resyncButtonEl.disabled = true;
  try {
    await fetchJson(`${BACKEND_BASE}/api/resync`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}",
    });
    state.historyCache.clear();
    await refreshState();
  } finally {
    resyncButtonEl.disabled = false;
  }
});

function connectSSE() {
  const es = new EventSource(`${BACKEND_BASE}/api/events`);
  es.onmessage = async () => {
    try {
      await refreshState();
    } catch (error) {
      console.error(error);
    }
  };
  es.onerror = () => {
    es.close();
    setTimeout(connectSSE, 2000);
  };
}

function connectProgressSSE() {
  const es = new EventSource(`${BACKEND_BASE}/api/progress/stream`);
  es.onmessage = (event) => {
    try {
      state.progress = JSON.parse(event.data);
      renderQueryCounts();
    } catch (error) {
      console.error("Progress SSE error:", error);
    }
  };
  es.onerror = () => {
    es.close();
    setTimeout(connectProgressSSE, 2000);
  };
}

async function boot() {
  await refreshState();
  connectSSE();
  connectProgressSSE();
}

boot().catch((error) => {
  console.error(error);
});
