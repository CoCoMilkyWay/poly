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
所有协议操作都是**原子的**，在同一个函数调用中完成 Transfer 和 emit 语义事件：
| 操作                     | Transfer                                  | 语义事件           | 来源     |
| ------------------------ | ----------------------------------------- | ------------------ | -------- |
| CTF.splitPosition        | `_batchMint` → TransferBatch              | PositionSplit      | 同一函数 |
| CTF.mergePositions       | `_batchBurn` → TransferBatch              | PositionsMerge     | 同一函数 |
| CTF.redeemPositions      | `_burn` → TransferSingle                  | PayoutRedemption   | 同一函数 |
| Exchange.fillOrder       | `safeTransferFrom` × 2                    | OrderFilled        | 同一函数 |
| FPMM.buy                 | splitPosition + safeTransferFrom          | FPMMBuy            | 同一函数 |
| FPMM.sell                | safeTransferFrom + mergePositions         | FPMMSell           | 同一函数 |
| FPMM.addFunding          | splitPosition + safeBatchTransferFrom     | FPMMFundingAdded   | 同一函数 |
| FPMM.removeFunding       | safeBatchTransferFrom (NOT burn!)         | FPMMFundingRemoved | 同一函数 |
| NegRisk.convertPositions | splitPosition + safeBatchTransferFrom × 3 | PositionsConverted | 同一函数 |
**边界情况**：用户直接调用 `safeTransferFrom` 只有 Transfer、无语义事件 → 分类为 TransferIn/Out


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
   │  └─ FPMM  // desc: poly & fcreate & pfactory & !treg; scene: 只创建AMM池,未注册Exchange; 对应: cond_tree.polymarket.fpmm_poly
   └─ 其他  // desc: 非Polymarket子树; scene: 无法确认属于Polymarket; 对应: cond_tree.other.total
      ├─ Prep  // desc: !poly & cprep; scene: 早期或其他协议(如Omen); 对应: cond_tree.other.prep
      ├─ FPMM  // desc: !poly & fcreate & !pfactory; scene: 只监听PolyFactory,其他Factory不会收到; 对应: cond_tree.other.fpmm_other

代币树 (`s2-token-tree`)
└─ 代币  // desc: token分类总览; scene: ERC1155 token_id来源分层; 对应: token_tree.total
   ├─ Polymarket  // desc: Polymarket子树; scene: 已确认归属Polymarket; 对应: token_tree.polymarket.total
   │  ├─ TokenReg  // desc: TokenRegistered子类; scene: Exchange事件直接给token; 对应: token_tree.polymarket.token_reg.total
   │  │  ├─ AMM  // desc: poly & treg & amm; scene: 注册后又创建了AMM池; 对应: token_tree.polymarket.token_reg.amm
   │  │  ├─ NegRisk  // desc: poly & treg & nr; scene: 多选题市场代币; 对应: token_tree.polymarket.token_reg.negrisk
   │  │  ├─ OB  // desc: poly & treg & ob; scene: 无AMM池,只有挂单; 对应: token_tree.polymarket.token_reg.orderbook
   │  │  └─ Other(预期=0)  // desc: poly & treg & other0; scene: Polymarket-TokenReg未覆盖项; 对应: token_tree.polymarket.token_reg.other
   │  └─ FPMM  // desc: poly & fcreate & pfactory & !treg; scene: 只创建AMM池,未注册Exchange; 对应: token_tree.polymarket.fpmm_poly
   │     └─ (动态) by_collateral_fpmm
   └─ 其他  // desc: 非Polymarket子树; scene: 无法确认属于Polymarket; 对应: token_tree.other.total
      ├─ FPMM  // desc: !poly & fcreate & !pfactory; scene: 只监听PolyFactory,其他Factory不会收到; 对应: token_tree.other.fpmm_other
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
├─ cond/token: poly=归属Polymarket, treg=出现TokenRegistered, cprep=出现ConditionPreparation, fcreate=出现FPMMCreation, pfactory=来自PolyFactory
├─ cond/token: fpmm=该condition后续有关联FPMM, nr=命中NegRisk路径, ob=普通订单簿(!fpmm & !nr), xfer_inf=从Transfer推断
├─ cond/token: other0=兜底未覆盖(!fpmm & !nr & !ob), group_by(x)=按字段x聚合
├─ transfer: amt=amount, known=known_cond, m(x)=match_x, holder=stakeholder, evt=event, tidx=token_idx, coll=collateral
└─ transfer: is_proto=is_protocol, is_user=is_user

### RawEvent (内存, 32字节)
```
sort_key:   i64   block_number * 1e9 + log_index
cond_idx:   u32
type:       u8    EventType
token_idx:  u8    0=YES, 1=NO
collateral: u8    Collateral enum
_pad:       u8
amount:     i64   raw units (1e6 = $1), 负数=流出
price:      i64   price * 1e6, 非USDC时为0
```

## 流程
## Phase 1: 更新映射
### 映射表 (持久化 rb\_\* + 内存)
| 映射                                                                                | Stage1表作为输入                                                                  |
| ----------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| `cond_idx_map[cond_id] -> cond_idx`                                                 | `condition_preparation` / `token_map` / `fpmm` / `split` / `merge` / `redemption` |
| `cond_info_map[cond_idx] -> cond_info:{outcome_count, payout, question_id, source}` | `condition_preparation` / `condition_resolution`                                  |
| `token_info_map[token_id] -> token_info:{cond_idx, is_yes, source}`                 | `token_map` / `fpmm` / `split` / `merge` / `redemption` / `transfer`              |
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
④ `fpmm`                                    → `cond_idx_map` + `cond_info_map`(source) + `fpmm_info_map` + `token_info_map` + `coll_map` + `pm_cond_set`
⑤ `split` / `merge` / `redemption` 增量更新 → `cond_idx_map` + `cond_info_map`(source) + `token_info_map` + `coll_map`
⑥ `neg_risk_question`                       → `mid_map` + `nr_cond_set`
→ `update_cond_type_stats()`                → `ConditionTree` / `TokenTree`
```

## Phase 2: 构建语义索引
### 语义索引表 (chunk内存)
| 索引                                                                                                                           | Stage1表作为输入 |
| ------------------------------------------------------------------------------------------------------------------------------ | ---------------- |
| `tx_split_idx[tx_key:{block, tx_hash}] -> [{amount, stakeholder, cond_id}]`                                                    | `split`          |
| `tx_merge_idx[tx_key:{block, tx_hash}] -> [{amount, stakeholder, cond_id}]`                                                    | `merge`          |
| `tx_redemption_idx[tx_key:{block, tx_hash}] -> [{payout, redeemer, cond_id}]`                                                  | `redemption`     |
| `tx_convert_idx[tx_market_key:{block, tx_hash, market_id}] -> [{market_id, index_set, amount, stakeholder}]`                   | `convert`        |
| `tx_order_idx[tx_token_key:{block, tx_hash, token_id}] -> {maker, taker, maker_side, usdc, tokens, fee}`                       | `order_filled`   |
| `tx_fpmm_trade_idx[tx_fpmm_key:{block, tx_hash, fpmm_addr}] -> [{fpmm_addr, trader, side, outcome_idx, usdc, tokens}]`         | `fpmm_trade`     |
| `tx_fpmm_funding_idx[tx_fpmm_key:{block, tx_hash, fpmm_addr}] -> [{fpmm_addr, funder, side, amount0, amount1, amounts_count}]` | `fpmm_funding`   |

## Phase 3: Transfer 分类 (classify_and_emit)
TransferClass 完整分类 (33类)
```
Split(3):        SplitNormal, SplitNegRisk, SplitNonPoly
Merge(3):        MergeNormal, MergeNegRisk, MergeNonPoly
Redemption(2):   Redemption, RedemptionNonPoly
Poly专属(8):     Convert, OrderBuy, OrderSell, FPMMBuy, FPMMSell, FPMMLPAdd, FPMMLPRemove, FPMMLPReturn
Transfer(6):     TransferIn{NegRisk,Other,NonPoly}, TransferOut{NegRisk,Other,NonPoly}
InternalMint(2): InternalMintNegRisk, InternalMintFPMM
InternalBurn(3): InternalBurnNegRisk, InternalBurnFPMM, InternalBurnConvert
InternalXfer(5): InternalTransferZero, InternalTransferOrder, InternalTransferNegRisk, InternalTransferFPMM, InternalTransferOther
Error(1):        Unclassified (必须=0, assert验证)
```

`verify()`: `total == user_events + internal + unclassified`, `unclassified == 0`

