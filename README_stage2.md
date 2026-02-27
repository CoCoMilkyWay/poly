### 原理 1: Transfer 是持仓变化的唯一来源 ✅
ERC1155 规范强制要求：**任何 `_balances` 修改都必须 emit Transfer**
```
ERC1155.sol 中所有修改余额的地方：
  safeTransferFrom   → emit TransferSingle
  safeBatchTransferFrom → emit TransferBatch
  _mint / _batchMint → emit TransferSingle / TransferBatch (from=0x0)
  _burn / _batchBurn → emit TransferSingle / TransferBatch (to=0x0)
```

### 原理 2: 语义事件和 Transfer 在同一个 tx ✅
协议操作是原子的，但**同一语义可能包含多条 Transfer 组合**，不能只看单一方向：
| 操作                     | Transfer 真实形态                                                                                                            | 语义事件                 | 来源     |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------- | ------------------------ | -------- |
| CTF.splitPosition        | 可能是 `burn + mint` 组合：`stakeholder->0x0`（当 `parentCollectionId != 0` 或子集拆分）+ `0x0->stakeholder`                 | PositionSplit            | 同一函数 |
| CTF.mergePositions       | `stakeholder` 按 partition 批量 burn；然后要么发 collateral（ERC20，不在 ERC1155 transfer 表），要么 mint 回 parent position | PositionsMerge           | 同一函数 |
| CTF.redeemPositions      | 对 indexSets 对应持仓逐个 burn（可能多条/可能部分为0）；再发 payout（ERC20 或 parent position mint）                         | PayoutRedemption         | 同一函数 |
| Exchange.fillOrder       | `safeTransferFrom` × 2（token 对手盘互换）                                                                                   | OrderFilled              | 同一函数 |
| FPMM.buy/sell            | 交易转账 + 内部 split/merge 触发的 mint/burn                                                                                 | FPMMBuy / FPMMSell       | 同一函数 |
| FPMM.add/removeFunding   | 资金进出 + 条件仓位转移（remove 不是 burn）                                                                                  | FPMMFundingAdded/Removed | 同一函数 |
| NegRisk.convertPositions | Adapter 内部 split 产生仓位，再把 NO 批量转到 `NO_TOKEN_BURN_ADDRESS`，并把 YES/费用转给用户或 vault                         | PositionsConverted       | 同一函数 |
**边界情况**：用户直接 `safeTransferFrom` 只有 Transfer、无语义事件 → TransferIn/Out


```
Stage2Sync (timer驱动, boost::asio)
  ├─ 检查 stage1_last_block vs stage2_cursor
  ├─ behind > 0 → build_chunk(cursor + chunk_size)
  │   ├─ Phase 1: phase1_update_mappings
  │   ├─ Phase 2: phase2_build_semantic_index
  │   ├─ Phase 3: phase3_process_transfers
  │   └─ commit_chunk (单事务: rb_* + user_event + cursor)
  └─ behind == 0 → sleep(base_interval)
```
**崩溃恢复**: 从 stage2_cursor 断点重做，语义索引不持久化


问题树 (`s2-cond-tree`)
└─ 问题  // desc: condition分类总览; scene: CTF问题来源分层; 对应: cond_tree.total
   ├─ Polymarket  // desc: Polymarket子树; scene: 已确认归属Polymarket; 对应: cond_tree.polymarket.total
   │  ├─ TokenReg  // desc: TokenRegistered子类; scene: 在Exchange登记过; 对应: cond_tree.polymarket.token_reg.total
   │  │  ├─ AMM  // desc: poly & treg & amm; scene: 注册后又创建了AMM池; 对应: cond_tree.polymarket.token_reg.amm
   │  │  ├─ NegRisk  // desc: poly & treg & nr; scene: 多选题市场(谁当选总统); 对应: cond_tree.polymarket.token_reg.negrisk
   │  │  ├─ OB  // desc: poly & treg & ob; scene: 无AMM池,只有挂单交易; 对应: cond_tree.polymarket.token_reg.orderbook
   │  │  └─ Other(预期=0)  // desc: poly & treg & other0; scene: Polymarket-TokenReg未覆盖项; 对应: cond_tree.polymarket.token_reg.other
   │  └─ FPMM  // desc: poly & fcreate & !treg; scene: 只创建AMM池,未注册Exchange; 对应: cond_tree.polymarket.fpmm_poly
   └─ 其他  // desc: 非Polymarket子树; scene: 无法确认属于Polymarket; 对应: cond_tree.other.total
      ├─ Prep  // desc: !poly & cprep; scene: 早期或其他协议(如Omen); 对应: cond_tree.other.prep
      ├─ FPMM  // desc: !poly & fcreate; scene: 非Polymarket来源的FPMMCreation; 对应: cond_tree.other.fpmm_other
      ├─ Split  // desc: !poly & split_event; scene: 仅由Split事件推断condition; 对应: cond_tree.other.split
      ├─ Merge  // desc: !poly & merge_event; scene: 仅由Merge事件推断condition; 对应: cond_tree.other.merge
      └─ Redemption  // desc: !poly & redemption_event; scene: 仅由Redemption事件推断condition; 对应: cond_tree.other.redemption

代币树 (`s2-token-tree`)
└─ 代币  // desc: token分类总览; scene: ERC1155 token_id来源分层; 对应: token_tree.total
   ├─ Polymarket  // desc: Polymarket子树; scene: 已确认归属Polymarket; 对应: token_tree.polymarket.total
   │  ├─ TokenReg  // desc: TokenRegistered子类; scene: Exchange事件直接给token; 对应: token_tree.polymarket.token_reg.total
   │  │  ├─ AMM  // desc: poly & treg & amm; scene: 注册后又创建了AMM池; 对应: token_tree.polymarket.token_reg.amm
   │  │  ├─ NegRisk  // desc: poly & treg & nr; scene: 多选题市场代币; 对应: token_tree.polymarket.token_reg.negrisk
   │  │  ├─ OB  // desc: poly & treg & ob; scene: 无AMM池,只有挂单; 对应: token_tree.polymarket.token_reg.orderbook
   │  │  └─ Other(预期=0)  // desc: poly & treg & other0; scene: Polymarket-TokenReg未覆盖项; 对应: token_tree.polymarket.token_reg.other
   │  └─ FPMM  // desc: poly & fcreate & !treg; scene: 只创建AMM池,未注册Exchange; 对应: token_tree.polymarket.fpmm_poly
   │     └─ (动态) by_collateral_fpmm
   └─ 其他  // desc: 非Polymarket子树; scene: 无法确认属于Polymarket; 对应: token_tree.other.total
      ├─ FPMM  // desc: !poly & fcreate; scene: 非Polymarket来源的FPMMCreation; 对应: token_tree.other.fpmm_other
      ├─ Split  // desc: !poly & split_event; scene: 仅由Split事件推断token; 对应: token_tree.other.split
      ├─ Merge  // desc: !poly & merge_event; scene: 仅由Merge事件推断token; 对应: token_tree.other.merge
      ├─ Redemption  // desc: !poly & redemption_event; scene: 仅由Redemption事件推断token; 对应: token_tree.other.redemption
      └─ Transfer  // desc: !poly & xfer_inf; scene: 未知token,无condition信息; 对应: token_tree.other.transfer_inferred
         └─ (动态) by_collateral_transfer_inferred

Transfer树 (`s2-xfer-tree`)
└─ Transfer  // desc: TransferSingle/Batch; scene: ERC1155代币转移事件;
  ├─ 用户操作  // desc: 用户事件汇总; scene: 影响用户资产; 对应: user_events
  │  ├─ 铸造  // desc: split分类; scene: 用户拆仓; 对应: Split
  │  │  ├─ Poly  // desc: Polymarket子类; scene: 已知条件; 对应: SplitNormal|SplitNegRisk
  │  │  │  ├─ CTF  // desc: *:0->用户 & amt>0 & m(split) & holder=to & known; scene: 用户直连CTF split收到仓位; 对应: SplitNormal
  │  │  │  └─ NegRisk  // desc: Adapter:Adapter->用户 & amt>0 & m(split) & holder=Adapter & known; scene: Adapter代split后给用户; 对应: SplitNegRisk
  │  │  ├─ NonPoly  // desc: *:*->用户 & amt>0 & m(split) & !known; scene: 非Polymarket split; 对应: SplitNonPoly
  │  │  │  └─ (动态) by_collateral_split
  │  ├─ 合并  // desc: merge分类; scene: 用户合仓; 对应: Merge
  │  │  ├─ Poly  // desc: Polymarket子类; scene: 已知条件; 对应: MergeNormal|MergeNegRisk
  │  │  │  ├─ CTF  // desc: *:用户->0 & amt>0 & m(merge) & holder=from & known; scene: 用户直连CTF merge销毁仓位; 对应: MergeNormal
  │  │  │  └─ NegRisk  // desc: Adapter:用户->Adapter & amt>0 & m(merge) & holder=Adapter & known; scene: 用户给Adapter代理merge; 对应: MergeNegRisk
  │  │  ├─ NonPoly  // desc: *:用户->* & amt>0 & m(merge) & !known; scene: 非Polymarket merge; 对应: MergeNonPoly
  │  │  │  └─ (动态) by_collateral_merge
  │  ├─ 赎回  // desc: redemption分类; scene: 用户结算赎回; 对应: Redemption
  │  │  ├─ Poly  // desc: Polymarket子类; scene: 已知条件; 对应: Redemption
  │  │  ├─ NonPoly  // desc: *:用户->0 & amt>0 & m(redeem) & !known; scene: 非Polymarket结算赎回; 对应: RedemptionNonPoly
  │  │  │  └─ (动态) by_collateral_redemption
  │  ├─ 转换  // desc: convert分类; scene: NO转YES; 对应: Convert
  │  ├─ 订单  // desc: order分类; scene: 订单簿撮合; 对应: OrderBuy|OrderSell
  │  │  ├─ 买入  // desc: CTF|NEG_RISK:taker->用户 & amt>0 & m(order) & maker_side=buy; scene: 用户买入条件token; 对应: OrderBuy
  │  │  ├─ 卖出  // desc: CTF|NEG_RISK:用户->taker & amt>0 & m(order) & maker_side=sell; scene: 用户卖出条件token; 对应: OrderSell
  │  ├─ 池交易  // desc: FPMM交易分类(通过 m(trade) 配对后从FPMM取 collateral); scene: 用户与池交易; 对应: FPMMBuy|FPMMSell
  │  │  ├─ 买入  // desc: FPMM:FPMM->用户 & amt>0 & m(trade) & side=buy; scene: 用户从池子买入token; 对应: FPMMBuy
  │  │  ├─ 卖出  // desc: FPMM:用户->FPMM & amt>0 & m(trade) & side=sell; scene: 用户向池子卖出token; 对应: FPMMSell
  │  │  └─ (动态) by_collateral_fpmm_trade
  │  ├─ 池流动性  // desc: LP分类(通过 m(lp_add|lp_refund) 配对后从FPMM取 collateral); scene: 用户加减流动性; 对应: FPMMLPAdd|FPMMLPRemove|FPMMLPReturn
  │  │  ├─ 添加  // desc: *:0->FPMM & amt>0 & m(lp_add); scene: LP加池触发铸仓位到FPMM; 对应: FPMMLPAdd
  │  │  ├─ 移除  // desc: FPMM:FPMM->用户 & amt>0 & !m(trade) & !m(lp_refund); scene: LP移除流动性取回仓位; 对应: FPMMLPRemove
  │  │  ├─ 返还  // desc: FPMM:FPMM->用户 & amt>0 & m(lp_refund); scene: LP加池不对称部分退回; 对应: FPMMLPReturn
  │  │  └─ (动态) by_collateral_fpmm_lp
  │  └─ 转账  // desc: 普通转账分类; scene: 用户地址参与; 对应: TransferIn*|TransferOut*
  │     ├─ 转入  // desc: 转入分类; scene: 用户收到token; 对应: TransferIn*
  │     │  ├─ Poly  // desc: 已知token子类; scene: Polymarket token转入; 对应: TransferInNegRisk|TransferInOther
  │     │  │  ├─ Adapter  // desc: Adapter:Adapter->用户 & amt>0 & known & !m(split); scene: Adapter直接转token给用户; 对应: TransferInNegRisk
  │     │  │  └─ 其他  // desc: *:其他->用户 & amt>0 & known & !m(split) & op!=Adapter; scene: 用户间转账或协议空投; 对应: TransferInOther
  │     │  └─ NonPoly  // desc: *:*->用户 & amt>0 & !known; scene: 非Polymarket token转入; 对应: TransferInNonPoly
  │     ├─ 转出  // desc: 转出分类; scene: 用户发出token; 对应: TransferOut*
  │     │  ├─ Poly  // desc: 已知token子类; scene: Polymarket token转出; 对应: TransferOutNegRisk|TransferOutOther
  │     │  │  ├─ Adapter  // desc: Adapter:用户->Adapter & amt>0 & known & !m(merge); scene: 用户直接把token转给Adapter; 对应: TransferOutNegRisk
  │     │  │  └─ 其他  // desc: *:用户->其他 & amt>0 & known & !m(merge) & op!=Adapter; scene: 用户转账给他人或外部协议; 对应: TransferOutOther
  │     │  └─ NonPoly  // desc: *:用户->* & amt>0 & !known; scene: 非Polymarket token转出; 对应: TransferOutNonPoly
  └─ 内部操作  // desc: 协议内部分类; scene: 不影响用户持仓; 对应: internal
      ├─ 内铸  // desc: InternalMint子类; scene: 协议接收mint; 对应: InternalMint*
      │  ├─ Adapter  // desc: *:0->Adapter & amt>0; scene: Adapter代理split的mint; 对应: InternalMintNegRisk
      │  └─ FPMM  // desc: *:0->FPMM & amt>0 & in_fpmm_map(to) & !m(lp_add); scene: AMM买入时内部split; 对应: InternalMintFPMM
      ├─ 内燃  // desc: InternalBurn子类; scene: 协议发起burn; 对应: InternalBurn*
      │  ├─ Adapter  // desc: *:Adapter->0 & amt>0; scene: Adapter代理merge的burn; 对应: InternalBurnNegRisk
      │  ├─ FPMM  // desc: *:FPMM->0 & amt>0 & in_fpmm_map(from); scene: AMM卖出时内部merge; 对应: InternalBurnFPMM
      │  └─ Convert  // desc: Adapter:Adapter->BurnAddr & amt>0; scene: Convert时Adapter烧NO; 对应: InternalBurnConvert
      └─ 内转  // desc: InternalTransfer子类; scene: 协议间转移; 对应: InternalTransfer*
        ├─ 零值  // desc: *:*->* & amt=0; scene: 零数量Transfer事件; 对应: InternalTransferZero
        ├─ 订单  // desc: CTF|NEG_RISK:*->* & m(order) & is_proto(from) & is_proto(to); scene: 订单双方都是协议合约; 对应: InternalTransferOrder
        ├─ Adapter  // desc: Adapter:*->* & amt>0 & internal_adapter_transfer_path; scene: Adapter其他内部操作(当前实现无直接命中分支,预期=0); 对应: InternalTransferNegRisk
        ├─ FPMM  // desc: FPMM:*->* & amt>0 & internal_fpmm_transfer_path; scene: FPMM其他内部操作(当前实现无直接命中分支,预期=0); 对应: InternalTransferFPMM
        └─ 其他  // desc: *:*->* & amt>0 & !is_user(from) & !is_user(to) & !m(order); scene: 其他协议间转移; 对应: InternalTransferOther

abbr(all)
├─ cond/token: poly=归属Polymarket, treg=出现TokenRegistered, cprep=出现ConditionPreparation, fcreate=出现FPMMCreation
├─ cond/token: fpmm=该condition后续有关联FPMM, nr=命中NegRisk路径, ob=普通订单簿(!fpmm & !nr), xfer_inf=从Transfer推断
├─ cond/token: split_event/merge_event/redemption_event=仅由对应语义事件反推得到
├─ cond/token: other0=兜底未覆盖(!fpmm & !nr & !ob), group_by(x)=按字段x聚合
├─ transfer: amt=amount, known=known_cond, m(x)=match_x, holder=stakeholder, evt=event, tidx=token_idx, coll=collateral
└─ transfer: is_proto=is_protocol, is_user=is_user

## 流程
## Phase 1: 更新映射
### 映射表 (持久化 rb\_\* + 内存)
| 映射                                                                                | Stage1表作为输入                                                                  |
| ----------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| `cond_idx_map[cond_id] -> cond_idx`                                                 | `condition_preparation` / `token_map` / `fpmm` / `split` / `merge` / `redemption` |
| `cond_info_map[cond_idx] -> cond_info:{outcome_count, payout, question_id, source}` | `condition_preparation` / `condition_resolution`                                  |
| `token_info_map[token_id] -> token_info:{cond_idx, token_idx, source}`              | `token_map` / `fpmm` / `split` / `merge` / `redemption` / `transfer`              |
| `fpmm_info_map[fpmm_addr] -> fpmm_info:{cond_idx, coll}`                            | `fpmm`                                                                            |
| `coll_map[cond_idx] -> coll`                                                        | `fpmm` / `split` / `merge` / `redemption`                                         |
| `mid_map[qid] -> mid`                                                               | `neg_risk_question`                                                               |
| `pm_cond_set[cond_idx] -> bool`                                                     | `fpmm`                                                                            |
| `nr_cond_set[cond_idx] -> bool`                                                     | `neg_risk_question`                                                               |

顺序处理 (依赖关系保证):
```
① `condition_preparation`                   → `cond_idx_map` + `cond_info_map`(outcome_count/question_id/source)
② `condition_resolution`                    → `cond_info_map`(payout)
③ `token_map`                               → `cond_idx_map` + `cond_info_map`(source) + `token_info_map`
④ 读取 `fpmm` 原始行(延迟落盘)              → 暂存 `{fpmm_addr, condition_ids[], collateral, conditional_tokens}`
⑤ `split` / `merge` / `redemption` 增量更新 → `cond_idx_map` + `cond_info_map`(source/outcome_count按index_set最高位推断并扩展) + `token_info_map` + `coll_map`(同condition若出现多collateral，按“已知优先+确定性tie-break”规范化)
⑥ 回放暂存 `fpmm` 行并严格落盘              → 仅处理 `conditional_tokens == CONDITIONAL_TOKENS` 的域内行；域内要求 `condition_ids` 全已知，再写 `fpmm_info_map` + `token_info_map` + `coll_map` + `pm_cond_set`
⑦ `neg_risk_question`                       → `mid_map` + `nr_cond_set`
⑧ `token_info_map` 升级规则                 → 按证据强度单向升级(`TransferInferred < FPMM < token_map < split/merge/redemption`)；同映射可仅升级source；`token_map` 同condition反向重复注册可忽略；非推断来源要求 `cond_idx` 一致
→ `update_cond_type_stats()`                → `ConditionTree` / `TokenTree`
```

## Phase 2: 构建语义索引
### 语义索引表 (chunk内存)
| 索引                                                                                                                                           | Stage1表作为输入                                                      |
| ---------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| `tx_split_idx[tx_key:{block, tx_hash}] -> [{log_index, stakeholder, collateral_token, parent_collection_id, cond_id, partition[], amount}]`    | `split`                                                               |
| `tx_merge_idx[tx_key:{block, tx_hash}] -> [{log_index, stakeholder, collateral_token, parent_collection_id, cond_id, partition[], amount}]`    | `merge`                                                               |
| `tx_redemption_idx[tx_key:{block, tx_hash}] -> [{log_index, redeemer, collateral_token, parent_collection_id, cond_id, index_sets[], payout}]` | `redemption`                                                          |
| `tx_convert_idx[tx_market_key:{block, tx_hash, market_id}] -> [{log_index, market_id, index_set, amount, stakeholder}]`                        | `convert`                                                             |
| `tx_order_idx[tx_token_key:{block, tx_hash, token_id}] -> [{log_index, maker, taker, maker_side, usdc, tokens, fee}]`                          | `order_filled`                                                        |
| `tx_fpmm_trade_idx[tx_fpmm_key:{block, tx_hash, fpmm_addr}] -> [{log_index, fpmm_addr, trader, side, outcome_idx, usdc, tokens}]`              | `fpmm_trade`                                                          |
| `tx_fpmm_funding_idx[tx_fpmm_key:{block, tx_hash, fpmm_addr}] -> [{log_index, fpmm_addr, funder, side, amounts[]}]`                            | `fpmm_funding`                                                        |
| `tx_op_bounds_idx[tx_key:{block, tx_hash}] -> [{left_exclusive, right_inclusive}]`                                                             | `split/merge/redemption/convert/order_filled/fpmm_trade/fpmm_funding` |

索引构建规则：
```
1) 先按事件类型入各自索引 (split/merge/redeem/convert/order/trade/funding)
   - `order_filled` 全量入索引；是否可绑定由 Phase3 的 order 地址腿硬约束决定（不在 Phase2 预过滤）
   - FPMM trade/funding 仅对 `fpmm_info_map` 已知的域内 FPMM 建索引（与 Phase1 的 `conditional_tokens` 域过滤一致）
2) 每个 tx 汇总全部语义 log_index，升序去重
3) 基于相邻 op_log_index 生成 tx_op_bounds_idx：
   left_exclusive = prev_op_log_index
   right_inclusive = curr_op_log_index
```

## Phase 3: Transfer 分类 (classify_and_emit)
### 1. 详细流程（树状）
```
phase3_process_transfers(chunk)
├─ 输入(只读)
│  ├─ transfer流: stage1.transfer (按 block_number, log_index)
│  ├─ phase1映射: cond_idx_map/cond_info_map/token_info_map/fpmm_info_map/coll_map/pm_cond_set/nr_cond_set
│  └─ phase2索引: tx_split/merge/redeem/convert/order/trade/funding + tx_op_bounds_idx
├─ 预处理
│  ├─ sort_key = block_number * 1e9 + log_index
│  ├─ 读取 tx_op_bounds_idx 作为语义窗口边界
│  └─ build_transfer_ctx
│     ├─ token_info_map 命中 -> known_cond=true
│     ├─ token_info_map 未命中 -> TransferInferred{cond_idx=UNKNOWN_COND_IDX, token_idx=255}
│     ├─ coll = coll_map[cond_idx], miss=>UnknownCollateral
│     └─ known_cond=false 且命中FPMM语义 且 operator in fpmm_info_map -> coll回填为fpmm.coll
├─ 主循环 for transfer in transfers(by sort_key)
│  ├─ Pass A: transfer -> RootOp 绑定（每条 transfer 最多绑定一次）
│  │  ├─ 双通道绑定:
│  │  │  ├─ 窗口通道: split/merge/redeem/convert 先按 tx_key + op_bounds 取候选 op（窗口命中才绑定）
│  │  │  └─ FPMM通道: trade/funding 按 tx_key + fpmm_addr 在同tx内前向匹配（log_index >= 当前transfer）
│  │  ├─ 再按硬约束过滤:
│  │  │  ├─ split/merge: stakeholder + cond_id + collateral + parent + partition + direction + amount
│  │  │  ├─ redemption: redeemer + cond_id + collateral + parent + index_sets + direction
│  │  │  ├─ convert: market_id + stakeholder + index_set + operator路径(NEG_RISK_ADAPTER)
│  │  │  ├─ order: token_id + maker/taker + maker_side + usdc/tokens（地址腿硬约束；BUY: taker->maker 或 exchange->maker；SELL: maker->taker 或 maker->exchange）
│  │  │  ├─ trade/lp: fpmm_addr + side + actor(trader/funder) + token_amount/transfer_amount
│  │  │  │  ├─ trade_leg_required = (tokens > 0 && trader != fpmm_addr)
│  │  │  │  ├─ observed_trade_leg(side1)=存在 {from=fpmm 或 0->fpmm} 的 amount>0 transfer
│  │  │  │  ├─ observed_trade_leg(side2)=存在 {to=fpmm 或 fpmm->0} 的 amount>0 transfer
│  │  │  │  ├─ lp_add_mint: split_amount == transfer_amount(max(amounts))
│  │  │  │  ├─ lp_add_refund: transfer_amount == split_amount - amounts[token_idx|任意腿]
│  │  │  │  └─ lp_remove: funder == counterparty 且 transfer_amount ∈ amounts
│  │  │  └─ unknown token 仅在通过结构约束时可绑定，不以“地址像不像”放宽
│  │  ├─ split/merge/redeem 匹配策略: 窗口内唯一命中优先；窗口无命中时在同tx内按最近语义log兜底；同距离并列 -> assert(false)
│  │  │  ├─ mint/burn 发生多语义候选时按语义log距离择优（窗口命中优先）；同距离并列 -> assert(false)
│  │  │  └─ split/merge 的 collateral 约束为“严格优先、结构兜底”：先按 collateral 命中；未命中再按同window结构条件匹配
│  │  ├─ FPMM trade多候选决策: 在“trade_leg_required 且 未消费 且 未解释”候选中取最近未来语义log；同log并列 -> assert(false)
│  │  ├─ order 匹配策略: 先按地址腿硬约束筛选，再取窗口内唯一命中；若窗口无命中，取同tx内“未消费且地址腿满足”的最近未来语义log；仅当窗口存在未消费同量候选且不存在可用前向候选时才 assert(false)
│  │  ├─ 多候选同时命中 -> assert(false)
│  │  └─ 唯一命中 -> 记录 root_op + leg_type，并更新 consumed/covered
│  ├─ Pass B: 分类与事件产出
│  │  ├─ 已绑定 transfer: 由 root_op_type + leg_type 驱动分类（不靠单条 transfer 猜语义）
│  │  │  ├─ split: 支持 parent burn + child mint 多腿消费；含 0->FPMM 内部 mint 腿先消费 split 再落 InternalMintFPMM
│  │  │  ├─ merge: 支持多条 burn + parent mint（若存在）；含 FPMM->0 内部 burn 腿先消费 merge 再落 InternalBurnFPMM
│  │  │  ├─ redemption: 支持 index_sets 对应的多条 burn
│  │  │  ├─ convert: 保持 InternalBurnConvert + Convert + (YES侧 SplitNegRisk/TransferInNegRisk) 三段路径
│  │  │  ├─ adapter其他路径: 未命中 split/merge/convert 时按 transfer 兜底（仅 known 且 cond∈nr_cond_set 才归 TransferIn/OutNegRisk；known 非nr归 TransferIn/OutOther；unknown 归 TransferIn/OutNonPoly）
│  │  │  └─ FPMM内部腿 explain: 在同side的“trade_leg_required 且 未消费 且 未解释”候选中取最近未来语义log；若已无待解释候选则允许直接通过
│  │  ├─ 未绑定 transfer: fallback 到 TransferIn*/TransferOut*/Internal*
│  │  ├─ amount==0 -> InternalTransferZero（若命中FPMM trade语义则先消费语义，再按零值分类）
│  │  ├─ N outcome 支持: known token_idx ∈ [0, outcome_count), unknown=255
│  │  └─ 未落类 -> Unclassified -> assert(false)
│  ├─ emit_raw_event（用户22类全部写入，含NonPoly）
│  │  ├─ amount 按用户视角定符号(流入+,流出-)
│  │  ├─ price规则:
│  │  │  ├─ Split/Merge/FPMMLPAdd/FPMMLPRemove/FPMMLPReturn: USDC类coll -> 1e6/outcome_count，否则0
│  │  │  ├─ OrderBuy/Sell/FPMMBuy/Sell: USDC类coll -> usdc*1e6/tokens，否则0
│  │  │  ├─ Redemption: USDC类coll -> payout_numerator[token_idx]，否则0
│  │  │  └─ Convert/TransferIn/TransferOut: 0
│  │  └─ 落库编码: cond_idx=UNKNOWN_COND_IDX 时写 -1；token_idx 保留 255
│  └─ update_xfer_tree
│     ├─ 固定节点 +1
│     └─ 动态节点by_collateral_* 独立维护（局部维护）
│        ├─ by_collateral_split
│        ├─ by_collateral_merge
│        ├─ by_collateral_redemption
│        ├─ by_collateral_order
│        ├─ by_collateral_fpmm_buy
│        ├─ by_collateral_fpmm_sell
│        ├─ by_collateral_lp_add
│        ├─ by_collateral_lp_remove
│        ├─ by_collateral_lp_return
│        └─ by_collateral_transfer
├─ 收尾校验
│  ├─ n_unclass = n_total - n_user - n_internal
│  ├─ assert(n_total == n_user + n_internal + n_unclass)
│  ├─ assert(n_unclass == 0)
│  ├─ assert(每条transfer消费次数 <= 1)
│  ├─ assert(op消费闭环: 每个语义 op 都满足“已消费或可解释例外”)
│  │  ├─ order: 若观察到可消费的 order 腿则 consumed=true；无可观察腿允许零消费
│  │  ├─ trade: !must_consume_or_explain 或 consumed=true 或 explained_without_direct_leg=true
│  │  │        其中 must_consume_or_explain = trade_leg_required && observed_trade_leg(side)
│  │  ├─ convert: consumed_count>0
│  │  ├─ funding: consumed_count>0；FundingRemoved amounts 全零允许零腿
│  │  ├─ split: 若在该 split 语义window内观测到可消费CTF mint腿(0->stakeholder)则 consumed_count>0 或 covered_by_parent=true；amount==0 允许零腿
│  │  ├─ merge: 若在该 merge 语义window内观测到可消费CTF burn腿(stakeholder->0)则 consumed_count>0 或 covered_by_parent=true；amount==0 允许零腿
│  │  └─ redeem: 仅对 payout>0 且在该 redeem 语义window内观测到可消费CTF burn腿的行强制 consumed_count>0 或 covered_by_parent=true；其余允许零消费
│  ├─ assert(op候选唯一性: order/trade/funding/split/merge/redeem/convert 均无重复消费和歧义并列)
│  ├─ assert(语义硬约束成立: actor/cond/collateral/parent/index_set/amount/side/log-window)
│  ├─ assert(Poly类=>is_poly, NegRisk类=>is_nr, NonPoly类=>!is_poly或!known_cond)
│  ├─ assert(convert 在 market 维度的路径与金额关系成立)
│  └─ assert(token_idx==255 -> cond_idx==UNKNOWN_COND_IDX)
└─ 输出
   ├─ user_events(RawEvent序列, 用户22类全写)
   └─ xfer_stats/event_by_collateral 增量
```

### 2. 事件类型
```
输入事件
├─ Transfer原始事件
│  ├─ TransferSingle
│  └─ TransferBatch
└─ 语义事件(由Phase2索引提供匹配能力)
   ├─ PositionSplit      -> m(split)
   ├─ PositionsMerge     -> m(merge)
   ├─ PayoutRedemption   -> m(redeem)
   ├─ PositionsConverted -> m(convert)
   ├─ OrderFilled        -> m(order)
   ├─ FPMMBuy/FPMMSell   -> m(trade)
   └─ FPMMFunding*       -> m(lp_add/lp_refund/lp_remove)

断言层级（Assertion Hierarchy）
├─ L0 输入/结构层：schema/type/range/u256解析合法
├─ L1 映射不变量层：cond/token/collateral/fpmm 映射一致；outcome_count 合法且仅扩展不回退；coll_map 对多来源冲突做确定性规范化
├─ L2 匹配唯一性层：窗口候选命中数 <= 1；同层并列歧义直接 assert
├─ L3 语义约束层：命中后必须满足 actor/cond/collateral/amount/direction/window 等硬约束
├─ L4 语义消费闭环层：chunk 收尾每类语义 op 必须“已消费或显式例外（含 split/merge amount==0、redeem payout==0、trade !must_consume_or_explain 零腿）”
└─ L5 结果守恒层：total 守恒、unclassified==0、树统计恒等式成立

TransferClass输出事件(33类, 唯一落类)
├─ 用户操作(22类)
│  ├─ Split(3):        SplitNormal, SplitNegRisk, SplitNonPoly
│  ├─ Merge(3):        MergeNormal, MergeNegRisk, MergeNonPoly
│  ├─ Redemption(2):   Redemption, RedemptionNonPoly
│  ├─ Poly专属(8):     Convert, OrderBuy, OrderSell, FPMMBuy, FPMMSell, FPMMLPAdd, FPMMLPRemove, FPMMLPReturn
│  └─ Transfer(6):     TransferIn{NegRisk,Other,NonPoly}, TransferOut{NegRisk,Other,NonPoly}
├─ 内部操作(10类)
│  ├─ InternalMint(2): InternalMintNegRisk, InternalMintFPMM
│  ├─ InternalBurn(3): InternalBurnNegRisk, InternalBurnFPMM, InternalBurnConvert
│  └─ InternalXfer(5): InternalTransferZero, InternalTransferOrder, InternalTransferNegRisk, InternalTransferFPMM, InternalTransferOther
└─ 错误(1类)
   └─ Unclassified (必须=0, assert)

RawEvent.type
└─ 用户操作22类全部写入RawEvent(含NonPoly); 内部10类不产出RawEvent(仅计数)
```

### 3. 中间/输出的数据结构
```
Phase3中间结构 (chunk内存)
├─ TransferRow
│  └─ {block, tx_hash, log_index, sort_key, operator, from, to, token_id, amount}
├─ TransferCtx
│  └─ {cond_idx, token_idx, coll, known_cond}
│     ├─ known token: token_idx in [0, outcome_count)
│     └─ unknown token: token_idx=255, cond_idx=UNKNOWN_COND_IDX
├─ RootBind (classify_and_emit 局部状态)
│  └─ {root_op_type, leg_type}，每条 transfer 最多绑定一个 root
├─ Semantic索引行
│  ├─ SplitInfo/MergeInfo/RedemptionInfo {consumed_count, covered_by_parent}
│  ├─ ConvertInfo {consumed_count}
│  ├─ OrderInfo {consumed}
│  ├─ FPMMTradeInfo {requires_erc1155_leg, consumed, explained_without_direct_leg}
│  └─ FPMMFundingInfo {consumed_count}
└─ TxOpBounds
   └─ {left_exclusive, right_inclusive}

Phase3输出结构
├─ user_event (持久化)
│  └─ RawEvent(32B)
│     ├─ sort_key:   i64   = block_number * 1e9 + log_index
│     ├─ cond_idx:   u32   (runtime, unknown=UNKNOWN_COND_IDX)
│     ├─ type:       u8    (22个用户类之一)
│     ├─ token_idx:  u8    (known: 0..outcome_count-1, unknown: 255)
│     ├─ collateral: u8
│     ├─ _pad:       u8
│     ├─ amount:     i64   (raw units, 1e6=$1, 用户流入+,流出-)
│     └─ price:      i64   (price*1e6, 按价格规则计算; 非可定价场景=0)
├─ user_event_row (落库)
│  ├─ cond_idx_i32: int32 (unknown写-1)
│  └─ PK: (user_addr, sort_key, cond_idx_i32, event_type, token_idx)
└─ xfer_stats_delta + event_by_collateral_delta (提交时写入 stage2_cursor 快照)
   ├─ TransferStats: user/internal/unclassified 与 33类固定节点计数
   └─ event_by_collateral: (EventType, Collateral) 维度增量
```

