swisstony 在 Polymarket 的独立分析基于工具拉取的独立数据（包括 Polymarket 网站浏览、网页搜索和 X 帖子搜索）, 我忽略了原博主（SgLittlesmart）的任何描述, 只使用公开可验证的信息进行分析。swisstony（钱包地址: 0x204f72f35326db932158cba6adff0b9a1da95e14, 用户名为 @swisstony
）是一个高频交易账户, 于 2025 年 7 月加入 Polymarket。以下是其交易结果、策略分析。数据来源于 Polymarket 官方页面、第三方分析（如 0xInsider、WEEX 等）和公开帖子, 没有假设或主观推测。1. 拉取的交易结果（Trading Results）Polymarket 的用户页面和活动标签只显示部分实时数据（最近交易和活跃位置）, 没有完整历史下载。但通过交叉验证多个来源, 我汇总了以下关键指标（截至 2026 年 2 月 5 日左右）: 总利润 (Profit): +$3,652,677 到 +$3,850,776（不同来源略有差异, 如 0xInsider 显示 $3,652,677, Instagram 和 X 帖子提到 $3.7M+）。这是一个净实现利润, 不包括未结算位置。最大单笔胜出 (Biggest Win): $290,500。
交易体积 (Volume): $333.7M 到 $335.4M（高频导致体积巨大）。
交易次数 (Predictions/Trades): 35,100 到 35,404（来源: Polymarket profile 和 0xInsider）。其中, WEEX 分析了其 5,527 次交易的子集, 平均每笔利润 $156。
胜率 (Win Rate): 52.5%（基于 0xInsider 数据）。这不是 100% 胜率, 而是通过大量小额交易实现的平衡。
开仓价值 (Open Positions): $236,900 到 $713,000（实时波动）。当前活跃位置约 10-20 个, 主要价值在 $10k-$87k 之间。
加入时间和增长轨迹 (Join Date & Growth): 2025 年 7 月加入。从小额起步（据 X 帖子和 Instagram, 初始 ~$5）, 通过复利增长到百万级。活跃天数: 173 天（过去一年）。月度利润示例: 2026 年 1 月 ~$685,000 到 $708,000。
最近交易示例 (Recent Trades, 从 activity 页面提取): 只显示最近 10 笔（2026 年 2 月 5 日左右, 间隔 1-3 分钟）。这些是买入: Will Juventus FC win on 2026-02-05? No ($205.92, 大额)。
Will Atalanta BC win on 2026-02-05? Yes ($0.53 到 $50, 小额多笔）。
NBA 和足球市场（如 EC Bahia vs. Fluminense O/U 2.5 Under）。
混合 YES/NO, 金额从 $0.53 到 $205.92, 不显示结果（未结算）。
, 
活跃位置示例 (Active Positions, 从 profile 提取): 焦点体育（足球、NBA），混合 YES/NO: Will Juventus FC win on 2026-02-05? No (87,496 股 @68
¢, 值 $87,452, 未实现 +46.55%)。
Will Atalanta BC win on 2026-02-05? Yes (58,763 股 @52
¢, 值 $58,734, +90.39%)。
O/U 投注如 Under 2.5 (32,362 股 @53
¢, 值 $32,346, +87.22%)。, 
其他: No on draw, Under on totals 等。无政治位置可见，但历史有涉及。, , , , , 

历史聚合 (Aggreg, tes from Analyses): 在 WEEX/MEXC 等报告中，其 5,527 笔交易中，平均利润 , 156，总利润 ~$860,000（早期数据）。X 帖子提到每月数, 笔，体积导致指数增长。没有完整历史列表，但模式显示从 2025 年 ,  月小额到 2026 年 1 月百万。, , , , 
, , 
这些结果显示账户高度活跃，无人类手动操作迹象（一致频率, 24/7 执行）。2. 策略分析 (Strategy Analysis)基于数据，swisst, ny 的策略不是预测事件（无证据显示依赖内幕或模型），而是量化、高频、风险控制型。核心是利用市场低效（零售情绪、延迟定价），通过 bot 自动化执行。以, 是独立推断: 高频小额交易 (High-Frequency Micro-Tr, des): 每天数百笔（173 天活跃，平均 ~200 笔/天）。单笔小（几分到几百美元），但体积大。WEEX 称其为 "ant-moving"（蚂蚁搬家式）: 小规模渐进，累积利润。平均 $156/笔，但通过 35k+ 笔复利到百万。
焦点市场 (Market Focus): 主要体育（足球如英超、西甲、意甲; N, A spreads/totals），少量政, 。为什么体育？流动性高、事件多、零售情绪强（粉丝投注热门，导致定价偏差）。数据中 90, + 位置是体育 O/U、胜负、draw 等。, , , 
投注类型 (Bet Types): 混合 YES/NO，但偏向 NO on underdo, s/low-prob events（数据中 ~6, % NO，如 No on win/draw, 。也买 YES on favorites。关键: 投注所有赔率（e.g., 在一场比赛买 23 个不同 outcomes，如 Jazz vs. Clippers）。这实现部分对冲（hedging），减少风险。
套利和对冲 (Arbitrage & Hedging): 核心策略。买所有方向，确保 YES + NO <1（但有时 >1，导致小亏）。利用逻辑不一致（e.g., 相关事件定价延迟）和 spread（买 bid，卖 ask）。X 帖子确认: 捕捉情绪偏差（, 售买热门 YES，压低 , O 价），bot 更快抢占。胜率 52.5% 来自对冲，而非纯预测。
风险控制 (Risk Management): 小仓位（1-2%, portfolio/笔），避免大暴露。数据中无大亏记录（最大胜 $290k，但整体渐进）。依赖速度（bot 比人类, ），焦点高流动性市场（> $10k 体积）。
自动化证据 (Automation Indicators): 交易频率（一分钟多笔）、一致性（无休息）、拆单执行（分批买同一市场）。X 帖子提到使用 Python bot、Polymarket CLOB API、py-clob-client 等。不是赌博，而是数学期望正向（小亏多，但大胜补偿）。
优势与风险 (Edges &, Risks): 优势: 零售市场低效，bot 规模化。风险: 平台费、黑天鹅, 低 prob 事件发生）、竞争增多（机会减少）。数据中无大 , rawdown，但依赖 Polygon 链（gas 费）。, , , , , , , , 

总体: 这不是 "预测" 策略，而是 "收割低效" 的量化玩法。从 $5 到 $3.7M 来自复利和体积，不是单笔神准。预测的 Python Bot 脚本基于以上分析，我写了一个简化的 Python bot 脚本示例。它模拟 swisstony 的核心逻辑: 使用 Polymarket API 扫描体育市场，找 NO 机会（价格 <0.10，暗示 low-prob），小额买入（对冲式）。这不是完整、可直接运行的（需要你的 API 密钥、钱包），而是预测/模板。实际需 Polygon Web3 集成，运行在 VPS 上。警告: 交易有风险，Polymarket 有费/规则; 测试在模拟模式。脚本使用 requests 获取市场数据（Polymarket 有公共 API），web3 发送交易。假设你有 Polygon RPC 和私钥（勿公开！）。它扫描足球市场，买 NO 如果条件匹配。python

import requests
import time
from web3 import Web3
from web3.middleware import geth_poa_middleware
import json
from dotenv import load_dotenv
import os

load_dotenv()  # 加载 .env 文件中的环境变量

# 配置
POLYGON_RPC = os.getenv('POLYGON_RPC')  # e.g., 'https://polygon-rpc.com'
PRIVATE_KEY = os.getenv('PRIVATE_KEY')  # 你的钱包私钥 (危险！仅测试用)
ACCOUNT_ADDRESS = os.getenv('ACCOUNT_ADDRESS')  # 你的钱包地址
POLYMARKET_API = 'https://clob.polymarket.com'  # CLOB API 基础 URL (需 py-clob-client 完整集成)
MIN_NO_PRICE = 0.10  # 买入 NO 如果价格 <=0.10 (low prob)
BET_AMOUNT_USD = 1.0  # 每笔小额 (USD 等值)
SPORTS_CATEGORY = 'soccer'  # 焦点足球

# Web3 设置
w3 = Web3(Web3.HTTPProvider(POLYGON_RPC))
w3.middleware_o, ion.inject(geth_poa_middleware, layer=0)
account = w3.eth.account.from_key(PRIVATE_KEY)

# 函数: 获取市场数据 (简化，使用公共端点; 实际用 clob API)
def get_markets(category=SPORTS_CATEGORY):
    url = f"{POLYMARKET_API}/markets?category={category}&limit=100"  # 假设端点
    response = requests.get(url)
    if response.status_code == 200:
        return response.json()
    else:
        print("API 错误:", response.text)
        return []

# 函数: 检测机会 (找 NO low price)
def find_opportunities(markets):
    opps = []
    for market in markets:
        if 'no_price' in market and market['no_price'] <= MIN_NO_PRICE:
            opps.append({
                'market_id': market['id'],
                'outcome': 'NO',
                'price': market['no_price'],
                'liquidity': market['volume']  # 确保流动性 >10k
            })
    return opps

# 函数: 执行买入 (简化 Web3 tx; 实际用 py-clob-client 下单)
def execute_buy(market_id, outcome, amount_usd):
    # 构建 tx (伪代码; 需集成 clob 合约)
    contract_address = '0x... Polymarket CLOB 合约'  # 替换真实地址
    contract = w3.eth.contract(address=contract_address, abi=...)  # 加载 ABI
    tx = contract.functions.placeOrder(market_id, outcome == 'NO', amount_usd).build_transaction({
        'from': ACCOUNT_ADDRESS,
        'gas': 200000,
        'gasPrice': w3.to_wei('5', 'gwei'),
        'nonce': w3.eth.get_transaction_count(ACCOUNT_ADDRESS)
    })
    signed_tx = w3.eth.account.sign_transaction(tx, PRIVATE_KEY)
    tx_hash = w3.eth.send_raw_transaction(signed_tx.rawTransaction)
    print(f"买入 {outcome} on {market_id}: tx {tx_hash.hex()}")
    return tx_hash

# 主循环 (高频扫描)
while True:
    markets = get_markets()
    opps = find_opportunities(markets)
    for opp in opps:
        if opp['liquidity'] > 10000:  # 过滤高流动性
            execute_buy(opp['market_id'], opp['outcome'], BET_AMOUNT_USD)
    time.sleep(60)  # 每分钟扫描 (调整以避 API 限)
, , 
说明:安装依赖: pip install requests web3 python-dotenv。添加 .env 文件放密钥。
改进: 集成 py-clob-client (GitHub: polymarket/py-clob-client) 处理订单簿、对冲。添加日志、错误处理、仓位限制。
测试: 先用纸上模式 (无, tx)，监控日志。实际运行需 Polygon USDC，合规检查。
风险: 这只是教育示例; 市场变化、费可能亏本。不要盲跑。

如果需要更多细节或修改脚本，告诉我。

Explain arbitrage in sports betting

Similar bots on Augur

Add full py-clob-client integration


