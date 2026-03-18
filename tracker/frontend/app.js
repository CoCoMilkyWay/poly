const backendParam = new URLSearchParams(window.location.search).get("backend");
const savedBackend = window.localStorage.getItem("tracker.backend");
const BACKEND_BASE = backendParam || savedBackend || `${window.location.protocol}//${window.location.hostname}:8871`;
window.localStorage.setItem("tracker.backend", BACKEND_BASE);
const state = {
  payload: null,
  selectedUser: "",
  selectedSnapshot: "",
  mode: "aggregate-current",
  historyCache: new Map(),
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
  return new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 2 }).format(value);
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

function renderUserList() {
  const payload = state.payload;
  if (!payload || !payload.users || payload.users.length === 0) {
    setEmpty(usersListEl, "暂无用户数据");
    return;
  }

  usersListEl.innerHTML = payload.users.map((row) => {
    const activeClass = row.user === state.selectedUser ? "active" : "";
    return `
      <div class="user-row ${activeClass}" data-user="${row.user}">
        <div class="user-row-head">
          <div class="mono">${shortAddr(row.user)}</div>
          <div>${fmtNumber(row.total_value_usd || 0)} USD</div>
        </div>
        <div class="metric-pair">
          <span>token ${fmtNumber(row.token_value_usd || 0)}</span>
          <span>stable ${fmtNumber(row.stable_value_usd || 0)}</span>
          <span>${fmtNumber((row.token_value_usd_ratio || 0) * 100)}%</span>
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

  let rows = payload.recent_events || [];
  if (state.selectedUser && state.historyCache.has(state.selectedUser)) {
    const history = state.historyCache.get(state.selectedUser);
    const eventsByBlock = history?.events || {};
    const blocks = Object.keys(eventsByBlock).sort().reverse();
    rows = [];
    for (const block of blocks) {
      for (const event of eventsByBlock[block]) {
        rows.push(event);
      }
      if (rows.length >= 100) {
        break;
      }
    }
  }

  if (!rows.length) {
    setEmpty(tradeListEl, "暂无交易记录");
    return;
  }

  tradeListEl.innerHTML = rows.slice(0, 120).map((row) => {
    const outClass = row.direction === "out" ? "out" : "";
    const collateral = row.collateral_amount_raw
      ? ` | coll ${fmtNumber(Number(row.collateral_amount_raw) / 1e6)}`
      : "";
    return `
      <div class="trade-row">
        <div class="trade-row-head">
          <div>
            <div class="trade-kind ${outClass}">${row.kind || "-"}</div>
            <div class="mono">${shortAddr(row.user || "")}</div>
          </div>
          <div class="mono">blk ${row.block_number}</div>
        </div>
        <div class="metric-pair">
          <span>${row.direction || "-"}</span>
          <span>${row.token_id || "-"}</span>
        </div>
        <div class="metric-pair">
          <span>amt ${row.amount_raw || "-"}</span>
          <span>${shortAddr(row.counterparty || "")}</span>
        </div>
        <div class="metric-pair">
          <span class="mono">${shortAddr(row.tx_hash || "")}</span>
          <span>${row.price || ""}${collateral}</span>
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
        `snapshot ${payload.summary?.snapshot_block || 0}`,
        `last applied ${payload.summary?.last_applied_block || 0}`,
        `tokens ${payload.summary?.token_count || 0}`,
      ],
    };
  }

  const userRow = (payload.users || []).find((item) => item.user === state.selectedUser);
  if (!userRow) {
    return { rows: [], summary: [] };
  }

  if (state.mode === "user-current") {
    return {
      rows: userRow.positions || [],
      summary: [
        shortAddr(userRow.user),
        `token ${fmtNumber(userRow.token_value_usd || 0)} USD`,
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
    rows: snapshot.positions || [],
    summary: [
      shortAddr(state.selectedUser),
      `snapshot ${snapshot.block_number}`,
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

  const totalValue = rows.reduce((sum, row) => sum + Number(row.value_usd || row.total_value_usd || 0), 0);
  holdingsListEl.innerHTML = rows.map((row) => {
    const value = Number(row.value_usd || row.total_value_usd || 0);
    const weight = totalValue > 0 ? value / totalValue : Number(row.weight || 0);
    const width = Math.max(0, Math.min(100, weight * 100));
    const stable = row.asset_type === "stable" ? "stable" : "";
    const title = row.asset_type === "stable"
      ? (row.label || row.token_id || "-")
      : (row.market_question || row.token_id || "-");
    const subtitle = row.asset_type === "stable"
      ? `${row.label || ""}`
      : `${row.outcome_text || "-"} | idx ${row.outcome_index ?? "-"}`;
    const tooltip = [row.market_question || "", row.market_description || ""].filter(Boolean).join(" | ");
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
          <span>amt ${row.amount_raw || row.total_amount_raw || "-"}</span>
          <span>price ${row.price || row.weight || "-"}</span>
          <span>${fmtNumber(width)}%</span>
        </div>
      </div>
    `;
  }).join("");
}

function renderQueryCounts() {
  const queryCounts = state.payload?.summary?.query_counts || {};
  const rows = [
    ["rpc_http", queryCounts.rpc_http_calls || 0],
    ["rpc_ws_msg", queryCounts.rpc_ws_messages || 0],
    ["rpc_ws_sub", queryCounts.rpc_ws_subscriptions || 0],
    ["subgraph", queryCounts.subgraph_queries || 0],
    ["gamma", queryCounts.gamma_queries || 0],
    ["head", state.payload?.summary?.head_block || 0],
  ];
  queryCountsEl.innerHTML = rows.map(([label, value]) => `
    <div class="query-card">
      <div class="query-label">${label}</div>
      <div class="query-value">${fmtNumber(value)}</div>
    </div>
  `).join("");
}

function renderUserOptions() {
  const users = state.payload?.users || [];
  userSelectEl.innerHTML = users.map((row) => `
    <option value="${row.user}">${shortAddr(row.user)}</option>
  `).join("");

  if (!state.selectedUser && users.length) {
    state.selectedUser = users[0].user;
  }
  userSelectEl.value = state.selectedUser || "";
}

function resetSnapshotSelection() {
  const history = state.historyCache.get(state.selectedUser);
  const blocks = Object.keys(history?.snapshots || {}).sort().reverse();
  snapshotSelectEl.innerHTML = blocks.map((key) => {
    const row = history.snapshots[key];
    return `<option value="${key}">${row.block_number}</option>`;
  }).join("");
  if (blocks.length) {
    if (!blocks.includes(state.selectedSnapshot)) {
      state.selectedSnapshot = blocks[0];
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
  const payload = await fetchJson(`${BACKEND_BASE}/api/state`);
  state.payload = payload;
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

async function boot() {
  await refreshState();
  setInterval(async () => {
    try {
      await refreshState();
    } catch (error) {
      console.error(error);
    }
  }, 2000);
}

boot().catch((error) => {
  console.error(error);
});
