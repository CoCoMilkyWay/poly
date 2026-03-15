# 夏普比率计算方法
_Exported on 3/16/2026 at 00:16:51 GMT+8 from Cursor (2.3.35)_

---

**User**

@README_stage3.md 

现在的夏普计算还是有问题， 算的不对， 很多值1/-1的， 要么就是0.00几， 和净值/pnl对不上

我们不应该用exposure+pnl来近似净值， 而是应该直接用pnl， 因为exposure本身变动大， 经常有转账


对于一个净值曲线， 采样点分布非常不均匀的， 不考虑risk free rate
一个区间的夏普应该正确的怎么算？

采样点形式为 （time_index, net_value）

对于净值曲线采样点不均匀（时间间隔不固定）的情况，直接用普通夏普公式会出错，因为普通夏普默认收益率是等时间间隔。
你这种数据是 (time_index, net_value)，而且 不考虑 risk-free rate，那么夏普需要按时间加权收益率来算。

下面是正确思路 👇

一、普通夏普为什么不适用
普通夏普：

S = \frac{\mu}{\sigma}

这里默认

μ = 每期平均收益

σ = 每期收益标准差

每期时间长度相同

但你的数据：

(t0, v0)
(t1, v1)   Δt1 ≠ Δt2 ≠ Δt3
(t2, v2)
(t3, v3)
收益的时间跨度不同 → 方差不能直接算

二、正确方法：先转成连续收益率 + 时间归一化
对每个区间：

ri = log(vi / v(i-1))
Δti = ti - t(i-1)
得到：

r1, r2, r3 ...
Δt1, Δt2, Δt3 ...
三、时间加权平均收益
平均收益应该按时间权重：

μ = Σ(ri) / Σ(Δti)
这表示单位时间的平均log收益

四、时间加权方差（关键）
正确的方差：

σ² = Σ(Δti * (ri/Δti − μ)²) / Σ(Δti)
解释：

ri/Δti = 单位时间收益率

Δti 作为权重

标准差：

σ = sqrt(σ²)
五、Sharpe（不考虑无风险）
最终：

Sharpe = μ / σ
注意这里是单位时间 Sharpe

如果想换成年化，需要乘：

Sharpe_annual = Sharpe * sqrt(T_unit)
例如：

time_index单位	sqrt因子
秒	√(一年秒数)
分钟	√(一年分钟数)
天	√252
自定义index	√(一年index数)
六、更稳定的做法（推荐）
更金融正确的做法是：

Step1 用 log return
Step2 转成单位时间收益
Step3 再算均值和方差
公式：

ui = ri / Δti
μ = mean(ui, weight=Δti)
σ = std(ui, weight=Δti)

Sharpe = μ / σ
这是最标准做法 ✅

七、如果你愿意，我可以帮你写
numpy版本

pandas版本

pytorch版本

高频交易版本

vectorized版本

只要把你的 (time, value) 示例发来。


python

下面给你一个严格正确的 Python 实现，适用于

采样时间不均匀

输入 (time_index, net_value)

不考虑 risk-free rate

使用 log return

时间加权 Sharpe

可年化

这是量化里比较标准的一种实现。

✅ 数学定义（当前代码实现的公式）
区间收益：

ri = log(vi / v(i-1))
Δti = ti - t(i-1)
ui = ri / Δti
时间加权均值：

μ = Σ(Δti * ui) / Σ(Δti)
时间加权方差：

σ² = Σ(Δti * (ui − μ)²) / Σ(Δti)
Sharpe：

S = μ / σ
年化：

S_annual = S * sqrt(period_per_year)
✅ Python实现（推荐版本）
import numpy as np


def sharpe_irregular_time(
    time_index,
    net_value,
    periods_per_year=None,
):
    """
    time_index : array-like, increasing
    net_value  : array-like, same length
    periods_per_year : float or None
        用于年化，例如:
        252 (daily)
        365
        252*390 (分钟)
        None -> 不年化
    """

    t = np.asarray(time_index, dtype=float)
    v = np.asarray(net_value, dtype=float)

    if len(t) < 2:
        return np.nan

    # Δt
    dt = np.diff(t)

    # log return
    r = np.log(v[1:] / v[:-1])

    # 单位时间收益
    u = r / dt

    # 时间权重
    w = dt

    # 加权均值
    mu = np.sum(w * u) / np.sum(w)

    # 加权方差
    var = np.sum(w * (u - mu) ** 2) / np.sum(w)

    sigma = np.sqrt(var)

    if sigma == 0:
        return np.nan

    sharpe = mu / sigma

    # 年化
    if periods_per_year is not None:
        sharpe = sharpe * np.sqrt(periods_per_year)

    return sharpe
✅ 使用示例
time_index = [0, 1, 2, 5, 9, 10]
net_value = [1, 1.01, 1.02, 1.10, 1.08, 1.15]

s = sharpe_irregular_time(
    time_index,
    net_value,
    periods_per_year=252
)

print(s)
✅ 如果是高频数据推荐这样设
数据类型	periods_per_year
日频	252
小时	252*6.5
分钟	252*390
秒	25239060
自定义time_index	用一年包含多少index
✅ 如果你做的是 HFT / RL / 回测 / crypto
我可以给你写更专业版本：

支持 nan

支持 torch

支持 rolling sharpe

支持 streaming

支持 million points

支持 GPU

直接说。

---

**Cursor**

我先查一下仓库里当前夏普的实现位置和数据来源，确认它现在到底是按 `exposure/realized` 近似，还是已经有一套别的增量公式。

我已经定位到实现：当前代码确实是用 `realized_delta / exposure_before` 当事件收益率，再做时间加权，这和你说的问题是一致的。接下来我读下具体公式和 stage3 现有能拿到的 `rpnl/upnl` 数据，判断能不能直接改成净值曲线法。

我看到 API timeline 已经直接暴露了每个采样点的 `rpnl/upnl`，也就是净值曲线至少在查询层是现成的。还要继续确认内部特征表现在有没有把“净值夏普”的必需中间量持久化，不然窗口 `100w/1000w` 可能需要顺带改前缀字段。

这里有一个关键实现约束我刚确认到：系统里目前可直接拿到的是每个事件后的 `rpnl + upnl`，也就是 `pnl` 曲线；但它不是传统永远为正的 `net_value`，很可能会穿过 0 甚至为负。这样你给的 `log(v_i / v_{i-1})` 公式在工程上会遇到定义域问题，所以我需要先跟你确认这块口径。

我先给你结论：如果采样点真的是严格正的 `net_value`，更标准的连续时间口径是先算 `r_i = log(v_i / v_{i-1})`，再用总时间 `T = ΣΔt_i` 估计 `μ = Σr_i / T`、`σ² = Σ(r_i - μΔt_i)^2 / T`，最后 `Sharpe = μ / σ`。  
你现在仓库里有两层问题：一层是分子用了 `realized_delta / exposure_before`，另一层是方差项也写偏了，代码现在累计的是 `r_i² * Δt_i`，这和不均匀采样下要的“按时间处理 log-return”不是一回事。

---

**User**

1. 对于给定时间范围ie， total return 可以被理解为 +/- abs（return）， 符号和log分开处理不就可以了？
中间的多个采样点， 也是一样的， 但是注意一定要做好时间加权， 因为一个用户的事件是极度不均匀的， 他可能段时间交易几百次， 然后一两年都不动

中间的事件， 都是算相邻事件的log return， 我不理解为什么 pnl正负toggle会有问题

---

**Cursor**

问题不在“不均匀采样”，而在你现在拿来做 `log` 的量是不是严格正数。

如果采样点是标准净值 `V_t > 0`，那你的方向是对的：先做相邻区间收益，再按时间加权。  
但如果你现在要直接拿 `pnl_t = rpnl_t + upnl_t` 当“净值”，`pnl_t` 会出现这几种情况：

```text
-100 -> -50
```

这是变好了，但
`log((-50)/(-100)) = log(0.5) < 0`
会给出负收益，方向反了。

```text
-50 -> +50
```

这是更明显地变好了，但
`log(50 / -50)`
直接没定义。

所以不是我说“toggle 就一定不能做”，而是“标准 log return 只对严格正的财富过程有定义”。  
你说的“符号和 log 分开”可以做，但那已经不是标准 log return，而是一个你自定义的 signed transform，必须把区间符号规则写死，否则实现会有歧义。

最直观的反例是这两个区间：

```text
-100 -> +100
-100 -> -100
```

如果只看 `abs`，二者的 `|v1|/|v0|` 都是 `1`，`log(1)=0`，但第一个区间其实是巨大改善，第二个区间是没变化。  
所以“sign 和 log 分开”不能只写一句话，必须明确：

1. 区间方向到底取 `sign(v_i - v_{i-1})`，还是别的定义？
2. 幅度到底取 `log(|v_i| / |v_{i-1}|)`，还是 `abs(log(...))`？
3. `|v|` 接近 0 时怎么处理？否则会数值爆炸。
4. 穿过 0 的区间，是允许一个有限收益，还是直接特殊处理？

从数学上讲：

1. 如果你有严格正的账户权益曲线 `equity_t`，最标准的是  
   `r_i = log(equity_i / equity_{i-1})`  
   `u_i = r_i / Δt_i`  
   `μ = Σ(Δt_i * u_i) / ΣΔt_i = Σr_i / T`  
   `σ² = Σ(Δt_i * (u_i - μ)^2) / T`  
   `Sharpe = μ / σ`

2. 如果你只有可正可负的 `pnl_t`，那就没有“标准 log Sharpe”了。此时只能二选一：
   一是先把它变成严格正的权益曲线再算。
   二是定义一个你自己的 signed-return 变换，再对这个变换后的区间收益做时间加权均值/波动。

另外你仓库里当前实现还有第二个硬问题：它现在累计的是

```text
return_sum += r_i
return_tw_sum += r_i * Δt_i
return_sq_tw_sum += r_i^2 * Δt_i
```

然后再拿这些量去拼方差。  
但如果真正要算的是 `u_i = r_i / Δt_i` 的时间加权方差，那么充分统计量应该围绕 `u_i`，也就是本质上要保 `Σr_i` 和 `Σ(r_i^2 / Δt_i)` 这一类量，而不是现在这套 `r_i * Δt_i` / `r_i^2 * Δt_i`。所以即使先不谈 `exposure_before`，现有方差公式本身也偏了。

我先确认一个关键口径，然后我就能给你精确改法。

```plaintext
-100 -> -50
```

```plaintext
-50 -> +50
```

```plaintext
-100 -> +100
-100 -> -100
```

```plaintext
return_sum += r_i
return_tw_sum += r_i * Δt_i
return_sq_tw_sum += r_i^2 * Δt_i
```

---

**User**

问题是exposure经常大幅度变， 有频繁的出入金问题

这个情况就比前面复杂一层了：
你有

(time_index, exposure, cumulative_pnl)

但 exposure 经常大幅变化 + 有出入金，这意味着：

exposure 不能再当作 capital
cumulative_pnl 也不能直接当净值
Sharpe 必须用 PnL / 有效风险规模 的时间加权收益

这是典型的：

CTA / 高频 / 做市

RL策略

动态保证金

有资金流入流出

多账户合并

杠杆变化

这种情况下，最稳健的方法是：

用 exposure 的变化构造 effective capital，再算单位风险收益

下面给你一个在实盘里常用的近似方法。

✅ 推荐方法：用平均暴露当资本（最稳健）

用区间平均 exposure：

cap_i = (|exp_i| + |exp_{i-1}|) / 2

比用单点 exposure 稳定很多。

收益：

r_i = pnl_i / cap_i

时间不均匀：

u_i = r_i / Δt_i
w_i = Δt_i

Sharpe：

μ = Σ(w u) / Σ(w)
σ² = Σ(w (u-μ)²) / Σ(w)
S = μ / σ

这是实际交易里非常常见的做法。

✅ 更稳的方法（推荐）：用 RMS exposure

当 exposure 波动很大时，用 RMS 更合理：

cap_i = sqrt((exp_i² + exp_{i-1}²)/2)

比平均值更稳定。

这是做市 / 高频 / 期货里常用的。

✅ Python（推荐最终版本）
import numpy as np


def sharpe_with_flows(
    time_index,
    exposure,
    cumulative_pnl,
    periods_per_year=None,
    eps=1e-12,
    method="rms",  # "mean" | "rms"
):
    t = np.asarray(time_index, float)
    e = np.asarray(exposure, float)
    pnl_cum = np.asarray(cumulative_pnl, float)

    if len(t) < 2:
        return np.nan

    dt = np.diff(t)
    pnl = np.diff(pnl_cum)

    e0 = e[:-1]
    e1 = e[1:]

    if method == "mean":
        cap = (np.abs(e0) + np.abs(e1)) / 2

    elif method == "rms":
        cap = np.sqrt((e0**2 + e1**2) / 2)

    else:
        raise ValueError

    cap = np.maximum(cap, eps)

    r = pnl / cap

    u = r / dt
    w = dt

    mu = np.sum(w * u) / np.sum(w)

    var = np.sum(w * (u - mu) ** 2) / np.sum(w)

    sigma = np.sqrt(var)

    if sigma == 0:
        return np.nan

    sharpe = mu / sigma

    if periods_per_year is not None:
        sharpe *= np.sqrt(periods_per_year)

    return sharpe
✅ 为什么这个方法适合你

你的问题：

问题	影响
exposure大幅变化	不能直接当capital
有出入金	cumulative pnl ≠ return
时间不均匀	不能普通Sharpe
pnl正负	必须归一化
高频	需要单位风险

这个方法解决全部问题。

✅ 如果你愿意

可以把真实数据结构发出来，例如

t, exp, pnl

我可以给你写一个

✔ 和 quantlib 接近
✔ 和 CTA评估一致
✔ 和 HFT论文一致
✔ 和 prop desk一致

的 Sharpe 计算方法。

---

**Cursor**

你这个方向比“直接用 `pnl` 当净值做 log-return”更贴近你当前系统的数据现实，我认同。对这个仓库来说，更合理的口径应当是：

1. 用区间总 PnL 变化，而不是 `realized_delta / exposure_before`
2. 用区间有效风险资本 `cap_i` 归一化，而不是把 `exposure` 当净值
3. 再对不均匀时间间隔做时间加权

当前实现正好踩了前两条，而且第三条的方差统计量也不对：

```146:167:core-backend/src/stage3/stage3_comp_feat.cpp
void accumulate_event_delta(BucketAggState &agg, double realized_delta, int64_t exposure_before, int64_t volume, int64_t sort_key, int64_t current_block, int64_t prev_block) {
  constexpr int64_t kMinExposure = 1000;
  if (exposure_before >= kMinExposure) {
    const long double return_rate = static_cast<long double>(realized_delta) / static_cast<long double>(exposure_before);
    const int64_t return_rate_1e6 = round_i64(return_rate * 1e6);
    agg.return_sum += return_rate_1e6;
    const int64_t delta_t = (prev_block > 0) ? std::max<int64_t>(0, current_block - prev_block) : 0;
    if (agg.first_block == 0) {
      agg.first_block = current_block;
    }
    agg.return_tw_sum += static_cast<__int128>(return_rate_1e6) * delta_t;
    const long double sq = static_cast<long double>(return_rate_1e6) * static_cast<long double>(return_rate_1e6);
    agg.return_sq_tw_sum += static_cast<__int128>(sq + 0.5L) * delta_t;
  }
}
```

```138:166:core-backend/src/stage3/stage3_sync_runtime.cpp
double calc_sharpe_from_returns(int64_t return_sum, __int128 return_tw_sum, __int128 return_sq_tw_sum, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double sum = static_cast<long double>(return_sum);
  const long double tw_sum = i128_to_long_double(return_tw_sum);
  const long double sq_tw_sum = i128_to_long_double(return_sq_tw_sum);

  const long double r_bar = sum / T;
  const long double tw_avg = tw_sum / T;
  const long double sq_tw_avg = sq_tw_sum / T;
  const long double variance = sq_tw_avg - 2.0L * r_bar * tw_avg + r_bar * r_bar;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(r_bar / std::sqrt(static_cast<double>(variance)));
}
```

如果按你现在这个“带出入金、带暴露剧烈变化”的语境，建议改成这套：

- 采样点：`(t_i, pnl_i, exp_i)`，其中 `pnl_i = rpnl_i + upnl_i`
- 区间 PnL：`Δpnl_i = pnl_i - pnl_{i-1}`
- 有效资本：`cap_i = f(exp_{i-1}, exp_i)`，推荐 `RMS`
- 区间收益：`r_i = Δpnl_i / max(cap_i, eps)`
- 单位时间收益：`u_i = r_i / Δt_i`
- 时间加权均值：`μ = Σ(Δt_i * u_i) / ΣΔt_i = Σr_i / T`
- 时间加权方差：`σ² = Σ(Δt_i * (u_i - μ)^2) / T = Σ(r_i^2 / Δt_i) / T - μ^2`
- `Sharpe = μ / σ`

这里有一个实现上的关键岔路必须先定，不然我给不出准确 plan。  
因为当前时间轴是按 `block` 算的，而同一 `block` 内会有很多事件，导致 `Δt=0`。这种情况下，上面公式里的 `r_i / Δt_i` 是没法直接算的。



```cpp
void accumulate_event_delta(BucketAggState &agg, double realized_delta, int64_t exposure_before, int64_t volume, int64_t sort_key, int64_t current_block, int64_t prev_block) {
  constexpr int64_t kMinExposure = 1000;
  if (exposure_before >= kMinExposure) {
    const long double return_rate = static_cast<long double>(realized_delta) / static_cast<long double>(exposure_before);
    const int64_t return_rate_1e6 = round_i64(return_rate * 1e6);
    agg.return_sum += return_rate_1e6;
    const int64_t delta_t = (prev_block > 0) ? std::max<int64_t>(0, current_block - prev_block) : 0;
    if (agg.first_block == 0) {
      agg.first_block = current_block;
    }
    agg.return_tw_sum += static_cast<__int128>(return_rate_1e6) * delta_t;
    const long double sq = static_cast<long double>(return_rate_1e6) * static_cast<long double>(return_rate_1e6);
    agg.return_sq_tw_sum += static_cast<__int128>(sq + 0.5L) * delta_t;
  }
}
```

```cpp
double calc_sharpe_from_returns(int64_t return_sum, __int128 return_tw_sum, __int128 return_sq_tw_sum, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double sum = static_cast<long double>(return_sum);
  const long double tw_sum = i128_to_long_double(return_tw_sum);
  const long double sq_tw_sum = i128_to_long_double(return_sq_tw_sum);

  const long double r_bar = sum / T;
  const long double tw_avg = tw_sum / T;
  const long double sq_tw_avg = sq_tw_sum / T;
  const long double variance = sq_tw_avg - 2.0L * r_bar * tw_avg + r_bar * r_bar;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(r_bar / std::sqrt(static_cast<double>(variance)));
}
```

---

**User**

我建议：
1 → 选 A（RMS cap）
2 → 选 A（按 block 合并）

下面是原因，这个组合是最稳、最接近真实 Sharpe 定义的。

✅ Q1 有效资本 cap_i 用哪种？
✔ 推荐 A
cap_i = sqrt((exp_{i-1}^2 + exp_i^2)/2)
为什么 RMS 更好

当 exposure 剧烈变化时：

情况	mean	rms
+100 → -100	100	100
0 → 100	50	70.7
1 → 1000	500	707
高频跳变	偏小	更稳定

Sharpe 本质是 risk-adjusted return，
risk 应该和 平方相关，不是线性。

RMS 相当于用

risk ~ exposure^2

这更符合：

波动率定义

VaR / variance

Sharpe推导

continuous-time模型

所以实盘 / HFT / CTA 更常用 RMS。

✔ 推荐 A

✅ Q2 Δt = 0 怎么处理？

你给的两个选项：

A 按 block 合并（推荐）

同一 block 内先合并 → block 末尾采样 → 再算 block 间收益

这是更正确的时间加权。

原因：

Sharpe 的时间加权定义是

μ = ∫ r(t) dt / T
σ² = ∫ (r(t)-μ)² dt / T

如果 Δt = 0：

权重 = 0

事件不应该影响时间统计。

如果直接算：

u = r / dt

会爆。

所以正确做法是：

先在同一时间点聚合 → 再算时间区间

这等价于 continuous-time Sharpe。

✔ 推荐 A

✅ 为什么 B 不好

B = 保留事件级采样

问题：

Δt=0 → 除零

高频事件会放大方差

Sharpe依赖撮合频率

不可比较

在真实交易评估里一般不会用。

✅ 最终推荐组合
选择	推荐
cap_i	A (RMS)
Δt=0	A (block merge)

---

**Cursor**

口径已经定下来了。我再补读一轮和持久化字段有关的代码，确认要不要改 `feature_tensor_state` 的列定义，还是只改统计语义就够。

我补到一个很重要的实现点：这次不只是改公式，还是一次“历史特征口径变更”。因为 `feature_tensor_state` 里已经落库了旧语义的 Sharpe 中间量，如果不重算历史窗口，前后会混在一起，结果还是会脏。

我已经确认当前代码里没有现成的“重算 feature_tensor”通道，只有正常增量同步。所以计划里我会把“改完后需要重刷 stage3 历史特征”单独列出来，不然只会把新旧语义混在一起。

我在推实现时又碰到一个真正的工程坑：你要求“按 block 合并”，那就意味着同一 block 的最后一个采样点才是有效点。这个在增量同步里不是一句换公式能解决的，因为 batch 可能正好截在 block 中间。

还有一个必须先定的工程口径：历史数据怎么迁移。  
因为 `feature_tensor_state` 里旧 Sharpe 中间量已经持久化了，而这次按 block 合并后，需要额外保存新的 Sharpe 锚点状态；如果不重建历史，旧行一定会污染新结果。

---

**User**

删掉所有向后兼容， 删掉冗余代码， 只保留最优实现， 我会重建数据局
同步更新README和代码

---

**Cursor**

计划已经收敛了：不做兼容层，直接把旧的 Sharpe 路径整套替换掉，并按你定的 `RMS cap + block merge` 落地。接下来我把具体实施计划发给你确认。

---

**User**

Implement the plan as specified, it is attached for your reference. Do NOT edit the plan file itself.

To-do's from the plan have already been created. Do not create them again. Mark them as in_progress as you work, starting with the first one. Don't stop until you have completed all the to-dos.

Implement the plan as specified, it is attached for your reference. Do NOT edit the plan file itself.

To-do's from the plan have already been created. Do not create them again. Mark them as in_progress as you work, starting with the first one. Don't stop until you have completed all the to-dos.

---

**Cursor**

我开始按计划落地，先把第一个待办切到 `in_progress`，同时把 Sharpe 相关结构和 runtime 关键片段再完整读一遍，准备直接替换旧统计路径。

我在梳理一个关键状态点：为了按 `block` 合并又支持增量同步，除了改公式，还得给每个 `(user, tag)` 预加载“批次开始前最后一个 block 采样点”。我先确认现有临时表和查询里有没有能直接复用的 pair 级预加载。

我找到现成可复用的 pair 级预加载入口了：`tmp_touched_user_tags` 已经会把 `(user, tag)` 和 `(user, ALL)` 收集出来，后面可以直接在这里补“批次开始前最后一个 Sharpe 采样点”的 seed，而不用再新造一套索引。

我先把底层数学和状态结构改掉：去掉旧的 `return_tw/sq_tw`，换成 `sum_r / sum_r2_over_dt / time_sum`，并补上 block-merge 需要的 Sharpe 锚点字段。

```cpp
#pragma once

#include "../stage2/stage2_types.hpp"

#include <cstdint>

namespace stage3::feature_comp {

struct TokenSnapshot {
  double pos = 0.0;
  double cost = 0.0;
  double lp = 0.0;
  double entry_block = 0.0;
};

struct BucketAggState {
  __int128 exposure_tw_sum = 0;
  int64_t volume_sum = 0;
  __int128 holding_period_exp_tw_sum = 0;
  int64_t token_count_tw_sum = 0;
  int64_t time_weight_sum = 0;
  // Sharpe 原子统计：
  // r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
  // sum_r = Σr_i (1e6 标度)
  // sum_r2_over_dt = Σ(r_i² / Δt_i) (1e12 标度)
  // time_sum = ΣΔt_i
  int64_t sharpe_sum_r = 0;
  __int128 sharpe_sum_r2_over_dt = 0;
  int64_t sharpe_time_sum = 0;
  // Sharpe 续算锚点：
  // prev_* 是上一个已完成 block 采样点
  // pending_* 是当前未完成 block 的最新采样点
  // pending block / exposure 复用 last_block / last_exposure
  int64_t sharpe_prev_block = 0;
  int64_t sharpe_prev_pnl = 0;
  int64_t sharpe_prev_exposure = 0;
  int64_t sharpe_pending_pnl = 0;
  int64_t last_sort_key = 0;
  int64_t last_block = 0;
  int64_t last_exposure = 0;
  __int128 last_holding_exp = 0;
  int64_t last_token_count = 0;
  bool has_tail = false;
};

int64_t round_i64(double v);
int64_t sort_key_to_block(int64_t sort_key, int64_t sort_key_scale);
int64_t sort_key_to_block_bucket(int64_t sort_key, int64_t sort_key_scale, int64_t block_bucket_size);
int64_t bucket_end_block(int64_t block_bucket, int64_t block_bucket_size);

bool is_volume_event(stage2::EventType ty);
double calc_unrealized_pnl(const TokenSnapshot &st, double pos_epsilon);
int64_t calc_exposure_1e6(const TokenSnapshot &st, double pos_epsilon);
int64_t calc_holding_period_blocks(int64_t current_block, const TokenSnapshot &st, double pos_epsilon);
int64_t calc_volume_1e6(stage2::EventType ty, int64_t amount, int64_t price_1e6);

void update_tail_window(BucketAggState &agg,
                        int64_t block_bucket,
                        int64_t current_block,
                        int64_t current_exposure,
                        __int128 current_holding_exp,
                        int64_t current_token_count,
                        int64_t block_bucket_size);

void accumulate_sharpe_interval(BucketAggState &agg,
                                int64_t pnl_delta,
                                int64_t exposure_before,
                                int64_t exposure_after,
                                int64_t delta_t);

} // namespace stage3::feature_comp
```

```cpp
#include "stage3_comp_feat.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace stage3::feature_comp {
namespace {

int64_t narrow_i64(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

__int128 linear_series_i128(__int128 base, int64_t slope, int64_t len) {
  assert(base >= 0);
  assert(slope >= 0);
  assert(len >= 0);
  const __int128 L = static_cast<__int128>(len);
  return static_cast<__int128>(base) * L + static_cast<__int128>(slope) * L * (L - 1) / 2;
}

int64_t calc_rms_cap_1e6(int64_t exposure_before, int64_t exposure_after) {
  assert(exposure_before >= 0);
  assert(exposure_after >= 0);
  const long double before = static_cast<long double>(exposure_before);
  const long double after = static_cast<long double>(exposure_after);
  const long double rms = std::sqrt((before * before + after * after) / 2.0L);
  return round_i64(static_cast<double>(rms));
}

} // namespace

int64_t round_i64(double v) { return static_cast<int64_t>(std::llround(v)); }

int64_t sort_key_to_block(int64_t sort_key, int64_t sort_key_scale) {
  assert(sort_key >= 0);
  assert(sort_key_scale > 0);
  return sort_key / sort_key_scale;
}

int64_t sort_key_to_block_bucket(int64_t sort_key, int64_t sort_key_scale, int64_t block_bucket_size) {
  assert(block_bucket_size > 0);
  return sort_key_to_block(sort_key, sort_key_scale) / block_bucket_size;
}

int64_t bucket_end_block(int64_t block_bucket, int64_t block_bucket_size) {
  assert(block_bucket >= 0);
  assert(block_bucket_size > 0);
  return (block_bucket + 1) * block_bucket_size;
}

bool is_volume_event(stage2::EventType ty) {
  return ty == stage2::EventType::OrderBuy || ty == stage2::EventType::OrderSell ||
         ty == stage2::EventType::FPMMBuy || ty == stage2::EventType::FPMMSell ||
         ty == stage2::EventType::TransferInNegRisk || ty == stage2::EventType::TransferInOther ||
         ty == stage2::EventType::TransferInNonPoly || ty == stage2::EventType::TransferOutNegRisk ||
         ty == stage2::EventType::TransferOutOther || ty == stage2::EventType::TransferOutNonPoly ||
         ty == stage2::EventType::Redemption || ty == stage2::EventType::RedemptionNonPoly;
}

double calc_unrealized_pnl(const TokenSnapshot &st, double pos_epsilon) {
  if (std::abs(st.pos) <= pos_epsilon || st.lp <= 0.0) {
    return 0.0;
  }
  const double mtm = st.pos * st.lp / 1e6;
  return mtm - st.cost;
}

int64_t calc_exposure_1e6(const TokenSnapshot &st, double pos_epsilon) {
  if (std::abs(st.pos) <= pos_epsilon || st.lp <= 0.0) {
    return 0;
  }
  return round_i64(std::abs(st.pos) * st.lp / 1e6);
}

int64_t calc_holding_period_blocks(int64_t current_block, const TokenSnapshot &st, double pos_epsilon) {
  if (std::abs(st.pos) <= pos_epsilon || st.entry_block <= 0.0) {
    return 0;
  }
  const double hp = std::max(0.0, static_cast<double>(current_block) - st.entry_block);
  return round_i64(hp);
}

int64_t calc_volume_1e6(stage2::EventType ty, int64_t amount, int64_t price_1e6) {
  if (!is_volume_event(ty)) {
    return 0;
  }
  const double qty = std::abs(static_cast<double>(amount));
  const double px = std::abs(static_cast<double>(price_1e6)) / 1e6;
  return round_i64(qty * px);
}

void update_tail_window(BucketAggState &agg,
                        int64_t block_bucket,
                        int64_t current_block,
                        int64_t current_exposure,
                        __int128 current_holding_exp,
                        int64_t current_token_count,
                        int64_t block_bucket_size) {
  assert(block_bucket >= 0);
  assert(block_bucket_size > 0);
  assert(current_exposure >= 0);
  assert(current_holding_exp >= 0);
  assert(current_token_count >= 0);
  if (current_exposure == 0) {
    assert(current_holding_exp == 0);
  }
  assert(current_block >= block_bucket * block_bucket_size);
  assert(current_block <= bucket_end_block(block_bucket, block_bucket_size));
  const int64_t end_block = bucket_end_block(block_bucket, block_bucket_size);
  if (agg.has_tail) {
    assert(agg.last_block >= block_bucket * block_bucket_size);
    assert(agg.last_block <= end_block);
    assert(agg.last_exposure >= 0);
    assert(agg.last_holding_exp >= 0);
    assert(agg.last_token_count >= 0);
    if (agg.last_exposure == 0) {
      assert(agg.last_holding_exp == 0);
    }

    const int64_t old_tail = std::max<int64_t>(0, end_block - agg.last_block);
    agg.exposure_tw_sum -= static_cast<__int128>(agg.last_exposure) * old_tail;
    agg.holding_period_exp_tw_sum -= linear_series_i128(agg.last_holding_exp, agg.last_exposure, old_tail);
    agg.token_count_tw_sum = narrow_i64(
        static_cast<__int128>(agg.token_count_tw_sum) - static_cast<__int128>(agg.last_token_count) * old_tail);
    agg.time_weight_sum = narrow_i64(static_cast<__int128>(agg.time_weight_sum) - old_tail);

    const int64_t delta_blocks = current_block - agg.last_block;
    assert(delta_blocks >= 0);
    agg.exposure_tw_sum += static_cast<__int128>(agg.last_exposure) * delta_blocks;
    agg.holding_period_exp_tw_sum += linear_series_i128(agg.last_holding_exp, agg.last_exposure, delta_blocks);
    agg.token_count_tw_sum = narrow_i64(
        static_cast<__int128>(agg.token_count_tw_sum) + static_cast<__int128>(agg.last_token_count) * delta_blocks);
    agg.time_weight_sum = narrow_i64(static_cast<__int128>(agg.time_weight_sum) + delta_blocks);
  }

  const int64_t new_tail = std::max<int64_t>(0, end_block - current_block);
  agg.exposure_tw_sum += static_cast<__int128>(current_exposure) * new_tail;
  agg.holding_period_exp_tw_sum += linear_series_i128(current_holding_exp, current_exposure, new_tail);
  agg.token_count_tw_sum = narrow_i64(
      static_cast<__int128>(agg.token_count_tw_sum) + static_cast<__int128>(current_token_count) * new_tail);
  agg.time_weight_sum = narrow_i64(static_cast<__int128>(agg.time_weight_sum) + new_tail);

  agg.last_block = current_block;
  agg.last_exposure = current_exposure;
  agg.last_holding_exp = current_holding_exp;
  agg.last_token_count = current_token_count;
  agg.has_tail = true;
}

void accumulate_sharpe_interval(BucketAggState &agg,
                                int64_t pnl_delta,
                                int64_t exposure_before,
                                int64_t exposure_after,
                                int64_t delta_t) {
  assert(delta_t > 0);
  constexpr int64_t kMinCap = 1000; // 0.001 USD
  const int64_t cap = calc_rms_cap_1e6(exposure_before, exposure_after);
  if (cap < kMinCap) {
    return;
  }

  const long double return_rate =
      static_cast<long double>(pnl_delta) / static_cast<long double>(cap);
  const int64_t return_rate_1e6 = round_i64(static_cast<double>(return_rate * 1e6L));
  agg.sharpe_sum_r = narrow_i64(static_cast<__int128>(agg.sharpe_sum_r) + return_rate_1e6);

  const long double sq_over_dt =
      static_cast<long double>(return_rate_1e6) * static_cast<long double>(return_rate_1e6) /
      static_cast<long double>(delta_t);
  agg.sharpe_sum_r2_over_dt += static_cast<__int128>(sq_over_dt + 0.5L);
  agg.sharpe_time_sum =
      narrow_i64(static_cast<__int128>(agg.sharpe_time_sum) + static_cast<__int128>(delta_t));
}

} // namespace stage3::feature_comp
```

结构层面的第一步已经开始改了。现在我在替换 `feature_tensor_state` 的列定义和 runtime 读取路径，把旧的 Sharpe 中间量彻底摘掉，避免新旧语义混用。

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r̄ = Σr_i / T (单位时间收益率)
// σ² = Σ(r_i - r̄)² * Δt_i / T = Σ(r_i² * Δt_i)/T - 2*r̄*Σ(r_i * Δt_i)/T + r̄²
//    = sq_tw_avg - 2*r̄*tw_avg + r̄²
// sharpe = r̄ / σ
double calc_sharpe_from_returns(int64_t return_sum, __int128 return_tw_sum, __int128 return_sq_tw_sum, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double sum = static_cast<long double>(return_sum);
  const long double tw_sum = i128_to_long_double(return_tw_sum);
  const long double sq_tw_sum = i128_to_long_double(return_sq_tw_sum);

  // r̄ = Σr_i / T
  const long double r_bar = sum / T;
  // tw_avg = Σ(r_i * Δt_i) / T
  const long double tw_avg = tw_sum / T;
  // sq_tw_avg = Σ(r_i² * Δt_i) / T
  const long double sq_tw_avg = sq_tw_sum / T;

  // σ² = sq_tw_avg - 2*r̄*tw_avg + r̄²
  const long double variance = sq_tw_avg - 2.0L * r_bar * tw_avg + r_bar * r_bar;
  if (variance <= 0.0L) {
    return 0.0;
  }

  // sharpe = r̄ / σ (r̄ 以 1e6 为单位，σ 也以 1e6 为单位，结果无量纲)
  return static_cast<double>(r_bar / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_return_sum = 0;
  __int128 ps_return_tw_sum = 0;
  __int128 ps_return_sq_tw_sum = 0;
  int64_t ps_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] =
          UserTagRuntimePairState{last_token_count, last_exposure, exposure_entry_sum};
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.return_sum = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.return_tw_sum = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.return_sq_tw_sum = hugeint_to_i128(feature_state_result->GetValue(15, i).GetValue<duckdb::hugeint_t>());
      agg.first_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        const UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        // exposure_before: 事件发生前的暴露额（rt.exposure 已被 apply_runtime_contrib_delta 更新）
        const int64_t exposure_before = std::max<int64_t>(0, rt.exposure - exposure_delta);

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        // 保存 prev_block 用于 delta_t 计算（必须在 update_tail_window 之前）
        const int64_t prev_block = agg.has_tail ? agg.last_block : 0;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        feature_comp::accumulate_event_delta(agg, realized_delta, exposure_before, volume, row.sort_key, row_block, prev_block);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_return_sum_10w, f.ps_return_tw_sum_10w, f.ps_return_sq_tw_sum_10w, f.ps_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          hugeint_to_i128(prefix_sum_result->GetValue(9, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(10, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t return_sum_10w = 0;
        __int128 return_tw_sum_10w = 0;
        __int128 return_sq_tw_sum_10w = 0;
        int64_t time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_return_sum = 0;
        __int128 ps_return_tw_sum = 0;
        __int128 ps_return_sq_tw_sum = 0;
        int64_t ps_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // 计算 bucket 内时间跨度 T = last_block - first_block
        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
                                         ? (agg.last_block - agg.first_block)
                                         : 0;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_return_sum = prev.ps_return_sum;
          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
          prev_ps_time_sum = prev.ps_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_return_sum = prev_rec->ps_return_sum;
            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
            prev_ps_time_sum = prev_rec->ps_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t return_sum = 0, time_sum = 0;
            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.return_sum = row.ps_return_sum;
            result.return_tw_sum = row.ps_return_tw_sum;
            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
            result.time_sum = row.ps_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.return_sum = rec->ps_return_sum;
            result.return_tw_sum = rec->ps_return_tw_sum;
            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
            result.time_sum = rec->ps_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
        ap.Append(agg.first_block);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
        ap.Append(ps_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.return_sum,
            agg.return_tw_sum,
            agg.return_sq_tw_sum,
            time_sum_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_return_sum,
            ps_return_tw_sum,
            ps_return_sq_tw_sum,
            ps_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// sharpe = μ / σ
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(mean / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] =
          UserTagRuntimePairState{last_token_count, last_exposure, exposure_entry_sum};
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.return_sum = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.return_tw_sum = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.return_sq_tw_sum = hugeint_to_i128(feature_state_result->GetValue(15, i).GetValue<duckdb::hugeint_t>());
      agg.first_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        const UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        // exposure_before: 事件发生前的暴露额（rt.exposure 已被 apply_runtime_contrib_delta 更新）
        const int64_t exposure_before = std::max<int64_t>(0, rt.exposure - exposure_delta);

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        // 保存 prev_block 用于 delta_t 计算（必须在 update_tail_window 之前）
        const int64_t prev_block = agg.has_tail ? agg.last_block : 0;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        feature_comp::accumulate_event_delta(agg, realized_delta, exposure_before, volume, row.sort_key, row_block, prev_block);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_return_sum_10w, f.ps_return_tw_sum_10w, f.ps_return_sq_tw_sum_10w, f.ps_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          hugeint_to_i128(prefix_sum_result->GetValue(9, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(10, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t return_sum_10w = 0;
        __int128 return_tw_sum_10w = 0;
        __int128 return_sq_tw_sum_10w = 0;
        int64_t time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_return_sum = 0;
        __int128 ps_return_tw_sum = 0;
        __int128 ps_return_sq_tw_sum = 0;
        int64_t ps_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // 计算 bucket 内时间跨度 T = last_block - first_block
        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
                                         ? (agg.last_block - agg.first_block)
                                         : 0;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_return_sum = prev.ps_return_sum;
          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
          prev_ps_time_sum = prev.ps_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_return_sum = prev_rec->ps_return_sum;
            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
            prev_ps_time_sum = prev_rec->ps_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t return_sum = 0, time_sum = 0;
            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.return_sum = row.ps_return_sum;
            result.return_tw_sum = row.ps_return_tw_sum;
            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
            result.time_sum = row.ps_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.return_sum = rec->ps_return_sum;
            result.return_tw_sum = rec->ps_return_tw_sum;
            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
            result.time_sum = rec->ps_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
        ap.Append(agg.first_block);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
        ap.Append(ps_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.return_sum,
            agg.return_tw_sum,
            agg.return_sq_tw_sum,
            time_sum_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_return_sum,
            ps_return_tw_sum,
            ps_return_sq_tw_sum,
            ps_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

---

**User**

继续

---

**Cursor**

我继续往下替换 runtime 主路径，重点是把 `per-event` Sharpe 改成 `per-block` 结算，并把旧字段引用一并扫掉，避免留下半旧半新的状态。

我已经把列名和公式入口改开了，现在开始补 runtime 的状态机本体：加 pair 级的 `prev/pending` 采样状态、seed 读取、以及 block 前进时的结算。

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// sharpe = μ / σ
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(mean / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
    bool sharpe_prev_valid = false;
    int64_t sharpe_prev_block = 0;
    int64_t sharpe_prev_pnl = 0;
    int64_t sharpe_prev_exposure = 0;
    bool sharpe_pending_valid = false;
    int64_t sharpe_pending_block = 0;
    int64_t sharpe_pending_pnl = 0;
    int64_t sharpe_pending_exposure = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w, "
        "x.sharpe_prev_block_10w, x.sharpe_prev_pnl_10w, x.sharpe_prev_exposure_10w, x.sharpe_pending_pnl_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      const int64_t sharpe_prev_block = runtime_seed_result->GetValue(6, i).GetValue<int64_t>();
      const int64_t sharpe_prev_pnl = runtime_seed_result->GetValue(7, i).GetValue<int64_t>();
      const int64_t sharpe_prev_exposure = runtime_seed_result->GetValue(8, i).GetValue<int64_t>();
      const int64_t sharpe_pending_pnl = runtime_seed_result->GetValue(9, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      if (tag_id == -1) {
        assert(sharpe_prev_block >= 0);
        assert(sharpe_prev_exposure >= 0);
        if (sharpe_prev_block > 0) {
          state.sharpe_prev_valid = true;
          state.sharpe_prev_block = sharpe_prev_block;
          state.sharpe_prev_pnl = sharpe_prev_pnl;
          state.sharpe_prev_exposure = sharpe_prev_exposure;
        }
        if (last_block > 0) {
          state.sharpe_pending_valid = true;
          state.sharpe_pending_block = last_block;
          state.sharpe_pending_pnl = sharpe_pending_pnl;
          state.sharpe_pending_exposure = last_exposure;
          const int64_t pending_bucket = last_block / kBlockBucketSize;
          bucket_agg_state_by_agg_key.try_emplace(AggKey{user_id, pending_bucket, -1}, BucketAggState{});
        }
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.return_sum = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.return_tw_sum = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.return_sq_tw_sum = hugeint_to_i128(feature_state_result->GetValue(15, i).GetValue<duckdb::hugeint_t>());
      agg.first_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        const UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        // exposure_before: 事件发生前的暴露额（rt.exposure 已被 apply_runtime_contrib_delta 更新）
        const int64_t exposure_before = std::max<int64_t>(0, rt.exposure - exposure_delta);

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        // 保存 prev_block 用于 delta_t 计算（必须在 update_tail_window 之前）
        const int64_t prev_block = agg.has_tail ? agg.last_block : 0;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        feature_comp::accumulate_event_delta(agg, realized_delta, exposure_before, volume, row.sort_key, row_block, prev_block);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_return_sum_10w, f.ps_return_tw_sum_10w, f.ps_return_sq_tw_sum_10w, f.ps_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          hugeint_to_i128(prefix_sum_result->GetValue(9, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(10, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t return_sum_10w = 0;
        __int128 return_tw_sum_10w = 0;
        __int128 return_sq_tw_sum_10w = 0;
        int64_t time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_return_sum = 0;
        __int128 ps_return_tw_sum = 0;
        __int128 ps_return_sq_tw_sum = 0;
        int64_t ps_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // 计算 bucket 内时间跨度 T = last_block - first_block
        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
                                         ? (agg.last_block - agg.first_block)
                                         : 0;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_return_sum = prev.ps_return_sum;
          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
          prev_ps_time_sum = prev.ps_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_return_sum = prev_rec->ps_return_sum;
            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
            prev_ps_time_sum = prev_rec->ps_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t return_sum = 0, time_sum = 0;
            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.return_sum = row.ps_return_sum;
            result.return_tw_sum = row.ps_return_tw_sum;
            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
            result.time_sum = row.ps_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.return_sum = rec->ps_return_sum;
            result.return_tw_sum = rec->ps_return_tw_sum;
            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
            result.time_sum = rec->ps_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
        ap.Append(agg.first_block);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
        ap.Append(ps_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.return_sum,
            agg.return_tw_sum,
            agg.return_sq_tw_sum,
            time_sum_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_return_sum,
            ps_return_tw_sum,
            ps_return_sq_tw_sum,
            ps_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

我已经把 pair 级 seed 扩到了新 Sharpe 锚点，并在 `latest ALL bucket` 上补了回写 key，这样前一批遗留的未闭合 block 区间可以在下一批被正确结算到旧 bucket。接下来我把 DB 行加载和事件循环里的 block-merge 逻辑接上。

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// sharpe = μ / σ
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(mean / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
    bool sharpe_prev_valid = false;
    int64_t sharpe_prev_block = 0;
    int64_t sharpe_prev_pnl = 0;
    int64_t sharpe_prev_exposure = 0;
    bool sharpe_pending_valid = false;
    int64_t sharpe_pending_block = 0;
    int64_t sharpe_pending_pnl = 0;
    int64_t sharpe_pending_exposure = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w, "
        "x.sharpe_prev_block_10w, x.sharpe_prev_pnl_10w, x.sharpe_prev_exposure_10w, x.sharpe_pending_pnl_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      const int64_t sharpe_prev_block = runtime_seed_result->GetValue(6, i).GetValue<int64_t>();
      const int64_t sharpe_prev_pnl = runtime_seed_result->GetValue(7, i).GetValue<int64_t>();
      const int64_t sharpe_prev_exposure = runtime_seed_result->GetValue(8, i).GetValue<int64_t>();
      const int64_t sharpe_pending_pnl = runtime_seed_result->GetValue(9, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      if (tag_id == -1) {
        assert(sharpe_prev_block >= 0);
        assert(sharpe_prev_exposure >= 0);
        if (sharpe_prev_block > 0) {
          state.sharpe_prev_valid = true;
          state.sharpe_prev_block = sharpe_prev_block;
          state.sharpe_prev_pnl = sharpe_prev_pnl;
          state.sharpe_prev_exposure = sharpe_prev_exposure;
        }
        if (last_block > 0) {
          state.sharpe_pending_valid = true;
          state.sharpe_pending_block = last_block;
          state.sharpe_pending_pnl = sharpe_pending_pnl;
          state.sharpe_pending_exposure = last_exposure;
          const int64_t pending_bucket = last_block / kBlockBucketSize;
          bucket_agg_state_by_agg_key.try_emplace(AggKey{user_id, pending_bucket, -1}, BucketAggState{});
        }
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_sum_r = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.sharpe_sum_r2_over_dt = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_time_sum = feature_state_result->GetValue(15, i).GetValue<int64_t>();
      agg.sharpe_prev_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.sharpe_prev_pnl = feature_state_result->GetValue(17, i).GetValue<int64_t>();
      agg.sharpe_prev_exposure = feature_state_result->GetValue(18, i).GetValue<int64_t>();
      agg.sharpe_pending_pnl = feature_state_result->GetValue(19, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        const UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        // exposure_before: 事件发生前的暴露额（rt.exposure 已被 apply_runtime_contrib_delta 更新）
        const int64_t exposure_before = std::max<int64_t>(0, rt.exposure - exposure_delta);

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        // 保存 prev_block 用于 delta_t 计算（必须在 update_tail_window 之前）
        const int64_t prev_block = agg.has_tail ? agg.last_block : 0;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        feature_comp::accumulate_event_delta(agg, realized_delta, exposure_before, volume, row.sort_key, row_block, prev_block);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_return_sum_10w, f.ps_return_tw_sum_10w, f.ps_return_sq_tw_sum_10w, f.ps_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          hugeint_to_i128(prefix_sum_result->GetValue(9, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(10, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t return_sum_10w = 0;
        __int128 return_tw_sum_10w = 0;
        __int128 return_sq_tw_sum_10w = 0;
        int64_t time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_return_sum = 0;
        __int128 ps_return_tw_sum = 0;
        __int128 ps_return_sq_tw_sum = 0;
        int64_t ps_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // 计算 bucket 内时间跨度 T = last_block - first_block
        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
                                         ? (agg.last_block - agg.first_block)
                                         : 0;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_return_sum = prev.ps_return_sum;
          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
          prev_ps_time_sum = prev.ps_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_return_sum = prev_rec->ps_return_sum;
            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
            prev_ps_time_sum = prev_rec->ps_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t return_sum = 0, time_sum = 0;
            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.return_sum = row.ps_return_sum;
            result.return_tw_sum = row.ps_return_tw_sum;
            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
            result.time_sum = row.ps_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.return_sum = rec->ps_return_sum;
            result.return_tw_sum = rec->ps_return_tw_sum;
            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
            result.time_sum = rec->ps_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
        ap.Append(agg.first_block);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
        ap.Append(ps_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.return_sum,
            agg.return_tw_sum,
            agg.return_sq_tw_sum,
            time_sum_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_return_sum,
            ps_return_tw_sum,
            ps_return_sq_tw_sum,
            ps_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// sharpe = μ / σ
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(mean / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
    bool sharpe_prev_valid = false;
    int64_t sharpe_prev_block = 0;
    int64_t sharpe_prev_pnl = 0;
    int64_t sharpe_prev_exposure = 0;
    bool sharpe_pending_valid = false;
    int64_t sharpe_pending_block = 0;
    int64_t sharpe_pending_pnl = 0;
    int64_t sharpe_pending_exposure = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w, "
        "x.sharpe_prev_block_10w, x.sharpe_prev_pnl_10w, x.sharpe_prev_exposure_10w, x.sharpe_pending_pnl_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      const int64_t sharpe_prev_block = runtime_seed_result->GetValue(6, i).GetValue<int64_t>();
      const int64_t sharpe_prev_pnl = runtime_seed_result->GetValue(7, i).GetValue<int64_t>();
      const int64_t sharpe_prev_exposure = runtime_seed_result->GetValue(8, i).GetValue<int64_t>();
      const int64_t sharpe_pending_pnl = runtime_seed_result->GetValue(9, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      if (tag_id == -1) {
        assert(sharpe_prev_block >= 0);
        assert(sharpe_prev_exposure >= 0);
        if (sharpe_prev_block > 0) {
          state.sharpe_prev_valid = true;
          state.sharpe_prev_block = sharpe_prev_block;
          state.sharpe_prev_pnl = sharpe_prev_pnl;
          state.sharpe_prev_exposure = sharpe_prev_exposure;
        }
        if (last_block > 0) {
          state.sharpe_pending_valid = true;
          state.sharpe_pending_block = last_block;
          state.sharpe_pending_pnl = sharpe_pending_pnl;
          state.sharpe_pending_exposure = last_exposure;
          const int64_t pending_bucket = last_block / kBlockBucketSize;
          bucket_agg_state_by_agg_key.try_emplace(AggKey{user_id, pending_bucket, -1}, BucketAggState{});
        }
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_sum_r = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.sharpe_sum_r2_over_dt = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_time_sum = feature_state_result->GetValue(15, i).GetValue<int64_t>();
      agg.sharpe_prev_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.sharpe_prev_pnl = feature_state_result->GetValue(17, i).GetValue<int64_t>();
      agg.sharpe_prev_exposure = feature_state_result->GetValue(18, i).GetValue<int64_t>();
      agg.sharpe_pending_pnl = feature_state_result->GetValue(19, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  auto sync_bucket_sharpe_anchor = [&](BucketAggState &agg, const UserTagRuntimePairState &rt) {
    if (rt.sharpe_prev_valid) {
      agg.sharpe_prev_block = rt.sharpe_prev_block;
      agg.sharpe_prev_pnl = rt.sharpe_prev_pnl;
      agg.sharpe_prev_exposure = rt.sharpe_prev_exposure;
    } else {
      agg.sharpe_prev_block = 0;
      agg.sharpe_prev_pnl = 0;
      agg.sharpe_prev_exposure = 0;
    }
    agg.sharpe_pending_pnl = rt.sharpe_pending_valid ? rt.sharpe_pending_pnl : 0;
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      const int64_t account_pnl_after =
          feature_comp::round_i64(realized_total_it->second + realized_delta + unrealized_total_it->second);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        // exposure_before: 事件发生前的暴露额（rt.exposure 已被 apply_runtime_contrib_delta 更新）
        const int64_t exposure_before = std::max<int64_t>(0, rt.exposure - exposure_delta);

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        agg.volume_sum += volume;
        agg.last_sort_key = row.sort_key;
        if (agg_tag_id != -1) {
          return;
        }
        assert(agg_key.tag_id == -1);
        const int64_t account_exposure_after = rt.exposure;
        if (!rt.sharpe_pending_valid) {
          rt.sharpe_pending_valid = true;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else if (row_block == rt.sharpe_pending_block) {
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else {
          assert(row_block > rt.sharpe_pending_block);
          const int64_t pending_bucket = rt.sharpe_pending_block / kBlockBucketSize;
          AggKey pending_key{row.user_id, pending_bucket, -1};
          auto pending_agg_it = bucket_agg_state_by_agg_key.find(pending_key);
          assert(pending_agg_it != bucket_agg_state_by_agg_key.end());
          if (rt.sharpe_prev_valid) {
            assert(rt.sharpe_pending_block > rt.sharpe_prev_block);
            feature_comp::accumulate_sharpe_interval(
                pending_agg_it->second,
                rt.sharpe_pending_pnl - rt.sharpe_prev_pnl,
                rt.sharpe_prev_exposure,
                rt.sharpe_pending_exposure,
                rt.sharpe_pending_block - rt.sharpe_prev_block);
          }
          rt.sharpe_prev_valid = true;
          rt.sharpe_prev_block = rt.sharpe_pending_block;
          rt.sharpe_prev_pnl = rt.sharpe_pending_pnl;
          rt.sharpe_prev_exposure = rt.sharpe_pending_exposure;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        }
        sync_bucket_sharpe_anchor(agg, rt);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_return_sum_10w, f.ps_return_tw_sum_10w, f.ps_return_sq_tw_sum_10w, f.ps_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          hugeint_to_i128(prefix_sum_result->GetValue(9, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(10, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t return_sum_10w = 0;
        __int128 return_tw_sum_10w = 0;
        __int128 return_sq_tw_sum_10w = 0;
        int64_t time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_return_sum = 0;
        __int128 ps_return_tw_sum = 0;
        __int128 ps_return_sq_tw_sum = 0;
        int64_t ps_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // 计算 bucket 内时间跨度 T = last_block - first_block
        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
                                         ? (agg.last_block - agg.first_block)
                                         : 0;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_return_sum = prev.ps_return_sum;
          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
          prev_ps_time_sum = prev.ps_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_return_sum = prev_rec->ps_return_sum;
            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
            prev_ps_time_sum = prev_rec->ps_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t return_sum = 0, time_sum = 0;
            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.return_sum = row.ps_return_sum;
            result.return_tw_sum = row.ps_return_tw_sum;
            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
            result.time_sum = row.ps_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.return_sum = rec->ps_return_sum;
            result.return_tw_sum = rec->ps_return_tw_sum;
            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
            result.time_sum = rec->ps_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
        ap.Append(agg.first_block);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
        ap.Append(ps_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.return_sum,
            agg.return_tw_sum,
            agg.return_sq_tw_sum,
            time_sum_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_return_sum,
            ps_return_tw_sum,
            ps_return_sq_tw_sum,
            ps_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

核心 block-merge 已经接进事件循环了：现在同一 `block` 只会更新 pending sample，只有遇到更大 `block` 时才会把上一个 block 结算成一个 Sharpe 区间。  
我再清一遍残留的旧变量/旧前缀统计引用，然后把窗口投影和 schema 同步到新字段。

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// sharpe = μ / σ
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(mean / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
    bool sharpe_prev_valid = false;
    int64_t sharpe_prev_block = 0;
    int64_t sharpe_prev_pnl = 0;
    int64_t sharpe_prev_exposure = 0;
    bool sharpe_pending_valid = false;
    int64_t sharpe_pending_block = 0;
    int64_t sharpe_pending_pnl = 0;
    int64_t sharpe_pending_exposure = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w, "
        "x.sharpe_prev_block_10w, x.sharpe_prev_pnl_10w, x.sharpe_prev_exposure_10w, x.sharpe_pending_pnl_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      const int64_t sharpe_prev_block = runtime_seed_result->GetValue(6, i).GetValue<int64_t>();
      const int64_t sharpe_prev_pnl = runtime_seed_result->GetValue(7, i).GetValue<int64_t>();
      const int64_t sharpe_prev_exposure = runtime_seed_result->GetValue(8, i).GetValue<int64_t>();
      const int64_t sharpe_pending_pnl = runtime_seed_result->GetValue(9, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      if (tag_id == -1) {
        assert(sharpe_prev_block >= 0);
        assert(sharpe_prev_exposure >= 0);
        if (sharpe_prev_block > 0) {
          state.sharpe_prev_valid = true;
          state.sharpe_prev_block = sharpe_prev_block;
          state.sharpe_prev_pnl = sharpe_prev_pnl;
          state.sharpe_prev_exposure = sharpe_prev_exposure;
        }
        if (last_block > 0) {
          state.sharpe_pending_valid = true;
          state.sharpe_pending_block = last_block;
          state.sharpe_pending_pnl = sharpe_pending_pnl;
          state.sharpe_pending_exposure = last_exposure;
          const int64_t pending_bucket = last_block / kBlockBucketSize;
          bucket_agg_state_by_agg_key.try_emplace(AggKey{user_id, pending_bucket, -1}, BucketAggState{});
        }
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_sum_r = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.sharpe_sum_r2_over_dt = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_time_sum = feature_state_result->GetValue(15, i).GetValue<int64_t>();
      agg.sharpe_prev_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.sharpe_prev_pnl = feature_state_result->GetValue(17, i).GetValue<int64_t>();
      agg.sharpe_prev_exposure = feature_state_result->GetValue(18, i).GetValue<int64_t>();
      agg.sharpe_pending_pnl = feature_state_result->GetValue(19, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  auto sync_bucket_sharpe_anchor = [&](BucketAggState &agg, const UserTagRuntimePairState &rt) {
    if (rt.sharpe_prev_valid) {
      agg.sharpe_prev_block = rt.sharpe_prev_block;
      agg.sharpe_prev_pnl = rt.sharpe_prev_pnl;
      agg.sharpe_prev_exposure = rt.sharpe_prev_exposure;
    } else {
      agg.sharpe_prev_block = 0;
      agg.sharpe_prev_pnl = 0;
      agg.sharpe_prev_exposure = 0;
    }
    agg.sharpe_pending_pnl = rt.sharpe_pending_valid ? rt.sharpe_pending_pnl : 0;
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      const int64_t account_pnl_after =
          feature_comp::round_i64(realized_total_it->second + realized_delta + unrealized_total_it->second);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        agg.volume_sum += volume;
        agg.last_sort_key = row.sort_key;
        if (agg_tag_id != -1) {
          return;
        }
        assert(agg_key.tag_id == -1);
        const int64_t account_exposure_after = rt.exposure;
        if (!rt.sharpe_pending_valid) {
          rt.sharpe_pending_valid = true;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else if (row_block == rt.sharpe_pending_block) {
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else {
          assert(row_block > rt.sharpe_pending_block);
          const int64_t pending_bucket = rt.sharpe_pending_block / kBlockBucketSize;
          AggKey pending_key{row.user_id, pending_bucket, -1};
          auto pending_agg_it = bucket_agg_state_by_agg_key.find(pending_key);
          assert(pending_agg_it != bucket_agg_state_by_agg_key.end());
          if (rt.sharpe_prev_valid) {
            assert(rt.sharpe_pending_block > rt.sharpe_prev_block);
            feature_comp::accumulate_sharpe_interval(
                pending_agg_it->second,
                rt.sharpe_pending_pnl - rt.sharpe_prev_pnl,
                rt.sharpe_prev_exposure,
                rt.sharpe_pending_exposure,
                rt.sharpe_pending_block - rt.sharpe_prev_block);
          }
          rt.sharpe_prev_valid = true;
          rt.sharpe_prev_block = rt.sharpe_pending_block;
          rt.sharpe_prev_pnl = rt.sharpe_pending_pnl;
          rt.sharpe_prev_exposure = rt.sharpe_pending_exposure;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        }
        sync_bucket_sharpe_anchor(agg, rt);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_return_sum_10w, f.ps_return_tw_sum_10w, f.ps_return_sq_tw_sum_10w, f.ps_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          hugeint_to_i128(prefix_sum_result->GetValue(9, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(10, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t return_sum_10w = 0;
        __int128 return_tw_sum_10w = 0;
        __int128 return_sq_tw_sum_10w = 0;
        int64_t time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_return_sum = 0;
        __int128 ps_return_tw_sum = 0;
        __int128 ps_return_sq_tw_sum = 0;
        int64_t ps_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // 计算 bucket 内时间跨度 T = last_block - first_block
        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
                                         ? (agg.last_block - agg.first_block)
                                         : 0;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_return_sum = prev.ps_return_sum;
          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
          prev_ps_time_sum = prev.ps_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_return_sum = prev_rec->ps_return_sum;
            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
            prev_ps_time_sum = prev_rec->ps_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t return_sum = 0, time_sum = 0;
            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.return_sum = row.ps_return_sum;
            result.return_tw_sum = row.ps_return_tw_sum;
            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
            result.time_sum = row.ps_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.return_sum = rec->ps_return_sum;
            result.return_tw_sum = rec->ps_return_tw_sum;
            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
            result.time_sum = rec->ps_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
        ap.Append(agg.first_block);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
        ap.Append(ps_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.return_sum,
            agg.return_tw_sum,
            agg.return_sq_tw_sum,
            time_sum_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_return_sum,
            ps_return_tw_sum,
            ps_return_sq_tw_sum,
            ps_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// sharpe = μ / σ
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(mean / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
    bool sharpe_prev_valid = false;
    int64_t sharpe_prev_block = 0;
    int64_t sharpe_prev_pnl = 0;
    int64_t sharpe_prev_exposure = 0;
    bool sharpe_pending_valid = false;
    int64_t sharpe_pending_block = 0;
    int64_t sharpe_pending_pnl = 0;
    int64_t sharpe_pending_exposure = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w, "
        "x.sharpe_prev_block_10w, x.sharpe_prev_pnl_10w, x.sharpe_prev_exposure_10w, x.sharpe_pending_pnl_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      const int64_t sharpe_prev_block = runtime_seed_result->GetValue(6, i).GetValue<int64_t>();
      const int64_t sharpe_prev_pnl = runtime_seed_result->GetValue(7, i).GetValue<int64_t>();
      const int64_t sharpe_prev_exposure = runtime_seed_result->GetValue(8, i).GetValue<int64_t>();
      const int64_t sharpe_pending_pnl = runtime_seed_result->GetValue(9, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      if (tag_id == -1) {
        assert(sharpe_prev_block >= 0);
        assert(sharpe_prev_exposure >= 0);
        if (sharpe_prev_block > 0) {
          state.sharpe_prev_valid = true;
          state.sharpe_prev_block = sharpe_prev_block;
          state.sharpe_prev_pnl = sharpe_prev_pnl;
          state.sharpe_prev_exposure = sharpe_prev_exposure;
        }
        if (last_block > 0) {
          state.sharpe_pending_valid = true;
          state.sharpe_pending_block = last_block;
          state.sharpe_pending_pnl = sharpe_pending_pnl;
          state.sharpe_pending_exposure = last_exposure;
          const int64_t pending_bucket = last_block / kBlockBucketSize;
          bucket_agg_state_by_agg_key.try_emplace(AggKey{user_id, pending_bucket, -1}, BucketAggState{});
        }
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_sum_r = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.sharpe_sum_r2_over_dt = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_time_sum = feature_state_result->GetValue(15, i).GetValue<int64_t>();
      agg.sharpe_prev_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.sharpe_prev_pnl = feature_state_result->GetValue(17, i).GetValue<int64_t>();
      agg.sharpe_prev_exposure = feature_state_result->GetValue(18, i).GetValue<int64_t>();
      agg.sharpe_pending_pnl = feature_state_result->GetValue(19, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  auto sync_bucket_sharpe_anchor = [&](BucketAggState &agg, const UserTagRuntimePairState &rt) {
    if (rt.sharpe_prev_valid) {
      agg.sharpe_prev_block = rt.sharpe_prev_block;
      agg.sharpe_prev_pnl = rt.sharpe_prev_pnl;
      agg.sharpe_prev_exposure = rt.sharpe_prev_exposure;
    } else {
      agg.sharpe_prev_block = 0;
      agg.sharpe_prev_pnl = 0;
      agg.sharpe_prev_exposure = 0;
    }
    agg.sharpe_pending_pnl = rt.sharpe_pending_valid ? rt.sharpe_pending_pnl : 0;
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      const int64_t account_pnl_after =
          feature_comp::round_i64(realized_total_it->second + realized_delta + unrealized_total_it->second);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        agg.volume_sum += volume;
        agg.last_sort_key = row.sort_key;
        if (agg_tag_id != -1) {
          return;
        }
        assert(agg_key.tag_id == -1);
        const int64_t account_exposure_after = rt.exposure;
        if (!rt.sharpe_pending_valid) {
          rt.sharpe_pending_valid = true;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else if (row_block == rt.sharpe_pending_block) {
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else {
          assert(row_block > rt.sharpe_pending_block);
          const int64_t pending_bucket = rt.sharpe_pending_block / kBlockBucketSize;
          AggKey pending_key{row.user_id, pending_bucket, -1};
          auto pending_agg_it = bucket_agg_state_by_agg_key.find(pending_key);
          assert(pending_agg_it != bucket_agg_state_by_agg_key.end());
          if (rt.sharpe_prev_valid) {
            assert(rt.sharpe_pending_block > rt.sharpe_prev_block);
            feature_comp::accumulate_sharpe_interval(
                pending_agg_it->second,
                rt.sharpe_pending_pnl - rt.sharpe_prev_pnl,
                rt.sharpe_prev_exposure,
                rt.sharpe_pending_exposure,
                rt.sharpe_pending_block - rt.sharpe_prev_block);
          }
          rt.sharpe_prev_valid = true;
          rt.sharpe_prev_block = rt.sharpe_pending_block;
          rt.sharpe_prev_pnl = rt.sharpe_pending_pnl;
          rt.sharpe_prev_exposure = rt.sharpe_pending_exposure;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        }
        sync_bucket_sharpe_anchor(agg, rt);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_sharpe_sum_r_10w, f.ps_sharpe_sum_r2_over_dt_10w, f.ps_sharpe_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(9, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t return_sum_10w = 0;
        __int128 return_tw_sum_10w = 0;
        __int128 return_sq_tw_sum_10w = 0;
        int64_t time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_return_sum = 0;
        __int128 ps_return_tw_sum = 0;
        __int128 ps_return_sq_tw_sum = 0;
        int64_t ps_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // 计算 bucket 内时间跨度 T = last_block - first_block
        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
                                         ? (agg.last_block - agg.first_block)
                                         : 0;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_return_sum = prev.ps_return_sum;
          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
          prev_ps_time_sum = prev.ps_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_return_sum = prev_rec->ps_return_sum;
            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
            prev_ps_time_sum = prev_rec->ps_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t return_sum = 0, time_sum = 0;
            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.return_sum = row.ps_return_sum;
            result.return_tw_sum = row.ps_return_tw_sum;
            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
            result.time_sum = row.ps_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.return_sum = rec->ps_return_sum;
            result.return_tw_sum = rec->ps_return_tw_sum;
            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
            result.time_sum = rec->ps_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
        ap.Append(agg.first_block);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_return_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
        ap.Append(ps_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.return_sum,
            agg.return_tw_sum,
            agg.return_sq_tw_sum,
            time_sum_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_return_sum,
            ps_return_tw_sum,
            ps_return_sq_tw_sum,
            ps_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

```cpp
*** Begin Patch
*** Update File: /home/chuyin/work/poly/core-backend/src/stage3/stage3_sync_runtime.cpp
@@
       struct BucketPrefixOutput {
         int64_t block_bucket = 0;
         int64_t token_avg_10w = 0;
         int64_t exposure_avg_10w = 0;
         int64_t volume_10w = 0;
         int64_t holding_period_avg_10w = 0;
-        int64_t return_sum_10w = 0;
-        __int128 return_tw_sum_10w = 0;
-        __int128 return_sq_tw_sum_10w = 0;
-        int64_t time_sum_10w = 0;
+        int64_t sharpe_sum_r_10w = 0;
+        __int128 sharpe_sum_r2_over_dt_10w = 0;
+        int64_t sharpe_time_sum_10w = 0;
         // 前缀和（累计到当前 bucket）
         int64_t ps_token_avg = 0;
         int64_t ps_exposure_avg = 0;
         int64_t ps_volume = 0;
         int64_t ps_holding_period_avg = 0;
-        int64_t ps_return_sum = 0;
-        __int128 ps_return_tw_sum = 0;
-        __int128 ps_return_sq_tw_sum = 0;
-        int64_t ps_time_sum = 0;
+        int64_t ps_sharpe_sum_r = 0;
+        __int128 ps_sharpe_sum_r2_over_dt = 0;
+        int64_t ps_sharpe_time_sum = 0;
       };
@@
         const int64_t holding_period_avg_10w =
             (agg.exposure_tw_sum > 0)
                 ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                 i128_to_long_double(agg.exposure_tw_sum)))
                 : 0;
         const int64_t volume_10w = agg.volume_sum;
-        // 计算 bucket 内时间跨度 T = last_block - first_block
-        const int64_t time_sum_10w = (agg.first_block > 0 && agg.last_block > agg.first_block)
-                                         ? (agg.last_block - agg.first_block)
-                                         : 0;
         // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
         const double sharpe_10w = (key.tag_id == -1)
-                                      ? calc_sharpe_from_returns(agg.return_sum, agg.return_tw_sum, agg.return_sq_tw_sum, time_sum_10w)
+                                      ? calc_sharpe_from_returns(agg.sharpe_sum_r, agg.sharpe_sum_r2_over_dt, agg.sharpe_time_sum)
                                       : 0.0;
@@
         // Step 1: 计算当前 bucket 的前缀和
         // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
         int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
-        int64_t prev_ps_return_sum = 0, prev_ps_time_sum = 0;
-        __int128 prev_ps_return_tw_sum = 0, prev_ps_return_sq_tw_sum = 0;
+        int64_t prev_ps_sharpe_sum_r = 0, prev_ps_sharpe_time_sum = 0;
+        __int128 prev_ps_sharpe_sum_r2_over_dt = 0;
         if (!prefix_outputs.empty()) {
           // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
           const BucketPrefixOutput &prev = prefix_outputs.back();
           prev_ps_token = prev.ps_token_avg;
           prev_ps_exposure = prev.ps_exposure_avg;
           prev_ps_volume = prev.ps_volume;
           prev_ps_holding = prev.ps_holding_period_avg;
-          prev_ps_return_sum = prev.ps_return_sum;
-          prev_ps_return_tw_sum = prev.ps_return_tw_sum;
-          prev_ps_return_sq_tw_sum = prev.ps_return_sq_tw_sum;
-          prev_ps_time_sum = prev.ps_time_sum;
+          prev_ps_sharpe_sum_r = prev.ps_sharpe_sum_r;
+          prev_ps_sharpe_sum_r2_over_dt = prev.ps_sharpe_sum_r2_over_dt;
+          prev_ps_sharpe_time_sum = prev.ps_sharpe_time_sum;
         } else {
           // 从 DB 历史获取前一个 bucket 的前缀和
           const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
           if (prev_rec) {
             prev_ps_token = prev_rec->ps_token_avg;
             prev_ps_exposure = prev_rec->ps_exposure_avg;
             prev_ps_volume = prev_rec->ps_volume;
             prev_ps_holding = prev_rec->ps_holding_period_avg;
-            prev_ps_return_sum = prev_rec->ps_return_sum;
-            prev_ps_return_tw_sum = prev_rec->ps_return_tw_sum;
-            prev_ps_return_sq_tw_sum = prev_rec->ps_return_sq_tw_sum;
-            prev_ps_time_sum = prev_rec->ps_time_sum;
+            prev_ps_sharpe_sum_r = prev_rec->ps_sharpe_sum_r;
+            prev_ps_sharpe_sum_r2_over_dt = prev_rec->ps_sharpe_sum_r2_over_dt;
+            prev_ps_sharpe_time_sum = prev_rec->ps_sharpe_time_sum;
           }
         }
 
         // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
         const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
         const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
         const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
         const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
-        const int64_t ps_return_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_return_sum) + agg.return_sum);
-        const __int128 ps_return_tw_sum = prev_ps_return_tw_sum + agg.return_tw_sum;
-        const __int128 ps_return_sq_tw_sum = prev_ps_return_sq_tw_sum + agg.return_sq_tw_sum;
-        const int64_t ps_time_sum = i64_narrow_checked(static_cast<__int128>(prev_ps_time_sum) + time_sum_10w);
+        const int64_t ps_sharpe_sum_r = i64_narrow_checked(static_cast<__int128>(prev_ps_sharpe_sum_r) + agg.sharpe_sum_r);
+        const __int128 ps_sharpe_sum_r2_over_dt = prev_ps_sharpe_sum_r2_over_dt + agg.sharpe_sum_r2_over_dt;
+        const int64_t ps_sharpe_time_sum =
+            i64_narrow_checked(static_cast<__int128>(prev_ps_sharpe_time_sum) + agg.sharpe_time_sum);
@@
         auto get_boundary_ps = [&](int64_t target_bucket) {
           struct BoundaryPrefixSum {
             int64_t token = 0, exposure = 0, volume = 0, holding = 0;
-            int64_t return_sum = 0, time_sum = 0;
-            __int128 return_tw_sum = 0, return_sq_tw_sum = 0;
+            int64_t sharpe_sum_r = 0, sharpe_time_sum = 0;
+            __int128 sharpe_sum_r2_over_dt = 0;
           };
           BoundaryPrefixSum result{};
@@
             result.token = row.ps_token_avg;
             result.exposure = row.ps_exposure_avg;
             result.volume = row.ps_volume;
             result.holding = row.ps_holding_period_avg;
-            result.return_sum = row.ps_return_sum;
-            result.return_tw_sum = row.ps_return_tw_sum;
-            result.return_sq_tw_sum = row.ps_return_sq_tw_sum;
-            result.time_sum = row.ps_time_sum;
+            result.sharpe_sum_r = row.ps_sharpe_sum_r;
+            result.sharpe_sum_r2_over_dt = row.ps_sharpe_sum_r2_over_dt;
+            result.sharpe_time_sum = row.ps_sharpe_time_sum;
             return result;
           }
@@
             result.token = rec->ps_token_avg;
             result.exposure = rec->ps_exposure_avg;
             result.volume = rec->ps_volume;
             result.holding = rec->ps_holding_period_avg;
-            result.return_sum = rec->ps_return_sum;
-            result.return_tw_sum = rec->ps_return_tw_sum;
-            result.return_sq_tw_sum = rec->ps_return_sq_tw_sum;
-            result.time_sum = rec->ps_time_sum;
+            result.sharpe_sum_r = rec->ps_sharpe_sum_r;
+            result.sharpe_sum_r2_over_dt = rec->ps_sharpe_sum_r2_over_dt;
+            result.sharpe_time_sum = rec->ps_sharpe_time_sum;
           }
           return result;
         };
@@
         const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
         const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
         const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
         const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
         const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
-        const int64_t return_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_100.return_sum);
-        const int64_t return_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_return_sum) - boundary_1000.return_sum);
-        const __int128 return_tw_sum_100 = ps_return_tw_sum - boundary_100.return_tw_sum;
-        const __int128 return_tw_sum_1000 = ps_return_tw_sum - boundary_1000.return_tw_sum;
-        const __int128 return_sq_tw_sum_100 = ps_return_sq_tw_sum - boundary_100.return_sq_tw_sum;
-        const __int128 return_sq_tw_sum_1000 = ps_return_sq_tw_sum - boundary_1000.return_sq_tw_sum;
-        const int64_t time_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_100.time_sum);
-        const int64_t time_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_time_sum) - boundary_1000.time_sum);
+        const int64_t sharpe_sum_r_100 =
+            i64_narrow_checked(static_cast<__int128>(ps_sharpe_sum_r) - boundary_100.sharpe_sum_r);
+        const int64_t sharpe_sum_r_1000 =
+            i64_narrow_checked(static_cast<__int128>(ps_sharpe_sum_r) - boundary_1000.sharpe_sum_r);
+        const __int128 sharpe_sum_r2_over_dt_100 = ps_sharpe_sum_r2_over_dt - boundary_100.sharpe_sum_r2_over_dt;
+        const __int128 sharpe_sum_r2_over_dt_1000 = ps_sharpe_sum_r2_over_dt - boundary_1000.sharpe_sum_r2_over_dt;
+        const int64_t sharpe_time_sum_100 =
+            i64_narrow_checked(static_cast<__int128>(ps_sharpe_time_sum) - boundary_100.sharpe_time_sum);
+        const int64_t sharpe_time_sum_1000 =
+            i64_narrow_checked(static_cast<__int128>(ps_sharpe_time_sum) - boundary_1000.sharpe_time_sum);
@@
         const int64_t holding_period_avg_1000w =
             (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;
 
         // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
         const double sharpe_100w = (key.tag_id == -1)
-                                       ? calc_sharpe_from_returns(return_sum_100, return_tw_sum_100, return_sq_tw_sum_100, time_sum_100)
+                                       ? calc_sharpe_from_returns(sharpe_sum_r_100, sharpe_sum_r2_over_dt_100, sharpe_time_sum_100)
                                        : 0.0;
         const double sharpe_1000w = (key.tag_id == -1)
-                                        ? calc_sharpe_from_returns(return_sum_1000, return_tw_sum_1000, return_sq_tw_sum_1000, time_sum_1000)
+                                        ? calc_sharpe_from_returns(sharpe_sum_r_1000, sharpe_sum_r2_over_dt_1000, sharpe_time_sum_1000)
                                         : 0.0;
@@
         ap.Append(agg.exposure_tw_sum)));
         ap.Append(agg.volume_sum);
         ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
-        ap.Append(agg.return_sum);
-        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_tw_sum)));
-        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.return_sq_tw_sum)));
-        ap.Append(agg.first_block);
+        ap.Append(agg.sharpe_sum_r);
+        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.sharpe_sum_r2_over_dt)));
+        ap.Append(agg.sharpe_time_sum);
+        ap.Append(agg.sharpe_prev_block);
+        ap.Append(agg.sharpe_prev_pnl);
+        ap.Append(agg.sharpe_prev_exposure);
+        ap.Append(agg.sharpe_pending_pnl);
         ap.Append(token_avg_10w);
         ap.Append(exposure_avg_10w);
         ap.Append(volume_10w);
         ap.Append(holding_period_avg_10w);
         ap.Append(sharpe_10w);
@@
         ap.Append(ps_exposure_avg);
         ap.Append(ps_volume);
         ap.Append(ps_holding_period_avg);
-        ap.Append(ps_return_sum);
-        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_tw_sum)));
-        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_return_sq_tw_sum)));
-        ap.Append(ps_time_sum);
+        ap.Append(ps_sharpe_sum_r);
+        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_sharpe_sum_r2_over_dt)));
+        ap.Append(ps_sharpe_time_sum);
@@
             key.block_bucket,
             token_avg_10w,
             exposure_avg_10w,
             volume_10w,
             holding_period_avg_10w,
-            agg.return_sum,
-            agg.return_tw_sum,
-            agg.return_sq_tw_sum,
-            time_sum_10w,
+            agg.sharpe_sum_r,
+            agg.sharpe_sum_r2_over_dt,
+            agg.sharpe_time_sum,
             ps_token_avg,
             ps_exposure_avg,
             ps_volume,
             ps_holding_period_avg,
-            ps_return_sum,
-            ps_return_tw_sum,
-            ps_return_sq_tw_sum,
-            ps_time_sum,
+            ps_sharpe_sum_r,
+            ps_sharpe_sum_r2_over_dt,
+            ps_sharpe_time_sum,
         });
*** End Patch

```

有一段大 patch 没吃进去，我先精确读出当前 `prefix/window` 那一段再分块替换，避免误伤你正在看的文件。

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// sharpe = μ / σ
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(mean / std::sqrt(static_cast<double>(variance)));
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
    bool sharpe_prev_valid = false;
    int64_t sharpe_prev_block = 0;
    int64_t sharpe_prev_pnl = 0;
    int64_t sharpe_prev_exposure = 0;
    bool sharpe_pending_valid = false;
    int64_t sharpe_pending_block = 0;
    int64_t sharpe_pending_pnl = 0;
    int64_t sharpe_pending_exposure = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w, "
        "x.sharpe_prev_block_10w, x.sharpe_prev_pnl_10w, x.sharpe_prev_exposure_10w, x.sharpe_pending_pnl_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      const int64_t sharpe_prev_block = runtime_seed_result->GetValue(6, i).GetValue<int64_t>();
      const int64_t sharpe_prev_pnl = runtime_seed_result->GetValue(7, i).GetValue<int64_t>();
      const int64_t sharpe_prev_exposure = runtime_seed_result->GetValue(8, i).GetValue<int64_t>();
      const int64_t sharpe_pending_pnl = runtime_seed_result->GetValue(9, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      if (tag_id == -1) {
        assert(sharpe_prev_block >= 0);
        assert(sharpe_prev_exposure >= 0);
        if (sharpe_prev_block > 0) {
          state.sharpe_prev_valid = true;
          state.sharpe_prev_block = sharpe_prev_block;
          state.sharpe_prev_pnl = sharpe_prev_pnl;
          state.sharpe_prev_exposure = sharpe_prev_exposure;
        }
        if (last_block > 0) {
          state.sharpe_pending_valid = true;
          state.sharpe_pending_block = last_block;
          state.sharpe_pending_pnl = sharpe_pending_pnl;
          state.sharpe_pending_exposure = last_exposure;
          const int64_t pending_bucket = last_block / kBlockBucketSize;
          bucket_agg_state_by_agg_key.try_emplace(AggKey{user_id, pending_bucket, -1}, BucketAggState{});
        }
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_sum_r = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.sharpe_sum_r2_over_dt = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_time_sum = feature_state_result->GetValue(15, i).GetValue<int64_t>();
      agg.sharpe_prev_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.sharpe_prev_pnl = feature_state_result->GetValue(17, i).GetValue<int64_t>();
      agg.sharpe_prev_exposure = feature_state_result->GetValue(18, i).GetValue<int64_t>();
      agg.sharpe_pending_pnl = feature_state_result->GetValue(19, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  auto sync_bucket_sharpe_anchor = [&](BucketAggState &agg, const UserTagRuntimePairState &rt) {
    if (rt.sharpe_prev_valid) {
      agg.sharpe_prev_block = rt.sharpe_prev_block;
      agg.sharpe_prev_pnl = rt.sharpe_prev_pnl;
      agg.sharpe_prev_exposure = rt.sharpe_prev_exposure;
    } else {
      agg.sharpe_prev_block = 0;
      agg.sharpe_prev_pnl = 0;
      agg.sharpe_prev_exposure = 0;
    }
    agg.sharpe_pending_pnl = rt.sharpe_pending_valid ? rt.sharpe_pending_pnl : 0;
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      const int64_t account_pnl_after =
          feature_comp::round_i64(realized_total_it->second + realized_delta + unrealized_total_it->second);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        agg.volume_sum += volume;
        agg.last_sort_key = row.sort_key;
        if (agg_tag_id != -1) {
          return;
        }
        assert(agg_key.tag_id == -1);
        const int64_t account_exposure_after = rt.exposure;
        if (!rt.sharpe_pending_valid) {
          rt.sharpe_pending_valid = true;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else if (row_block == rt.sharpe_pending_block) {
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else {
          assert(row_block > rt.sharpe_pending_block);
          const int64_t pending_bucket = rt.sharpe_pending_block / kBlockBucketSize;
          AggKey pending_key{row.user_id, pending_bucket, -1};
          auto pending_agg_it = bucket_agg_state_by_agg_key.find(pending_key);
          assert(pending_agg_it != bucket_agg_state_by_agg_key.end());
          if (rt.sharpe_prev_valid) {
            assert(rt.sharpe_pending_block > rt.sharpe_prev_block);
            feature_comp::accumulate_sharpe_interval(
                pending_agg_it->second,
                rt.sharpe_pending_pnl - rt.sharpe_prev_pnl,
                rt.sharpe_prev_exposure,
                rt.sharpe_pending_exposure,
                rt.sharpe_pending_block - rt.sharpe_prev_block);
          }
          rt.sharpe_prev_valid = true;
          rt.sharpe_prev_block = rt.sharpe_pending_block;
          rt.sharpe_prev_pnl = rt.sharpe_pending_pnl;
          rt.sharpe_prev_exposure = rt.sharpe_pending_exposure;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        }
        sync_bucket_sharpe_anchor(agg, rt);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_sharpe_sum_r_10w, f.ps_sharpe_sum_r2_over_dt_10w, f.ps_sharpe_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(9, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t sharpe_sum_r_10w = 0;
        __int128 sharpe_sum_r2_over_dt_10w = 0;
        int64_t sharpe_time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_sharpe_sum_r = 0;
        __int128 ps_sharpe_sum_r2_over_dt = 0;
        int64_t ps_sharpe_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.sharpe_sum_r, agg.sharpe_sum_r2_over_dt, agg.sharpe_time_sum)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_sharpe_sum_r = 0, prev_ps_sharpe_time_sum = 0;
        __int128 prev_ps_sharpe_sum_r2_over_dt = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_sharpe_sum_r = prev.ps_sharpe_sum_r;
          prev_ps_sharpe_sum_r2_over_dt = prev.ps_sharpe_sum_r2_over_dt;
          prev_ps_sharpe_time_sum = prev.ps_sharpe_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_sharpe_sum_r = prev_rec->ps_sharpe_sum_r;
            prev_ps_sharpe_sum_r2_over_dt = prev_rec->ps_sharpe_sum_r2_over_dt;
            prev_ps_sharpe_time_sum = prev_rec->ps_sharpe_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_sharpe_sum_r =
            i64_narrow_checked(static_cast<__int128>(prev_ps_sharpe_sum_r) + agg.sharpe_sum_r);
        const __int128 ps_sharpe_sum_r2_over_dt = prev_ps_sharpe_sum_r2_over_dt + agg.sharpe_sum_r2_over_dt;
        const int64_t ps_sharpe_time_sum =
            i64_narrow_checked(static_cast<__int128>(prev_ps_sharpe_time_sum) + agg.sharpe_time_sum);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t sharpe_sum_r = 0, sharpe_time_sum = 0;
            __int128 sharpe_sum_r2_over_dt = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.sharpe_sum_r = row.ps_sharpe_sum_r;
            result.sharpe_sum_r2_over_dt = row.ps_sharpe_sum_r2_over_dt;
            result.sharpe_time_sum = row.ps_sharpe_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.sharpe_sum_r = rec->ps_sharpe_sum_r;
            result.sharpe_sum_r2_over_dt = rec->ps_sharpe_sum_r2_over_dt;
            result.sharpe_time_sum = rec->ps_sharpe_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t sharpe_sum_r_100 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_sum_r) - boundary_100.sharpe_sum_r);
        const int64_t sharpe_sum_r_1000 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_sum_r) - boundary_1000.sharpe_sum_r);
        const __int128 sharpe_sum_r2_over_dt_100 = ps_sharpe_sum_r2_over_dt - boundary_100.sharpe_sum_r2_over_dt;
        const __int128 sharpe_sum_r2_over_dt_1000 = ps_sharpe_sum_r2_over_dt - boundary_1000.sharpe_sum_r2_over_dt;
        const int64_t sharpe_time_sum_100 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_time_sum) - boundary_100.sharpe_time_sum);
        const int64_t sharpe_time_sum_1000 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_time_sum) - boundary_1000.sharpe_time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(sharpe_sum_r_100, sharpe_sum_r2_over_dt_100, sharpe_time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(sharpe_sum_r_1000, sharpe_sum_r2_over_dt_1000, sharpe_time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.sharpe_sum_r);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.sharpe_sum_r2_over_dt)));
        ap.Append(agg.sharpe_time_sum);
        ap.Append(agg.sharpe_prev_block);
        ap.Append(agg.sharpe_prev_pnl);
        ap.Append(agg.sharpe_prev_exposure);
        ap.Append(agg.sharpe_pending_pnl);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_sharpe_sum_r);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_sharpe_sum_r2_over_dt)));
        ap.Append(ps_sharpe_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.sharpe_sum_r,
            agg.sharpe_sum_r2_over_dt,
            agg.sharpe_time_sum,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_sharpe_sum_r,
            ps_sharpe_sum_r2_over_dt,
            ps_sharpe_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

我现在切到 schema 和持久化定义同步，确保运行时写入列序和建表列序完全一致；否则就算公式对了，落库也会错位。

```cpp
#include "stage3_sync.hpp"
#include <fstream>
namespace stage3 {

StageSync::StageSync(EventBuilder &builder, Database &stage0_db, Database &stage2_db, Database &stage3_db,
                     int base_interval_seconds)
    : builder_(builder), stage0_db_(stage0_db), stage2_db_(stage2_db), stage3_db_(stage3_db),
      event_fact_store_(std::make_unique<core::rocks::Stage3EventFactStore>(
          stage3_db_.data_dir() + "/event_fact.rocks")),
      base_interval_seconds_(base_interval_seconds) {
  assert(base_interval_seconds_ > 0);
  init_schema();
  load_tag_mapping();
  load_conditions();
  load_cursor();
  refresh_status_locked();
}

json StageSync::stage2_rocksdb_memory_breakdown() const {
  return builder_.rocksdb_memory_breakdown();
}

json StageSync::stage3_rocksdb_memory_breakdown() const {
  const core::rocks::MemoryStats stats = event_fact_store_->memory_stats();
  return {
      {"name", "stage3_event_fact"},
      {"engine", "rocksdb"},
      {"path", event_fact_store_->db_path()},
      {"memtables_bytes", stats.memtables_bytes},
      {"table_readers_bytes", stats.table_readers_bytes},
      {"block_cache_bytes", stats.block_cache_bytes},
      {"block_cache_pinned_bytes", stats.block_cache_pinned_bytes},
      {"estimated_total_bytes", stats.estimated_total_bytes()},
  };
}

void StageSync::load_tag_mapping() {
  auto trim = [](const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
      return std::string();
    }
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  };

  std::ifstream f("core-backend/src/stage0/TAG.md");
  assert(f.is_open());

  tag_to_industry_id_.clear();
  int8_t current_id = -1;
  int8_t next_id = 0;
  std::string line;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (line.rfind("## ", 0) == 0) {
      const std::string level1 = trim(line.substr(3));
      assert(!level1.empty());
      assert(next_id <= 12);
      current_id = next_id;
      ++next_id;
      const std::string key = normalize_tag_key(level1);
      assert(!key.empty());
      tag_to_industry_id_[key] = current_id;
      continue;
    }
    if (line.rfind("- ", 0) == 0) {
      assert(current_id >= 0);
      std::string rest = line.substr(2);
      size_t hash_pos = rest.find('#');
      if (hash_pos != std::string::npos) {
        rest = rest.substr(0, hash_pos);
      }
      const std::string level2 = trim(rest);
      assert(!level2.empty());
      const std::string key = normalize_tag_key(level2);
      assert(!key.empty());
      tag_to_industry_id_[key] = current_id;
    }
  }
  assert(next_id == 13);
  assert(!tag_to_industry_id_.empty());
}

void StageSync::init_schema() const {
  const std::string table_cursor_state = kSqlTableCursorState;
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;

  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_cursor_state + R"( (
        id INTEGER PRIMARY KEY,
        sort_key BIGINT NOT NULL,
        processed_events BIGINT NOT NULL
      )
    )");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_token_state + R"( (
        user_addr BLOB NOT NULL,
        cond_idx INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        pos BIGINT NOT NULL,
        cost BIGINT NOT NULL,
        lp BIGINT NOT NULL,
        entry_block BIGINT NOT NULL,
        PRIMARY KEY (user_addr, cond_idx, token_idx)
      )
    )");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_user_summary + R"( (
        user_addr BLOB PRIMARY KEY,
        total_events BIGINT NOT NULL,
        total_realized_pnl BIGINT NOT NULL,
        total_unrealized_pnl BIGINT NOT NULL,
        active_tokens BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL
      )
    )");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_feature_tensor + R"( (
        user_addr BLOB NOT NULL,
        block_bucket BIGINT NOT NULL,
        tag_id INTEGER NOT NULL,
        last_sort_key_10w BIGINT NOT NULL DEFAULT 0,
        last_block_10w BIGINT NOT NULL DEFAULT 0,
        last_exposure_10w BIGINT NOT NULL DEFAULT 0,
        last_holding_period_10w HUGEINT NOT NULL DEFAULT 0,
        last_token_count_10w BIGINT NOT NULL DEFAULT 0,
        time_weight_sum_10w BIGINT NOT NULL DEFAULT 0,
        token_count_tw_sum_10w BIGINT NOT NULL DEFAULT 0,
        exposure_tw_sum_10w HUGEINT NOT NULL DEFAULT 0,
        volume_sum_10w BIGINT NOT NULL DEFAULT 0,
        holding_period_exp_tw_sum_10w HUGEINT NOT NULL DEFAULT 0,
        sharpe_sum_r_10w BIGINT NOT NULL DEFAULT 0,
        sharpe_sum_r2_over_dt_10w HUGEINT NOT NULL DEFAULT 0,
        sharpe_time_sum_10w BIGINT NOT NULL DEFAULT 0,
        sharpe_prev_block_10w BIGINT NOT NULL DEFAULT 0,
        sharpe_prev_pnl_10w BIGINT NOT NULL DEFAULT 0,
        sharpe_prev_exposure_10w BIGINT NOT NULL DEFAULT 0,
        sharpe_pending_pnl_10w BIGINT NOT NULL DEFAULT 0,
        token_avg_10w BIGINT NOT NULL DEFAULT 0,
        exposure_avg_10w BIGINT NOT NULL DEFAULT 0,
        volume_10w BIGINT NOT NULL DEFAULT 0,
        holding_period_avg_10w BIGINT NOT NULL DEFAULT 0,
        sharpe_10w DOUBLE NOT NULL DEFAULT 0,
        ps_token_avg_10w BIGINT NOT NULL DEFAULT 0,
        ps_exposure_avg_10w BIGINT NOT NULL DEFAULT 0,
        ps_volume_10w BIGINT NOT NULL DEFAULT 0,
        ps_holding_period_avg_10w BIGINT NOT NULL DEFAULT 0,
        ps_sharpe_sum_r_10w BIGINT NOT NULL DEFAULT 0,
        ps_sharpe_sum_r2_over_dt_10w HUGEINT NOT NULL DEFAULT 0,
        ps_sharpe_time_sum_10w BIGINT NOT NULL DEFAULT 0,
        token_avg_100w BIGINT NOT NULL DEFAULT 0,
        token_avg_1000w BIGINT NOT NULL DEFAULT 0,
        exposure_avg_100w BIGINT NOT NULL DEFAULT 0,
        exposure_avg_1000w BIGINT NOT NULL DEFAULT 0,
        volume_avg_100w BIGINT NOT NULL DEFAULT 0,
        volume_avg_1000w BIGINT NOT NULL DEFAULT 0,
        holding_period_avg_100w BIGINT NOT NULL DEFAULT 0,
        holding_period_avg_1000w BIGINT NOT NULL DEFAULT 0,
        sharpe_100w DOUBLE NOT NULL DEFAULT 0,
        sharpe_1000w DOUBLE NOT NULL DEFAULT 0,
        updated_sort_key BIGINT NOT NULL DEFAULT 0,
        PRIMARY KEY (user_addr, block_bucket, tag_id)
      )
    )");
  // 仅创建非PK前缀的有用索引 (PK自带B-tree可覆盖前缀查询)
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexUserSummaryEvents) +
      " ON " + table_user_summary + "(total_events)");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexFeatureTensorBucketTagUser) +
      " ON " + table_feature_tensor + "(block_bucket, tag_id, user_addr)");
  auto conn = stage3_db_.create_connection();
  auto cursor_count_result = conn->Query("SELECT COUNT(*) FROM " + table_cursor_state + " WHERE id=1");
  assert(cursor_count_result && !cursor_count_result->HasError());
  if (cursor_count_result->GetValue(0, 0).GetValue<int64_t>() == 0) {
    auto ins = conn->Query(
        "INSERT INTO " + table_cursor_state + " VALUES (1, -1, 0)");
    assert(ins && !ins->HasError());
  }
}

void StageSync::load_conditions() {
  conditions_.clear();
  cond_tag_ids_.clear();
  cond_market_question_counts_.clear();

  auto stage2_conn = stage2_db_.create_connection();
  auto condition_result = stage2_conn->Query(
      "SELECT cond_idx, lower(hex(cond_id)) AS cond_hex, outcome_cnt, "
      "payout_0, payout_1, payout_2, payout_3, payout_4, payout_5, payout_6, payout_7, "
      "CASE WHEN question_id IS NULL THEN '' ELSE lower(hex(question_id)) END AS qid, source "
      "FROM rb_condition ORDER BY cond_idx");
  assert(condition_result && !condition_result->HasError());
  conditions_.resize(static_cast<size_t>(condition_result->RowCount()));
  cond_tag_ids_.assign(static_cast<size_t>(condition_result->RowCount()), 13);
  cond_market_question_counts_.assign(static_cast<size_t>(condition_result->RowCount()), 0);
  std::unordered_map<std::string, uint32_t> cond_hex_to_index;
  cond_hex_to_index.reserve(static_cast<size_t>(condition_result->RowCount()) + 1);
  for (idx_t i = 0; i < condition_result->RowCount(); ++i) {
    uint32_t idx = condition_result->GetValue(0, i).GetValue<uint32_t>();
    assert(idx == i);
    std::string cond_hex = condition_result->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_hex_to_index.emplace(cond_hex, idx);
    ConditionInfo info;
    info.outcome_count = condition_result->GetValue(2, i).GetValue<uint8_t>();
    info.payout_numerators.reserve(info.outcome_count);
    for (int j = 0; j < info.outcome_count; ++j) {
      auto payout_value = condition_result->GetValue(3 + j, i);
      info.payout_numerators.push_back(payout_value.IsNull() ? -1 : payout_value.GetValue<int64_t>());
    }
    std::string question_hex = condition_result->GetValue(11, i).GetValueUnsafe<std::string>();
    if (!question_hex.empty()) {
      info.question_id = "0x" + question_hex;
    }
    info.source = static_cast<ConditionSource>(condition_result->GetValue(12, i).GetValue<int32_t>());
    conditions_[idx] = std::move(info);
  }

  std::unordered_map<std::string, std::string> question_to_market;
  std::unordered_map<std::string, int32_t> market_question_counts;
  auto neg_risk_market_result = stage2_conn->Query(
      "SELECT lower(hex(question_id)) AS qid_hex, lower(hex(market_id)) AS market_hex "
      "FROM rb_neg_risk_market");
  assert(neg_risk_market_result && !neg_risk_market_result->HasError());
  question_to_market.reserve(static_cast<size_t>(neg_risk_market_result->RowCount()) + 1);
  market_question_counts.reserve(static_cast<size_t>(neg_risk_market_result->RowCount()) + 1);
  for (idx_t i = 0; i < neg_risk_market_result->RowCount(); ++i) {
    const std::string question_hex = neg_risk_market_result->GetValue(0, i).GetValueUnsafe<std::string>();
    const std::string market_hex = neg_risk_market_result->GetValue(1, i).GetValueUnsafe<std::string>();
    auto [market_it, inserted] = question_to_market.emplace(question_hex, market_hex);
    assert(inserted || market_it->second == market_hex);
    market_question_counts[market_hex]++;
  }

  auto stage0_conn = stage0_db_.create_connection();
  auto tags = stage0_conn->Query(
      "SELECT lower(hex(condition_id)) AS cond_hex, coalesce(tag_name, 'Unknown') AS tag_name "
      "FROM pm_condition_scan_class");
  assert(tags && !tags->HasError());
  for (idx_t i = 0; i < tags->RowCount(); ++i) {
    std::string cond_hex = tags->GetValue(0, i).GetValueUnsafe<std::string>();
    auto cond_it = cond_hex_to_index.find(cond_hex);
    if (cond_it == cond_hex_to_index.end()) {
      continue;
    }
    std::string tag_name = tags->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_tag_ids_[cond_it->second] = tag_name_to_id(tag_name);
  }

  for (size_t i = 0; i < conditions_.size(); ++i) {
    const auto &info = conditions_[i];
    if (info.question_id.empty()) {
      continue;
    }
    assert(info.question_id.rfind("0x", 0) == 0);
    const std::string question_hex = info.question_id.substr(2);
    auto question_market_it = question_to_market.find(question_hex);
    if (question_market_it == question_to_market.end()) {
      continue;
    }
    auto market_question_count_it = market_question_counts.find(question_market_it->second);
    assert(market_question_count_it != market_question_counts.end());
    assert(market_question_count_it->second > 0);
    cond_market_question_counts_[i] = static_cast<uint16_t>(market_question_count_it->second);
  }
}

void StageSync::load_cursor() {
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query(
      "SELECT sort_key, processed_events FROM " +
      std::string(kSqlTableCursorState) + " WHERE id=1");
  assert(r && !r->HasError() && r->RowCount() == 1);
  sync_cursor_.sort_key = r->GetValue(0, 0).GetValue<int64_t>();
  sync_cursor_.processed_events = r->GetValue(1, 0).GetValue<int64_t>();
}

void StageSync::save_cursor_locked(duckdb::Connection &conn) const {
  auto q = conn.Query(
      "UPDATE " + std::string(kSqlTableCursorState) + " SET "
                                                      "sort_key=" +
      std::to_string(sync_cursor_.sort_key) +
      ", processed_events=" + std::to_string(sync_cursor_.processed_events) +
      " WHERE id=1");
  assert(q && !q->HasError());
}

void StageSync::refresh_status_locked() const {
  sync_.head_block = builder_.cursor();
  sync_.last_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  sync_.behind_blocks = std::max<int64_t>(0, sync_.head_block - sync_.last_block);
  sync_.behind_chunks = (sync_.behind_blocks == 0) ? 0 : 1;
}

} // namespace stage3

```

代码主体已经换到新口径了，我现在同步改 `README_stage3.md` 里的状态字段和 Sharpe 数学定义，避免文档继续写旧的 `realized_delta / exposure_before`。

```markdown
## 交易口径事件（符号驱动, 支持做空）
> `qty = abs(amount)`
> `dir = sign(amount)`（`+1 / -1 / 0`）
> `px = price_1e6 / 1e6`
> `cost_before = cost`（本事件前该token的cost）
> `pos_before = pos`（本事件前该token的pos）
> `has_usd = collateral ∈ {USDC(1), USDCe(2), USDT(3), WrappedUSDCe(4)}`

**Collateral 规则**: 只有 USD 类抵押物 (`has_usd=true`) 才计算 `cost / realized_delta / lp`；非 USD 仅更新 `pos`。  
**方向规则**: `EventType` 决定“经济语义”, `amount` 符号决定“方向”；同一 `EventType` 可出现正负。

| 事件族                                                     | `amount > 0` (正向腿)                                        | `amount < 0` (反向腿)                                         | realized 规则                                                | lp更新                   |
| ---------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------- | ------------------------------------------------------------ | ------------------------ |
| `Order* / FPMM* / Split* / Merge* / Redemption*`           | 先平空(`pos<0`)再开多；开多部分 `cost += open_long_qty * px` | 先平多(`pos>0`)再开空；开空部分 `cost -= open_short_qty * px` | 仅"平掉已有仓位"部分计 realized；开新仓部分不计当期 realized | 仅 `Order* / FPMM*` 更新 |
| `Convert`                                                  | 先平空再开多；开多部分 `cost += open_long_qty * convert_px`  | 先平多再开空；开空部分 `cost -= open_short_qty * convert_px`  | `convert_px = (popcount-1)/popcount`, 仅平仓部分计 realized  | 否                       |
| `TransferIn* / TransferOut* / FPMMLPRemove / FPMMLPReturn` | 先平空再开多；`cost` 不增加                                  | 先平多再开空；`cost` 不增加（保持 transfer 语义）             | 始终 `0`                                                     | 否                       |
| `FPMMLPAdd`                                                | 不改 `pos/cost`                                              | 不改 `pos/cost`                                               | `0`                                                          | 否                       |

`amount == 0` 视为 no-op（仅事件计数推进, 不改持仓/成本/PnL）。

## Unrealized PnL 计算
```
// 对该用户所有有持仓的token求和
unrealized_pnl = Σ(pos * lp / 1e6 - cost)  // 对所有 token_state 中 pos != 0 且 lp > 0 的token
```
- 只有 Order/FPMM 交易事件会更新 lp
- Split/Merge/Transfer 等不更新 lp
- 若某token从未交易过（如TransferIn获得）,该token不计入unrealized（lp=0）

## Flow 图

```text
stage3_bootstrap
└─ 初始化 schema 与索引；若游标不存在则写入初始 cursor

stage3_sync_tick
├─ 0) 读取游标与元数据
│  ├─ 读 sync_cursor_state(id=1) -> SyncCursorState
│  ├─ 刷新 condition_meta(只读缓存)
│  └─ 计算批次游标起点 (sort_key_exclusive = cursor.sort_key)
├─ 1) 拉取输入事件(Stage2 输出, RocksDB sort 索引)
│  ├─ scan_by_sort_key(sort_key_exclusive=cursor.sort_key,
│  │                  sort_key_inclusive=head_block*1e9+(1e9-1),
│  │                  limit=batch_events)
│  ├─ 返回顺序按 key: (sort_key, user_addr, cond_idx, event_type, token_idx)
│  └─ 若无事件 -> 将 cursor.sort_key 推进到 head_sort_key 后结束本 tick
├─ 2) 预加载状态
│  ├─ 扫描 batch_events,提取 touched keys
│  │  ├─ touched_token_keys = {(user_addr, cond_idx, token_idx)}
│  │  └─ touched_users      = {user_addr}
│  ├─ 读 token_state  -> token_state_map
│  ├─ 读 user_summary_state -> user_state_map
│  └─ 初始化临时结构
│     ├─ fact_rows
│     └─ dirty_token_keys / dirty_users
├─ 3) 回放循环(for evt in batch_events, 按 sort_key + PK 升序)
│  ├─ 3.1 输入断言
│  │  ├─ (sort_key, user_addr, cond_idx, event_type, token_idx) 严格递增
│  │  ├─ cond_idx < 0: 不进入状态机(仅事实行)
│  │  ├─ cond_idx >= 0: condition_meta 必须存在
│  │  ├─ token_idx ∈ [0, outcome_count)
│  │  ├─ event_type 合法；amount 符号不做类型硬断言（允许同类型正负共存）
│  │  ├─ amount==0 允许（no-op）
│  │  └─ 常规指标使用 double 计算并 round() 写入 int64；大数累计项直接写 HUGEINT
│  ├─ 3.2 路由
│  │  ├─ 方向由 amount 符号决定:正向腿(amount>0) / 反向腿(amount<0)
│  │  ├─ 事件族决定经济语义:price类 / convert类 / transfer类 / lp_add类
│  │  └─ LP添加: FPMMLPAdd(仅记录,不改pos/cost)
│  ├─ 3.3 应用规则
│  │  ├─ 公共计算
│  │  │  ├─ qty = abs(amount), px = price_1e6/1e6
│  │  │  └─ amount==0: no-op
│  │  ├─ 正向腿(amount>0): 先平空(pos<0)再开多
│  │  │  ├─ price/convert 类: 开多部分增加 cost
│  │  │  └─ transfer/LP-return 类: 开多部分不增加 cost
│  │  ├─ 反向腿(amount<0): 先平多(pos>0)再开空
│  │  │  ├─ price/convert 类: 开空部分记负 cost（入场信用）
│  │  │  └─ transfer/LP-return 类: 开空部分不改 cost
│  │  ├─ realized 仅在“平掉已有仓位”时产生
│  │  │  ├─ price 类: realized = closed_qty * px - cost_removed
│  │  │  └─ convert 类: realized = closed_qty * ((popcount-1)/popcount) - cost_removed
│  │  └─ FPMMLPAdd: 不改 token pos/cost; realized_delta=0
│  ├─ 3.4 更新 token state
│  │  ├─ 更新 pos, cost, lp (若Order/FPMM且price>0)
│  │  └─ 更新 entry_block:
│  │       建仓 (pos: 0 → 非0):    entry_block = current_block
│  │       加仓 (|pos| 增加):      entry_block = (|old_pos| * old_entry + delta * current_block) / |new_pos|
│  │       减仓 (|pos| 减少):      entry_block 不变
│  │       平仓 (pos → 0):         删除该行
│  ├─ 3.5 计算 user 级别聚合
│  │  ├─ unrealized_pnl = Σ(pos * lp / 1e6 - cost) for all tokens
│  │  ├─ token_count = 有效持仓token数（过滤dust, |qty| >= 10 token）
│  │  └─ 计算 exposure, holding_period (token级别)
│  ├─ 3.6 更新 user_state_map[user_addr]
│  │  ├─ total_events += 1
│  │  ├─ total_realized_pnl += realized_delta
│  │  └─ last_sort_key = evt.sort_key
│  ├─ 3.7 产出事实行
│  │  └─ append EventFact(user_addr, sort_key, cond_idx, token_idx, event_type,
│  │                         realized_delta, realized_cum, unrealized_pnl, token_count,
│  │                         tag_id, exposure, volume, holding_period)
│  └─ 3.8 统一特征图更新 (按 bucket 增量)
│     ├─ 将事件映射为 (user_addr, tag_id, block_bucket) dirty key
│     ├─ 更新该 bucket 的 Node-A0 续算锚点 + Node-A 原子统计节点
│     ├─ 依赖前序 bucket 前缀节点, 计算 100w/1000w 窗口节点
│     └─ 产出并覆盖 feature_tensor_state 对应行
├─ 4) 批次收尾校验
│  ├─ token 状态有限: pos/cost 必须是 finite（允许负数,表示短仓/负成本）
│  ├─ fact_rows.size == batch_events.size
│  └─ 任一失败 -> rollback,cursor 不推进
├─ 5) 单事务提交
│  ├─ upsert token_state(dirty_token_keys)
│  ├─ upsert user_summary_state(dirty_users)
│  ├─ write event_fact.rocks/timeline(fact_rows)
│  ├─ upsert feature_tensor_state(dirty_user_bucket_tag, 含 Node-A0/A/B/C/D)
│  └─ update sync_cursor_state(sort_key, processed_events)
└─ 6) 查询路径(只读)
   ├─ users_sorted(limit)                 -> user_summary_state(用户列表)
   ├─ user_pnl_timeline(user)             -> 预热并缓存该用户 timeline + snapshots
   ├─ user_positions_at(user, sk)         -> 命中内存 snapshots 返回 positions
   └─ user_feature_filter(anchor, exprs)  -> feature_tensor_state(截面 top users)
```

## 数据结构

符号: `E=事件总数`, `U=有事件用户数`, `T=活跃token数(user,cond,token_idx)`, `F≈E/20`  
Size估算基准: E=2B, U=2M, T=1M, F=100M

| 数据结构                                      | 层级/实例数       | 行大小 | Size   | 主要用途                                          | Persist      | ORDER BY                          |
| --------------------------------------------- | ----------------- | ------ | ------ | ------------------------------------------------- | ------------ | --------------------------------- |
| `EventInput`                                  | 输入流 / `E`      | -      | -      | 回放状态机输入                                    | 否（临时）   | -                                 |
| `ConditionMeta`                               | 只读缓存 / `C`    | -      | -      | `outcome_count` 与 `tag_id` 元信息                | 否（可重载） | -                                 |
| `TokenState` (`token_state`)                  | Token级 / `T`     | 60B    | ~60MB  | 当前持仓 `pos/cost/lp/entry_block`                | 是           | `(user_addr,cond_idx,token_idx)`  |
| `EventFact` (`event_fact.rocks/timeline`)     | Token级 / `E`     | 96B    | ~190GB | 事件事实、timeline、特征原料                      | 是           | `(user_addr,sort_key,cond_idx,event_type,token_idx)` |
| `UserSummaryState` (`user_summary_state`)     | User级 / `U`      | 60B    | ~120MB | 用户总览查询加速                                  | 是           | `(user_addr)`                     |
| `FeatureTensorState` (`feature_tensor_state`) | User*Bucket*Tag级 | 536B   | ~50GB  | 统一特征张量（含原子/窗口/前缀缓存/增量续算锚点） | 是           | `(block_bucket,tag_id,user_addr)` |
| `SyncCursorState` (`sync_cursor_state`)       | 全局 / `1`        | <1KB   | <1KB   | 增量同步断点                                      | 是           | -                                 |
| `UserQueryCache`                              | 进程内 / `<=U`    | -      | -      | 查询缓存（timeline/snapshot）                     | 否（内存）   | -                                 |

```text
// Stage3 内部统一输入结构 (用于回放/状态机)
struct EventInput {
  string user_hex;        // 查询路径可为空, 批处理路径必填
  int64  sort_key;
  int32  cond_idx;
  int32  event_type;
  int32  token_idx;
  int32  collateral;
  int64  amount;          // signed, 1e6
  int64  price_1e6;
}

// Condition 元数据 (只读缓存)
struct ConditionMeta {
  uint8 outcome_count;
  int64 payout_numerators[256];  // 支持任意outcome数
  int8 tag_id;                   // 行业分类
}

// Token 状态 (稀疏存储, 只存有持仓的token)
// PK: (user_addr, cond_idx, token_idx)
struct TokenState {
  double pos;           // 持仓量 (可负=空头)
  double cost;          // 成本基础 (可负=空头信用)
  double lp;            // 最近成交价
  double entry_block;   // 加权平均建仓block
}

// 事件事实行 (持久化, 用于构建 timeline)
struct EventFact {
  Address20 user_addr;
  int64   sort_key;
  int32   cond_idx;
  int32   token_idx;
  int32   event_type;
  int64   realized_delta;     // 本事件 realized 增量
  int64   realized_cum;       // user级别累计 realized pnl
  int64   unrealized_pnl;     // user级别 unrealized pnl
  int32   token_count;        // user级别有效持仓 token 种数（|qty| >= 10 token）
  // 新增字段
  int8    tag_id;             // 行业分类 (0-13)
  int64   exposure;           // 该token暴露额 |pos*lp|
  int64   volume;             // 交易额 |amount*price|
  int64   holding_period;     // 该token持仓周期 (blocks)
}

// 用户摘要 (持久化)
struct UserSummaryState {
  int64  total_events;
  int64  total_realized_pnl;
  int64  total_unrealized_pnl;
  int32  active_tokens;       // 有持仓的token数
  int64  last_sort_key;
}

// 统一特征张量状态 (持久化)
// PK: (user_addr, block_bucket, tag_id)
// 注: tag_id = -1 表示全行业聚合行（用于总夏普等 1 维特征）
struct FeatureTensorState {
  Address20 user_addr;
  int64  block_bucket;
  int32  tag_id;

  // Node-A0: 增量续算锚点
  // 持仓 tail 修正 + Sharpe block-merge 续算, 无需回扫 event_fact
  int64  last_sort_key_10w;
  int64  last_block_10w;              // 当前 pending block
  int64  last_exposure_10w;           // 当前 pending exposure
  int128 last_holding_period_10w;        // DuckDB: HUGEINT
  int64  last_token_count_10w;
  int64  sharpe_prev_block_10w;       // 上一个已完成采样点 block
  int64  sharpe_prev_pnl_10w;         // 上一个已完成采样点 pnl
  int64  sharpe_prev_exposure_10w;    // 上一个已完成采样点 exposure
  int64  sharpe_pending_pnl_10w;      // 当前 pending block 的块末 pnl

  // Node-A: 10w 原子统计（事件增量累加）
  int64  time_weight_sum_10w;
  int64  token_count_tw_sum_10w;
  int128 exposure_tw_sum_10w;            // DuckDB: HUGEINT
  int64  volume_sum_10w;
  int128 holding_period_exp_tw_sum_10w;  // DuckDB: HUGEINT
  int64  sharpe_sum_r_10w;               // Σr_i, r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
  int128 sharpe_sum_r2_over_dt_10w;      // Σ(r_i² / Δt_i), DuckDB: HUGEINT
  int64  sharpe_time_sum_10w;            // ΣΔt_i

  // Node-B: 10w 归一化输出
  int64  token_avg_10w;
  int64  exposure_avg_10w;
  int64  volume_10w;
  int64  holding_period_avg_10w;
  float  sharpe_10w;

  // Node-C: 前缀缓存（按 bucket 单调推进）
  int64  ps_token_avg_10w;
  int64  ps_exposure_avg_10w;
  int64  ps_volume_10w;
  int64  ps_holding_period_avg_10w;
  int64  ps_sharpe_sum_r_10w;
  int128 ps_sharpe_sum_r2_over_dt_10w;   // DuckDB: HUGEINT
  int64  ps_sharpe_time_sum_10w;

  // Node-D: 窗口投影输出（由 Node-C O(1) 计算）
  int64  token_avg_100w;
  int64  token_avg_1000w;
  int64  exposure_avg_100w;
  int64  exposure_avg_1000w;
  int64  volume_avg_100w;
  int64  volume_avg_1000w;
  int64  holding_period_avg_100w;
  int64  holding_period_avg_1000w;
  float  sharpe_100w;
  float  sharpe_1000w;

  int64  updated_sort_key;
}

// 游标
struct SyncCursorState {
  int64 sort_key;
  int64 processed_events;
}

// 查询缓存（单用户）
struct UserQueryCache {
  string user_addr_lower;
  vector<TimelineRow> timeline;
  vector<{int64 sort_key; vector<PositionRow> positions;}> snapshots;
}
```

## API 设计

### GET /api/stage3-status
| entry                  | type   | required | description          |
| ---------------------- | ------ | -------- | -------------------- |
| query                  | object | 否       | 无参数               |
| resp.syncing           | bool   | 是       | 是否正在同步         |
| resp.last_block        | i64    | 是       | 已同步到的最新 block |
| resp.head_block        | i64    | 是       | 链上当前 head block  |
| resp.behind_blocks     | i64    | 是       | 落后 block 数        |
| resp.behind_chunks     | i64    | 是       | 预估剩余 chunk 数    |
| resp.blocks_per_second | f64    | 是       | 同步速度             |
| resp.eta_seconds       | f64    | 是       | 预计剩余秒数         |
| resp.ready             | bool   | 是       | 是否可用于查询       |

### GET /api/stage3-pnl?user=0x...
| entry                | type   | required | description                      |
| -------------------- | ------ | -------- | -------------------------------- |
| query.user           | string | 是       | 用户地址                         |
| resp.user            | string | 是       | 用户地址                         |
| resp.block           | i64    | 是       | 该用户最新事件所在 block         |
| resp.total_events    | i64    | 是       | 该用户总事件数                   |
| resp.timeline[]      | array  | 是       | 全量事件序列（不因 cursor 截断） |
| resp.timeline[].sk   | i64    | 是       | sort_key（`block*1e9+log_idx`）  |
| resp.timeline[].ty   | u8     | 是       | 事件类型                         |
| resp.timeline[].rpnl | i64    | 是       | 累计已实现 PnL                   |
| resp.timeline[].upnl | i64    | 是       | 该时刻未实现 PnL                 |
| resp.timeline[].tk   | i32    | 是       | 有效持仓 token 种数（过滤dust）  |
| error.404            | object | 条件     | 用户不存在或无事件时返回         |

### GET /api/stage3-positions?user=0x...&sort_key=...
| entry                 | type   | required | description                            |
| --------------------- | ------ | -------- | -------------------------------------- |
| query.user            | string | 是       | 用户地址                               |
| query.sort_key        | i64    | 是       | 目标 cursor                            |
| resp.user             | string | 是       | 用户地址                               |
| resp.sort_key         | i64    | 是       | 回显请求的 cursor                      |
| resp.block            | i64    | 是       | 快照所在 block                         |
| resp.positions[]      | array  | 是       | 指定 cursor 的持仓快照（仅 positions） |
| resp.positions[].ci   | u32    | 是       | cond_idx                               |
| resp.positions[].ti   | u8     | 是       | token_idx                              |
| resp.positions[].qty  | i64    | 是       | 持仓数量（`1e6=1 token`, 可负）        |
| resp.positions[].cost | i64    | 是       | 成本基础（`1e6=$1`, 可负）             |
| resp.positions[].lp   | i64    | 是       | 最近成交价（`1e6=$1`）                 |
| resp.positions[].eb   | i64    | 是       | 建仓block                              |

### POST /api/stage3-filter
| entry                   | type     | required | description                                    |
| ----------------------- | -------- | -------- | ---------------------------------------------- |
| req.anchor_bucket       | i64      | 是       | 锚定 bucket（必须显式传入）                    |
| req.filters[]           | string[] | 否       | 过滤表达式列表（按 `AND` 连接）                |
| req.sort_expr           | string   | 是       | 排序表达式                                     |
| req.sort_asc            | bool     | 是       | 排序方向（必须显式传入）                       |
| req.limit               | i32      | 是       | 返回条数（必须显式传入，当前实现固定传 `100`） |
| resp.anchor_bucket      | i64      | 是       | 实际执行使用的 anchor bucket                   |
| resp.users[]            | array    | 是       | 过滤 + 排序后的用户列表                        |
| resp.users[].addr       | string   | 是       | 用户地址                                       |
| resp.users[].sort_value | f64      | 是       | 排序表达式值                                   |

## 建立动态高玩池(特征工程, 统一表 + Compute Graph)

10w blk ~ 2.3day ~ 日线  
100w blk ~ 23day ~ 月线  
1000w blk ~ 230day ~ 年线

特征张量索引: `User * Time(per 10w bucket) * Feature`

行业(N):
Crypto_Price
Crypto_Market
Sports_Basketball
Sports_Football
Sports_Soccer
Sports_Individual
Politics_US
Politics_World
Economy_Finance
Tech
Entertainment
Weather
Society
Unknown

时序特征(名称,数量,统计方式,描述):
注: 数量 N+1 表示 N 个行业 + 1 个综合(tag_id=-1)
    近期平均持仓token数        N+1  时间加权           近10w块移动平均持仓token数(分行业+综合)
    近月平均持仓token数        N+1  等权               近100w块移动平均持仓token数(分行业+综合)
    近年平均持仓token数        N+1  等权               近1000w块移动平均持仓token数(分行业+综合)
    近期平均持仓暴露额         N+1  时间加权           近10w块移动平均持仓暴露额(分行业+综合)
    近月平均持仓暴露额         N+1  等权               近100w块移动平均持仓暴露额(分行业+综合)
    近年平均持仓暴露额         N+1  等权               近1000w块移动平均持仓暴露额(分行业+综合)
    近期总和交易额             N+1  加总               近10w块总和交易额(分行业+综合)
    近月平均交易额             N+1  等权               近100w块平均交易额(分行业+综合)
    近年平均交易额             N+1  等权               近1000w块平均交易额(分行业+综合)
    近期平均持仓周期           N+1  时间,金额加权      近10w块移动平均持仓周期(分行业+综合)
    近月平均持仓周期           N+1  等权               近100w块移动平均持仓周期(分行业+综合)
    近年平均持仓周期           N+1  等权               近1000w块移动平均持仓周期(分行业+综合)
    近期夏普                   1    精确计算           近10w块全账户夏普(仅全账户)
    近月夏普                   1    精确计算           近100w块全账户夏普(仅全账户)
    近年夏普                   1    精确计算           近1000w块全账户夏普(仅全账户)
截面特征:
    输入: anchor bucket + 多条自由表达式(filters) + 排序表达式(sort_expr)
    输出: 过滤 + 排序后的 top 100 用户地址
    目标: 支持“分行业 nested 特征”的横截面对比与选人

注:
  1. 交易额里: 只记录会直接创造头寸暴露的操作(比如铸币, 合币就不应该记入), 暴露方向不重要
  2. 平均持仓: 需要统计周期内多个事件(非均匀)的持仓快照(记录不同token的平均持仓周期), 再按照token金额, 事件时间加权
  3. 夏普: 无风险=0, 基于账户级 `pnl/exposure` 的 block-merge 增量计算（与其他特征一致动态维护）
     - 仅统计全账户 Sharpe（tag_id=-1），不支持分行业 Sharpe
     - 采样点: 同一 block 内多事件先合并，只取块末 `(block, pnl, account_exposure)`
     - 其中: `pnl = realized_cum + unrealized_pnl`
     - 相邻采样点区间:
       - `Δpnl_i = pnl_i - pnl_{i-1}`
       - `Δt_i = block_i - block_{i-1}`
       - `cap_i = RMS(exp_{i-1}, exp_i) = sqrt((exp_{i-1}^2 + exp_i^2) / 2)`
       - `r_i = Δpnl_i / cap_i`
     - 增量累加:
       - `sharpe_sum_r += r_i`
       - `sharpe_sum_r2_over_dt += r_i^2 / Δt_i`
       - `sharpe_time_sum += Δt_i`
     - 时间加权平均收益率: `μ = sharpe_sum_r / T`
     - 时间加权方差: `σ² = sharpe_sum_r2_over_dt / T - μ²`
     - Sharpe = `μ / σ`
     - `cap_i < 0.001 USD` 或 `Δt_i <= 0` 或 `T <= 0` 时，该区间/窗口 Sharpe 贡献记 0
  4. 本次 Sharpe 口径变更不做兼容迁移；切换代码后必须重建 `data/stage3`

```

我先跑一遍这些改动文件的 IDE 诊断，先把明显的命名/类型/未定义字段问题清掉，再继续收尾 todo。

我把旧 Sharpe 路径已经全部替成新口径，并且 README 也同步完了；现在把 todo 全部收口，接着给你一个很短的结果说明和需要你手动做的重建动作。

已按计划改完，旧的 `realized_delta / exposure_before` 夏普路径已经删掉，改成了你定的这套：

- 按 `block` 合并采样，同一 `block` 只保留块末账户状态
- `pnl = rpnl + upnl`
- 区间收益 `r_i = Δpnl / RMS(exp_{i-1}, exp_i)`
- 时间加权统计改为维护 `Σr_i`、`Σ(r_i^2 / Δt_i)`、`ΣΔt_i`
- 仅 `tag_id = -1` 计算 Sharpe，分行业仍为 `0`

主要动了这些文件：
- `core-backend/src/stage3/stage3_comp_feat.hpp`
- `core-backend/src/stage3/stage3_comp_feat.cpp`
- `core-backend/src/stage3/stage3_sync_runtime.cpp`
- `core-backend/src/stage3/stage3_sync_schema.cpp`
- `README_stage3.md`

另外我把 `feature_tensor_state` 的 Sharpe 中间字段也直接换成了新语义，没有保留任何向后兼容。`read_lints` 看过这几个改动文件，当前没有 IDE lint 报错。

你下一步需要手动做的只有一件事：按新 schema 重建 `data/stage3`。旧数据里的 Sharpe 中间量和新口径不兼容，不能混用。按你的要求，我没有编译也没有运行。

---

**User**

做出来效果不好：
所有夏普都需要做年化normalize， 也就是normalize到1000W的range， 这个你觉得需要做吗

---

**Cursor**

我觉得`需要`，但要分清楚它能解决什么、不能解决什么。

现在这版算出来的是以`block`为基础时间单位的 raw Sharpe。  
如果你的目标是“所有窗口都落到同一个`1000W blocks`量纲上，方便展示和筛选阈值”，那应该做统一 annualize：

`sharpe_norm = sharpe_raw * sqrt(10_000_000)`

也就是统一乘 `sqrt(1000W) ≈ 3162.28`。

关键点有两个：

- 这个因子应该对 `sharpe_10w`、`sharpe_100w`、`sharpe_1000w` 都是同一个常数。
- 不要按窗口分别乘 `sqrt(1000w/10w)`、`sqrt(1000w/100w)` 这种窗口相关因子。那不是“统一年化”，而是在额外放大短窗噪声，`10w` 会更炸。

所以我的判断是：

- 如果你现在不满意的是“很多值只有 `0.00几`，量纲太小，不方便看”，那`需要做`。
- 如果你现在不满意的是“很多值已经有 `1/-1`，但和体感/净值不对上”，那`光做年化没用`。它只会把 `1/-1` 变成更大的数，不会改变排序，也不会修复分布问题。

一句话说，就是：

`年化 normalize 是展示尺度修正，不是统计口径修正。`

我建议下一步直接这样做：

- 内部保留 raw Sharpe 口径思维
- 对外写入/展示/筛选的 `sharpe_10w/100w/1000w` 统一乘 `sqrt(10000000)`

如果你要，我下一条就直接把代码改成这个版本，并同步改 `README_stage3.md`。

---

**User**

我理解是：
1. 我们目前的sharpe， 都是基于一个时间范围做计算的
2. 为了做到年化， 我们根据时间范围吧总return scale到一年（1000W block）， 同时假设波动性不变
3. 对于10w, 100w, 1000w sharpe都做成年化的， 统一口径
4. 你之前的per blcok 口径， 我觉得有问题， 你也是把范围normalize到per block， 同时假设波动率不变吗

---

**Cursor**

你的理解大体对，但第 `2` 条要改一下口径。

1. 现在这三个 `sharpe_10w / 100w / 1000w`，样本确实都来自各自时间窗口。
2. 但如果要“年化到 1000W block”，不是“总 return 拉到一年、波动率不变”。
   正确说法是：
   - 单位时间平均收益 `μ` 假设稳定，所以累计收益按时间线性放大
   - 单位时间方差 `σ²` 假设稳定，所以累计方差按时间线性放大
   - 因此累计标准差 `σ` 按 `sqrt(T)` 放大
   - 所以 `Sharpe_1000w = Sharpe_unit * sqrt(10000000)`
3. 所以如果我们决定统一成“1000W block 年化口径”，那 `10w / 100w / 1000w` 都应该统一乘同一个常数 `sqrt(10000000)`。
4. 我之前那版 `per block` 口径，不是“把总 return 除以 block 数，然后假设波动不变”那么粗。
   它实际是在算：

```text
r_i = Δpnl_i / cap_i
u_i = r_i / Δt_i

μ_block = Σ(Δt_i * u_i) / ΣΔt_i
σ_block² = Σ(Δt_i * (u_i - μ_block)^2) / ΣΔt_i
Sharpe_raw = μ_block / σ_block
```

也就是先把每个区间都变成“单位 block 收益率”，再做时间加权均值和方差。  
所以这里连波动项也一起做了 unit-time normalize，不是只 normalize return。

真正带假设的是最后这一步年化：

```text
Sharpe_1000w = Sharpe_raw * sqrt(10000000)
```

这一步才是在假设：
- 单位时间统计性质近似平稳
- 累计方差随时间线性增长

所以结论是：

- `per block raw Sharpe` 这个中间口径本身没问题
- 但如果你要展示和筛选都统一到“年化/1000W口径”，那现在确实还差最后一个 `* sqrt(10000000)` 的 normalize

还有一个细节你可以一起定：
- 如果全系统时间单位就是 `block`，那就直接按 `1000W block` 年化，完全自洽
- 如果你更在意真实时间而不是 block 数，严格讲应该改成按 `timestamp` 年化，不是按 `block` 年化

如果你确认继续沿用 `block` 作为时间单位，我下一步就把三个 Sharpe 全部统一乘 `sqrt(10000000)`，并同步改 `README_stage3.md`。

```plaintext
r_i = Δpnl_i / cap_i
u_i = r_i / Δt_i

μ_block = Σ(Δt_i * u_i) / ΣΔt_i
σ_block² = Σ(Δt_i * (u_i - μ_block)^2) / ΣΔt_i
Sharpe_raw = μ_block / σ_block
```

```plaintext
Sharpe_1000w = Sharpe_raw * sqrt(10000000)
```

---

**User**

统一改成1000W normalize吧

---

**Cursor**

我把 Sharpe 输出统一成 `1000W block` normalize：代码里统一乘 `sqrt(10000000)`，并顺手把 `README_stage3.md` 的口径说明改成“输出值已归一到 1000W block”。

我直接在 Sharpe 计算函数里统一做 `1000W` normalize，这样 `10w/100w/1000w` 三个输出天然保持同一量纲，不需要在各个窗口分散处理。

```cpp
#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "sharpe_sum_r_10w BIGINT, sharpe_sum_r2_over_dt_10w HUGEINT, sharpe_time_sum_10w BIGINT, "
    "sharpe_prev_block_10w BIGINT, sharpe_prev_pnl_10w BIGINT, sharpe_prev_exposure_10w BIGINT, sharpe_pending_pnl_10w BIGINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "ps_sharpe_sum_r_10w BIGINT, ps_sharpe_sum_r2_over_dt_10w HUGEINT, ps_sharpe_time_sum_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w, sharpe_sum_r2_over_dt_10w, sharpe_time_sum_10w, "
    "sharpe_prev_block_10w, sharpe_prev_pnl_10w, sharpe_prev_exposure_10w, sharpe_pending_pnl_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w, ps_sharpe_sum_r2_over_dt_10w, ps_sharpe_time_sum_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "sharpe_sum_r_10w=excluded.sharpe_sum_r_10w, "
    "sharpe_sum_r2_over_dt_10w=excluded.sharpe_sum_r2_over_dt_10w, "
    "sharpe_time_sum_10w=excluded.sharpe_time_sum_10w, "
    "sharpe_prev_block_10w=excluded.sharpe_prev_block_10w, "
    "sharpe_prev_pnl_10w=excluded.sharpe_prev_pnl_10w, "
    "sharpe_prev_exposure_10w=excluded.sharpe_prev_exposure_10w, "
    "sharpe_pending_pnl_10w=excluded.sharpe_pending_pnl_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "ps_sharpe_sum_r_10w=excluded.ps_sharpe_sum_r_10w, "
    "ps_sharpe_sum_r2_over_dt_10w=excluded.ps_sharpe_sum_r2_over_dt_10w, "
    "ps_sharpe_time_sum_10w=excluded.ps_sharpe_time_sum_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.sharpe_sum_r_10w, f.sharpe_sum_r2_over_dt_10w, f.sharpe_time_sum_10w, "
    "f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

// 时间加权夏普计算：
// r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
// μ = Σr_i / T
// σ² = Σ(r_i² / Δt_i) / T - μ²
// raw_sharpe = μ / σ
// output_sharpe = raw_sharpe * sqrt(10_000_000)
// 统一归一到 1000w block 量纲，便于跨窗口比较
double calc_sharpe_from_returns(int64_t sum_r, __int128 sum_r2_over_dt, int64_t time_sum) {
  if (time_sum <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(time_sum);
  const long double mean = static_cast<long double>(sum_r) / T;
  const long double second_moment = i128_to_long_double(sum_r2_over_dt) / T;
  const long double variance = second_moment - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  const long double raw_sharpe = mean / std::sqrt(variance);
  const long double normalize_factor = std::sqrt(10000000.0L);
  return static_cast<double>(raw_sharpe * normalize_factor);
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
  int64_t ps_sharpe_sum_r = 0;
  __int128 ps_sharpe_sum_r2_over_dt = 0;
  int64_t ps_sharpe_time_sum = 0;
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    sync_cursor_.sort_key = head_sort_key;
    (void)query_checked("BEGIN");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (user_cmp > 0 ||
            (user_cmp == 0 &&
             (row.cond_idx > prev_cond_idx ||
              (row.cond_idx == prev_cond_idx &&
               (row.event_type > prev_event_type ||
                (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_user_blob = user_blob;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
    bool sharpe_prev_valid = false;
    int64_t sharpe_prev_block = 0;
    int64_t sharpe_prev_pnl = 0;
    int64_t sharpe_prev_exposure = 0;
    bool sharpe_pending_valid = false;
    int64_t sharpe_pending_block = 0;
    int64_t sharpe_pending_pnl = 0;
    int64_t sharpe_pending_exposure = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w, "
        "x.sharpe_prev_block_10w, x.sharpe_prev_pnl_10w, x.sharpe_prev_exposure_10w, x.sharpe_pending_pnl_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         f.sharpe_prev_block_10w, f.sharpe_prev_pnl_10w, f.sharpe_prev_exposure_10w, f.sharpe_pending_pnl_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      const int64_t sharpe_prev_block = runtime_seed_result->GetValue(6, i).GetValue<int64_t>();
      const int64_t sharpe_prev_pnl = runtime_seed_result->GetValue(7, i).GetValue<int64_t>();
      const int64_t sharpe_prev_exposure = runtime_seed_result->GetValue(8, i).GetValue<int64_t>();
      const int64_t sharpe_pending_pnl = runtime_seed_result->GetValue(9, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      if (tag_id == -1) {
        assert(sharpe_prev_block >= 0);
        assert(sharpe_prev_exposure >= 0);
        if (sharpe_prev_block > 0) {
          state.sharpe_prev_valid = true;
          state.sharpe_prev_block = sharpe_prev_block;
          state.sharpe_prev_pnl = sharpe_prev_pnl;
          state.sharpe_prev_exposure = sharpe_prev_exposure;
        }
        if (last_block > 0) {
          state.sharpe_pending_valid = true;
          state.sharpe_pending_block = last_block;
          state.sharpe_pending_pnl = sharpe_pending_pnl;
          state.sharpe_pending_exposure = last_exposure;
          const int64_t pending_bucket = last_block / kBlockBucketSize;
          bucket_agg_state_by_agg_key.try_emplace(AggKey{user_id, pending_bucket, -1}, BucketAggState{});
        }
      }
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_missing_users;
      runtime_seed_missing_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        runtime_seed_missing_users.insert(pair_key.user_id);
      }
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (uint32_t user_id : runtime_seed_missing_users) {
          const std::string &user_blob = user_blob_by_user_id[user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
        }
        ap.Close();
      }
      const std::string sql_from_token_state_with_touched_users =
          "FROM " + table_token_state + " s " +
          "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
      auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
      for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
        const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        if (user_id_it == user_id_by_user_blob.end()) {
          continue;
        }
        const uint32_t user_id = user_id_it->second;
        const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
        assert(cond_idx >= 0);
        assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
        const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
        const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
        const auto contrib = token_feature_contrib(loaded_state);
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
          apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
          apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_sum_r = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.sharpe_sum_r2_over_dt = hugeint_to_i128(feature_state_result->GetValue(14, i).GetValue<duckdb::hugeint_t>());
      agg.sharpe_time_sum = feature_state_result->GetValue(15, i).GetValue<int64_t>();
      agg.sharpe_prev_block = feature_state_result->GetValue(16, i).GetValue<int64_t>();
      agg.sharpe_prev_pnl = feature_state_result->GetValue(17, i).GetValue<int64_t>();
      agg.sharpe_prev_exposure = feature_state_result->GetValue(18, i).GetValue<int64_t>();
      agg.sharpe_pending_pnl = feature_state_result->GetValue(19, i).GetValue<int64_t>();
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }

  // L5: event stream apply + feature fold
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  auto sync_bucket_sharpe_anchor = [&](BucketAggState &agg, const UserTagRuntimePairState &rt) {
    if (rt.sharpe_prev_valid) {
      agg.sharpe_prev_block = rt.sharpe_prev_block;
      agg.sharpe_prev_pnl = rt.sharpe_prev_pnl;
      agg.sharpe_prev_exposure = rt.sharpe_prev_exposure;
    } else {
      agg.sharpe_prev_block = 0;
      agg.sharpe_prev_pnl = 0;
      agg.sharpe_prev_exposure = 0;
    }
    agg.sharpe_pending_pnl = rt.sharpe_pending_valid ? rt.sharpe_pending_pnl : 0;
  };
  for (const auto &row : event_input_rows) {
    auto realized_total_it = realized_total_by_user_id.find(row.user_id);
    auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
    auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
    assert(realized_total_it != realized_total_by_user_id.end());
    assert(unrealized_total_it != unrealized_total_by_user_id.end());
    assert(active_token_count_it != active_token_count_by_user_id.end());

    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      TokenState &st = token_state_it->second;
      const TokenState before_state = st;
      const auto before_contrib = token_feature_contrib(st);
      const double before_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int before_holding = static_cast<int>(before_contrib.token_count);
      realized_delta = apply_event_input(row, st);
      const auto after_contrib = token_feature_contrib(st);
      const double after_unrealized = feature_comp::calc_unrealized_pnl(
          feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int after_holding = static_cast<int>(after_contrib.token_count);
      unrealized_total_it->second += (after_unrealized - before_unrealized);
      active_token_count_it->second += (after_holding - before_holding);
      assert(active_token_count_it->second >= 0);

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
      apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
      const int64_t account_pnl_after =
          feature_comp::round_i64(realized_total_it->second + realized_delta + unrealized_total_it->second);
      if (!token_state_rounded_equal(before_state, st)) {
        dirty_token_key_set.insert(key);
      }

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = after_contrib.exposure;
      holding_period = feature_comp::calc_holding_period_blocks(
          row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
      const int64_t block_bucket =
          feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);

      auto apply_feature_bucket = [&](int8_t agg_tag_id) {
        UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
        auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
        UserTagRuntimePairState &rt = runtime_it->second;
        assert(rt.token_count >= 0);
        assert(rt.exposure >= 0);
        __int128 holding_exp = 0;
        if (rt.exposure > 0) {
          const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
          assert(computed >= 0);
          holding_exp = computed;
        } else {
          assert(rt.exposure_entry_sum == 0);
        }

        AggKey agg_key{row.user_id, block_bucket, agg_tag_id};
        auto bucket_agg_it = bucket_agg_state_by_agg_key.find(agg_key);
        assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
        BucketAggState &agg = bucket_agg_it->second;
        feature_comp::update_tail_window(
            agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
        agg.volume_sum += volume;
        agg.last_sort_key = row.sort_key;
        if (agg_tag_id != -1) {
          return;
        }
        assert(agg_key.tag_id == -1);
        const int64_t account_exposure_after = rt.exposure;
        if (!rt.sharpe_pending_valid) {
          rt.sharpe_pending_valid = true;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else if (row_block == rt.sharpe_pending_block) {
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        } else {
          assert(row_block > rt.sharpe_pending_block);
          const int64_t pending_bucket = rt.sharpe_pending_block / kBlockBucketSize;
          AggKey pending_key{row.user_id, pending_bucket, -1};
          auto pending_agg_it = bucket_agg_state_by_agg_key.find(pending_key);
          assert(pending_agg_it != bucket_agg_state_by_agg_key.end());
          if (rt.sharpe_prev_valid) {
            assert(rt.sharpe_pending_block > rt.sharpe_prev_block);
            feature_comp::accumulate_sharpe_interval(
                pending_agg_it->second,
                rt.sharpe_pending_pnl - rt.sharpe_prev_pnl,
                rt.sharpe_prev_exposure,
                rt.sharpe_pending_exposure,
                rt.sharpe_pending_block - rt.sharpe_prev_block);
          }
          rt.sharpe_prev_valid = true;
          rt.sharpe_prev_block = rt.sharpe_pending_block;
          rt.sharpe_prev_pnl = rt.sharpe_pending_pnl;
          rt.sharpe_prev_exposure = rt.sharpe_pending_exposure;
          rt.sharpe_pending_block = row_block;
          rt.sharpe_pending_pnl = account_pnl_after;
          rt.sharpe_pending_exposure = account_exposure_after;
        }
        sync_bucket_sharpe_anchor(agg, rt);
      };

      apply_feature_bucket(tag_id);
      apply_feature_bucket(-1);
    }
    double &realized_total = realized_total_it->second;
    double &unrealized_total = unrealized_total_it->second;
    realized_total += realized_delta;
    int32_t token_count = active_token_count_it->second;
    int64_t account_exposure = 0;
    {
      UserTagRuntimePairKey account_key{row.user_id, -1};
      auto account_it = runtime_pair_state_by_pair_key.find(account_key);
      if (account_it != runtime_pair_state_by_pair_key.end()) {
        account_exposure = account_it->second.exposure;
      }
    }
    event_fact_rows.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        feature_comp::round_i64(realized_delta),
        feature_comp::round_i64(realized_total),
        feature_comp::round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
        account_exposure,
    });
  }
  assert(event_fact_rows.size() == event_input_rows.size());

  // L6: prefix-sum history + projection
  // Node-C: 前缀和历史记录（用于 O(1) 窗口计算）
  // 按 (user_id, tag_id) 索引，每个 pair 存储按 bucket 降序排列的前缀和记录
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher> prefix_sum_history_by_pair_key;
  prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key.size());
  if (!bucket_agg_state_by_agg_key.empty()) {
    // 查询每个 (user, tag) 对的历史前缀和（不在当前 batch 中的 bucket）
    // 只查询 bucket >= min_dirty_bucket - 100 的数据（更早的不需要）
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w, "
        "f.ps_sharpe_sum_r_10w, f.ps_sharpe_sum_r2_over_dt_10w, f.ps_sharpe_time_sum_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(7, i).GetValue<int64_t>(),
          hugeint_to_i128(prefix_sum_result->GetValue(8, i).GetValue<duckdb::hugeint_t>()),
          prefix_sum_result->GetValue(9, i).GetValue<int64_t>(),
      });
    }
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra) +
          core::mem::estimate_unordered_map(prefix_sum_history_by_pair_key, no_extra, [](const std::vector<PrefixSumHistoryRecord> &v) {
            return static_cast<int64_t>(v.capacity() * sizeof(PrefixSumHistoryRecord));
          });
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes + bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
          event_fact_row.account_exposure,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        int64_t sharpe_sum_r_10w = 0;
        __int128 sharpe_sum_r2_over_dt_10w = 0;
        int64_t sharpe_time_sum_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
        int64_t ps_sharpe_sum_r = 0;
        __int128 ps_sharpe_sum_r2_over_dt = 0;
        int64_t ps_sharpe_time_sum = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;
        // Sharpe = r̄ / σ，仅对 tag_id=-1（全账户）计算
        const double sharpe_10w = (key.tag_id == -1)
                                      ? calc_sharpe_from_returns(agg.sharpe_sum_r, agg.sharpe_sum_r2_over_dt, agg.sharpe_time_sum)
                                      : 0.0;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        int64_t prev_ps_sharpe_sum_r = 0, prev_ps_sharpe_time_sum = 0;
        __int128 prev_ps_sharpe_sum_r2_over_dt = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
          prev_ps_sharpe_sum_r = prev.ps_sharpe_sum_r;
          prev_ps_sharpe_sum_r2_over_dt = prev.ps_sharpe_sum_r2_over_dt;
          prev_ps_sharpe_time_sum = prev.ps_sharpe_time_sum;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
            prev_ps_sharpe_sum_r = prev_rec->ps_sharpe_sum_r;
            prev_ps_sharpe_sum_r2_over_dt = prev_rec->ps_sharpe_sum_r2_over_dt;
            prev_ps_sharpe_time_sum = prev_rec->ps_sharpe_time_sum;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);
        const int64_t ps_sharpe_sum_r =
            i64_narrow_checked(static_cast<__int128>(prev_ps_sharpe_sum_r) + agg.sharpe_sum_r);
        const __int128 ps_sharpe_sum_r2_over_dt = prev_ps_sharpe_sum_r2_over_dt + agg.sharpe_sum_r2_over_dt;
        const int64_t ps_sharpe_time_sum =
            i64_narrow_checked(static_cast<__int128>(prev_ps_sharpe_time_sum) + agg.sharpe_time_sum);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
            int64_t sharpe_sum_r = 0, sharpe_time_sum = 0;
            __int128 sharpe_sum_r2_over_dt = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            result.sharpe_sum_r = row.ps_sharpe_sum_r;
            result.sharpe_sum_r2_over_dt = row.ps_sharpe_sum_r2_over_dt;
            result.sharpe_time_sum = row.ps_sharpe_time_sum;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
            result.sharpe_sum_r = rec->ps_sharpe_sum_r;
            result.sharpe_sum_r2_over_dt = rec->ps_sharpe_sum_r2_over_dt;
            result.sharpe_time_sum = rec->ps_sharpe_time_sum;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);
        const int64_t sharpe_sum_r_100 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_sum_r) - boundary_100.sharpe_sum_r);
        const int64_t sharpe_sum_r_1000 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_sum_r) - boundary_1000.sharpe_sum_r);
        const __int128 sharpe_sum_r2_over_dt_100 = ps_sharpe_sum_r2_over_dt - boundary_100.sharpe_sum_r2_over_dt;
        const __int128 sharpe_sum_r2_over_dt_1000 = ps_sharpe_sum_r2_over_dt - boundary_1000.sharpe_sum_r2_over_dt;
        const int64_t sharpe_time_sum_100 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_time_sum) - boundary_100.sharpe_time_sum);
        const int64_t sharpe_time_sum_1000 =
            i64_narrow_checked(static_cast<__int128>(ps_sharpe_time_sum) - boundary_1000.sharpe_time_sum);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        // Sharpe 窗口投影，仅对 tag_id=-1（全账户）计算
        const double sharpe_100w = (key.tag_id == -1)
                                       ? calc_sharpe_from_returns(sharpe_sum_r_100, sharpe_sum_r2_over_dt_100, sharpe_time_sum_100)
                                       : 0.0;
        const double sharpe_1000w = (key.tag_id == -1)
                                        ? calc_sharpe_from_returns(sharpe_sum_r_1000, sharpe_sum_r2_over_dt_1000, sharpe_time_sum_1000)
                                        : 0.0;

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(agg.sharpe_sum_r);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.sharpe_sum_r2_over_dt)));
        ap.Append(agg.sharpe_time_sum);
        ap.Append(agg.sharpe_prev_block);
        ap.Append(agg.sharpe_prev_pnl);
        ap.Append(agg.sharpe_prev_exposure);
        ap.Append(agg.sharpe_pending_pnl);
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(ps_sharpe_sum_r);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(ps_sharpe_sum_r2_over_dt)));
        ap.Append(ps_sharpe_time_sum);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            agg.sharpe_sum_r,
            agg.sharpe_sum_r2_over_dt,
            agg.sharpe_time_sum,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
            ps_sharpe_sum_r,
            ps_sharpe_sum_r2_over_dt,
            ps_sharpe_time_sum,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3

```

```markdown
## 交易口径事件（符号驱动, 支持做空）
> `qty = abs(amount)`
> `dir = sign(amount)`（`+1 / -1 / 0`）
> `px = price_1e6 / 1e6`
> `cost_before = cost`（本事件前该token的cost）
> `pos_before = pos`（本事件前该token的pos）
> `has_usd = collateral ∈ {USDC(1), USDCe(2), USDT(3), WrappedUSDCe(4)}`

**Collateral 规则**: 只有 USD 类抵押物 (`has_usd=true`) 才计算 `cost / realized_delta / lp`；非 USD 仅更新 `pos`。  
**方向规则**: `EventType` 决定“经济语义”, `amount` 符号决定“方向”；同一 `EventType` 可出现正负。

| 事件族                                                     | `amount > 0` (正向腿)                                        | `amount < 0` (反向腿)                                         | realized 规则                                                | lp更新                   |
| ---------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------- | ------------------------------------------------------------ | ------------------------ |
| `Order* / FPMM* / Split* / Merge* / Redemption*`           | 先平空(`pos<0`)再开多；开多部分 `cost += open_long_qty * px` | 先平多(`pos>0`)再开空；开空部分 `cost -= open_short_qty * px` | 仅"平掉已有仓位"部分计 realized；开新仓部分不计当期 realized | 仅 `Order* / FPMM*` 更新 |
| `Convert`                                                  | 先平空再开多；开多部分 `cost += open_long_qty * convert_px`  | 先平多再开空；开空部分 `cost -= open_short_qty * convert_px`  | `convert_px = (popcount-1)/popcount`, 仅平仓部分计 realized  | 否                       |
| `TransferIn* / TransferOut* / FPMMLPRemove / FPMMLPReturn` | 先平空再开多；`cost` 不增加                                  | 先平多再开空；`cost` 不增加（保持 transfer 语义）             | 始终 `0`                                                     | 否                       |
| `FPMMLPAdd`                                                | 不改 `pos/cost`                                              | 不改 `pos/cost`                                               | `0`                                                          | 否                       |

`amount == 0` 视为 no-op（仅事件计数推进, 不改持仓/成本/PnL）。

## Unrealized PnL 计算
```
// 对该用户所有有持仓的token求和
unrealized_pnl = Σ(pos * lp / 1e6 - cost)  // 对所有 token_state 中 pos != 0 且 lp > 0 的token
```
- 只有 Order/FPMM 交易事件会更新 lp
- Split/Merge/Transfer 等不更新 lp
- 若某token从未交易过（如TransferIn获得）,该token不计入unrealized（lp=0）

## Flow 图

```text
stage3_bootstrap
└─ 初始化 schema 与索引；若游标不存在则写入初始 cursor

stage3_sync_tick
├─ 0) 读取游标与元数据
│  ├─ 读 sync_cursor_state(id=1) -> SyncCursorState
│  ├─ 刷新 condition_meta(只读缓存)
│  └─ 计算批次游标起点 (sort_key_exclusive = cursor.sort_key)
├─ 1) 拉取输入事件(Stage2 输出, RocksDB sort 索引)
│  ├─ scan_by_sort_key(sort_key_exclusive=cursor.sort_key,
│  │                  sort_key_inclusive=head_block*1e9+(1e9-1),
│  │                  limit=batch_events)
│  ├─ 返回顺序按 key: (sort_key, user_addr, cond_idx, event_type, token_idx)
│  └─ 若无事件 -> 将 cursor.sort_key 推进到 head_sort_key 后结束本 tick
├─ 2) 预加载状态
│  ├─ 扫描 batch_events,提取 touched keys
│  │  ├─ touched_token_keys = {(user_addr, cond_idx, token_idx)}
│  │  └─ touched_users      = {user_addr}
│  ├─ 读 token_state  -> token_state_map
│  ├─ 读 user_summary_state -> user_state_map
│  └─ 初始化临时结构
│     ├─ fact_rows
│     └─ dirty_token_keys / dirty_users
├─ 3) 回放循环(for evt in batch_events, 按 sort_key + PK 升序)
│  ├─ 3.1 输入断言
│  │  ├─ (sort_key, user_addr, cond_idx, event_type, token_idx) 严格递增
│  │  ├─ cond_idx < 0: 不进入状态机(仅事实行)
│  │  ├─ cond_idx >= 0: condition_meta 必须存在
│  │  ├─ token_idx ∈ [0, outcome_count)
│  │  ├─ event_type 合法；amount 符号不做类型硬断言（允许同类型正负共存）
│  │  ├─ amount==0 允许（no-op）
│  │  └─ 常规指标使用 double 计算并 round() 写入 int64；大数累计项直接写 HUGEINT
│  ├─ 3.2 路由
│  │  ├─ 方向由 amount 符号决定:正向腿(amount>0) / 反向腿(amount<0)
│  │  ├─ 事件族决定经济语义:price类 / convert类 / transfer类 / lp_add类
│  │  └─ LP添加: FPMMLPAdd(仅记录,不改pos/cost)
│  ├─ 3.3 应用规则
│  │  ├─ 公共计算
│  │  │  ├─ qty = abs(amount), px = price_1e6/1e6
│  │  │  └─ amount==0: no-op
│  │  ├─ 正向腿(amount>0): 先平空(pos<0)再开多
│  │  │  ├─ price/convert 类: 开多部分增加 cost
│  │  │  └─ transfer/LP-return 类: 开多部分不增加 cost
│  │  ├─ 反向腿(amount<0): 先平多(pos>0)再开空
│  │  │  ├─ price/convert 类: 开空部分记负 cost（入场信用）
│  │  │  └─ transfer/LP-return 类: 开空部分不改 cost
│  │  ├─ realized 仅在“平掉已有仓位”时产生
│  │  │  ├─ price 类: realized = closed_qty * px - cost_removed
│  │  │  └─ convert 类: realized = closed_qty * ((popcount-1)/popcount) - cost_removed
│  │  └─ FPMMLPAdd: 不改 token pos/cost; realized_delta=0
│  ├─ 3.4 更新 token state
│  │  ├─ 更新 pos, cost, lp (若Order/FPMM且price>0)
│  │  └─ 更新 entry_block:
│  │       建仓 (pos: 0 → 非0):    entry_block = current_block
│  │       加仓 (|pos| 增加):      entry_block = (|old_pos| * old_entry + delta * current_block) / |new_pos|
│  │       减仓 (|pos| 减少):      entry_block 不变
│  │       平仓 (pos → 0):         删除该行
│  ├─ 3.5 计算 user 级别聚合
│  │  ├─ unrealized_pnl = Σ(pos * lp / 1e6 - cost) for all tokens
│  │  ├─ token_count = 有效持仓token数（过滤dust, |qty| >= 10 token）
│  │  └─ 计算 exposure, holding_period (token级别)
│  ├─ 3.6 更新 user_state_map[user_addr]
│  │  ├─ total_events += 1
│  │  ├─ total_realized_pnl += realized_delta
│  │  └─ last_sort_key = evt.sort_key
│  ├─ 3.7 产出事实行
│  │  └─ append EventFact(user_addr, sort_key, cond_idx, token_idx, event_type,
│  │                         realized_delta, realized_cum, unrealized_pnl, token_count,
│  │                         tag_id, exposure, volume, holding_period)
│  └─ 3.8 统一特征图更新 (按 bucket 增量)
│     ├─ 将事件映射为 (user_addr, tag_id, block_bucket) dirty key
│     ├─ 更新该 bucket 的 Node-A0 续算锚点 + Node-A 原子统计节点
│     ├─ 依赖前序 bucket 前缀节点, 计算 100w/1000w 窗口节点
│     └─ 产出并覆盖 feature_tensor_state 对应行
├─ 4) 批次收尾校验
│  ├─ token 状态有限: pos/cost 必须是 finite（允许负数,表示短仓/负成本）
│  ├─ fact_rows.size == batch_events.size
│  └─ 任一失败 -> rollback,cursor 不推进
├─ 5) 单事务提交
│  ├─ upsert token_state(dirty_token_keys)
│  ├─ upsert user_summary_state(dirty_users)
│  ├─ write event_fact.rocks/timeline(fact_rows)
│  ├─ upsert feature_tensor_state(dirty_user_bucket_tag, 含 Node-A0/A/B/C/D)
│  └─ update sync_cursor_state(sort_key, processed_events)
└─ 6) 查询路径(只读)
   ├─ users_sorted(limit)                 -> user_summary_state(用户列表)
   ├─ user_pnl_timeline(user)             -> 预热并缓存该用户 timeline + snapshots
   ├─ user_positions_at(user, sk)         -> 命中内存 snapshots 返回 positions
   └─ user_feature_filter(anchor, exprs)  -> feature_tensor_state(截面 top users)
```

## 数据结构

符号: `E=事件总数`, `U=有事件用户数`, `T=活跃token数(user,cond,token_idx)`, `F≈E/20`  
Size估算基准: E=2B, U=2M, T=1M, F=100M

| 数据结构                                      | 层级/实例数       | 行大小 | Size   | 主要用途                                          | Persist      | ORDER BY                                             |
| --------------------------------------------- | ----------------- | ------ | ------ | ------------------------------------------------- | ------------ | ---------------------------------------------------- |
| `EventInput`                                  | 输入流 / `E`      | -      | -      | 回放状态机输入                                    | 否（临时）   | -                                                    |
| `ConditionMeta`                               | 只读缓存 / `C`    | -      | -      | `outcome_count` 与 `tag_id` 元信息                | 否（可重载） | -                                                    |
| `TokenState` (`token_state`)                  | Token级 / `T`     | 60B    | ~60MB  | 当前持仓 `pos/cost/lp/entry_block`                | 是           | `(user_addr,cond_idx,token_idx)`                     |
| `EventFact` (`event_fact.rocks/timeline`)     | Token级 / `E`     | 96B    | ~190GB | 事件事实、timeline、特征原料                      | 是           | `(user_addr,sort_key,cond_idx,event_type,token_idx)` |
| `UserSummaryState` (`user_summary_state`)     | User级 / `U`      | 60B    | ~120MB | 用户总览查询加速                                  | 是           | `(user_addr)`                                        |
| `FeatureTensorState` (`feature_tensor_state`) | User*Bucket*Tag级 | 536B   | ~50GB  | 统一特征张量（含原子/窗口/前缀缓存/增量续算锚点） | 是           | `(block_bucket,tag_id,user_addr)`                    |
| `SyncCursorState` (`sync_cursor_state`)       | 全局 / `1`        | <1KB   | <1KB   | 增量同步断点                                      | 是           | -                                                    |
| `UserQueryCache`                              | 进程内 / `<=U`    | -      | -      | 查询缓存（timeline/snapshot）                     | 否（内存）   | -                                                    |

```text
// Stage3 内部统一输入结构 (用于回放/状态机)
struct EventInput {
  string user_hex;        // 查询路径可为空, 批处理路径必填
  int64  sort_key;
  int32  cond_idx;
  int32  event_type;
  int32  token_idx;
  int32  collateral;
  int64  amount;          // signed, 1e6
  int64  price_1e6;
}

// Condition 元数据 (只读缓存)
struct ConditionMeta {
  uint8 outcome_count;
  int64 payout_numerators[256];  // 支持任意outcome数
  int8 tag_id;                   // 行业分类
}

// Token 状态 (稀疏存储, 只存有持仓的token)
// PK: (user_addr, cond_idx, token_idx)
struct TokenState {
  double pos;           // 持仓量 (可负=空头)
  double cost;          // 成本基础 (可负=空头信用)
  double lp;            // 最近成交价
  double entry_block;   // 加权平均建仓block
}

// 事件事实行 (持久化, 用于构建 timeline)
struct EventFact {
  Address20 user_addr;
  int64   sort_key;
  int32   cond_idx;
  int32   token_idx;
  int32   event_type;
  int64   realized_delta;     // 本事件 realized 增量
  int64   realized_cum;       // user级别累计 realized pnl
  int64   unrealized_pnl;     // user级别 unrealized pnl
  int32   token_count;        // user级别有效持仓 token 种数（|qty| >= 10 token）
  // 新增字段
  int8    tag_id;             // 行业分类 (0-13)
  int64   exposure;           // 该token暴露额 |pos*lp|
  int64   volume;             // 交易额 |amount*price|
  int64   holding_period;     // 该token持仓周期 (blocks)
}

// 用户摘要 (持久化)
struct UserSummaryState {
  int64  total_events;
  int64  total_realized_pnl;
  int64  total_unrealized_pnl;
  int32  active_tokens;       // 有持仓的token数
  int64  last_sort_key;
}

// 统一特征张量状态 (持久化)
// PK: (user_addr, block_bucket, tag_id)
// 注: tag_id = -1 表示全行业聚合行（用于总夏普等 1 维特征）
struct FeatureTensorState {
  Address20 user_addr;
  int64  block_bucket;
  int32  tag_id;

  // Node-A0: 增量续算锚点
  // 持仓 tail 修正 + Sharpe block-merge 续算, 无需回扫 event_fact
  int64  last_sort_key_10w;
  int64  last_block_10w;              // 当前 pending block
  int64  last_exposure_10w;           // 当前 pending exposure
  int128 last_holding_period_10w;        // DuckDB: HUGEINT
  int64  last_token_count_10w;
  int64  sharpe_prev_block_10w;       // 上一个已完成采样点 block
  int64  sharpe_prev_pnl_10w;         // 上一个已完成采样点 pnl
  int64  sharpe_prev_exposure_10w;    // 上一个已完成采样点 exposure
  int64  sharpe_pending_pnl_10w;      // 当前 pending block 的块末 pnl

  // Node-A: 10w 原子统计（事件增量累加）
  int64  time_weight_sum_10w;
  int64  token_count_tw_sum_10w;
  int128 exposure_tw_sum_10w;            // DuckDB: HUGEINT
  int64  volume_sum_10w;
  int128 holding_period_exp_tw_sum_10w;  // DuckDB: HUGEINT
  int64  sharpe_sum_r_10w;               // Σr_i, r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
  int128 sharpe_sum_r2_over_dt_10w;      // Σ(r_i² / Δt_i), DuckDB: HUGEINT
  int64  sharpe_time_sum_10w;            // ΣΔt_i

  // Node-B: 10w 归一化输出
  int64  token_avg_10w;
  int64  exposure_avg_10w;
  int64  volume_10w;
  int64  holding_period_avg_10w;
  float  sharpe_10w;

  // Node-C: 前缀缓存（按 bucket 单调推进）
  int64  ps_token_avg_10w;
  int64  ps_exposure_avg_10w;
  int64  ps_volume_10w;
  int64  ps_holding_period_avg_10w;
  int64  ps_sharpe_sum_r_10w;
  int128 ps_sharpe_sum_r2_over_dt_10w;   // DuckDB: HUGEINT
  int64  ps_sharpe_time_sum_10w;

  // Node-D: 窗口投影输出（由 Node-C O(1) 计算）
  int64  token_avg_100w;
  int64  token_avg_1000w;
  int64  exposure_avg_100w;
  int64  exposure_avg_1000w;
  int64  volume_avg_100w;
  int64  volume_avg_1000w;
  int64  holding_period_avg_100w;
  int64  holding_period_avg_1000w;
  float  sharpe_100w;
  float  sharpe_1000w;

  int64  updated_sort_key;
}

// 游标
struct SyncCursorState {
  int64 sort_key;
  int64 processed_events;
}

// 查询缓存（单用户）
struct UserQueryCache {
  string user_addr_lower;
  vector<TimelineRow> timeline;
  vector<{int64 sort_key; vector<PositionRow> positions;}> snapshots;
}
```

## API 设计

### GET /api/stage3-status
| entry                  | type   | required | description          |
| ---------------------- | ------ | -------- | -------------------- |
| query                  | object | 否       | 无参数               |
| resp.syncing           | bool   | 是       | 是否正在同步         |
| resp.last_block        | i64    | 是       | 已同步到的最新 block |
| resp.head_block        | i64    | 是       | 链上当前 head block  |
| resp.behind_blocks     | i64    | 是       | 落后 block 数        |
| resp.behind_chunks     | i64    | 是       | 预估剩余 chunk 数    |
| resp.blocks_per_second | f64    | 是       | 同步速度             |
| resp.eta_seconds       | f64    | 是       | 预计剩余秒数         |
| resp.ready             | bool   | 是       | 是否可用于查询       |

### GET /api/stage3-pnl?user=0x...
| entry                | type   | required | description                      |
| -------------------- | ------ | -------- | -------------------------------- |
| query.user           | string | 是       | 用户地址                         |
| resp.user            | string | 是       | 用户地址                         |
| resp.block           | i64    | 是       | 该用户最新事件所在 block         |
| resp.total_events    | i64    | 是       | 该用户总事件数                   |
| resp.timeline[]      | array  | 是       | 全量事件序列（不因 cursor 截断） |
| resp.timeline[].sk   | i64    | 是       | sort_key（`block*1e9+log_idx`）  |
| resp.timeline[].ty   | u8     | 是       | 事件类型                         |
| resp.timeline[].rpnl | i64    | 是       | 累计已实现 PnL                   |
| resp.timeline[].upnl | i64    | 是       | 该时刻未实现 PnL                 |
| resp.timeline[].tk   | i32    | 是       | 有效持仓 token 种数（过滤dust）  |
| error.404            | object | 条件     | 用户不存在或无事件时返回         |

### GET /api/stage3-positions?user=0x...&sort_key=...
| entry                 | type   | required | description                            |
| --------------------- | ------ | -------- | -------------------------------------- |
| query.user            | string | 是       | 用户地址                               |
| query.sort_key        | i64    | 是       | 目标 cursor                            |
| resp.user             | string | 是       | 用户地址                               |
| resp.sort_key         | i64    | 是       | 回显请求的 cursor                      |
| resp.block            | i64    | 是       | 快照所在 block                         |
| resp.positions[]      | array  | 是       | 指定 cursor 的持仓快照（仅 positions） |
| resp.positions[].ci   | u32    | 是       | cond_idx                               |
| resp.positions[].ti   | u8     | 是       | token_idx                              |
| resp.positions[].qty  | i64    | 是       | 持仓数量（`1e6=1 token`, 可负）        |
| resp.positions[].cost | i64    | 是       | 成本基础（`1e6=$1`, 可负）             |
| resp.positions[].lp   | i64    | 是       | 最近成交价（`1e6=$1`）                 |
| resp.positions[].eb   | i64    | 是       | 建仓block                              |

### POST /api/stage3-filter
| entry                   | type     | required | description                                    |
| ----------------------- | -------- | -------- | ---------------------------------------------- |
| req.anchor_bucket       | i64      | 是       | 锚定 bucket（必须显式传入）                    |
| req.filters[]           | string[] | 否       | 过滤表达式列表（按 `AND` 连接）                |
| req.sort_expr           | string   | 是       | 排序表达式                                     |
| req.sort_asc            | bool     | 是       | 排序方向（必须显式传入）                       |
| req.limit               | i32      | 是       | 返回条数（必须显式传入，当前实现固定传 `100`） |
| resp.anchor_bucket      | i64      | 是       | 实际执行使用的 anchor bucket                   |
| resp.users[]            | array    | 是       | 过滤 + 排序后的用户列表                        |
| resp.users[].addr       | string   | 是       | 用户地址                                       |
| resp.users[].sort_value | f64      | 是       | 排序表达式值                                   |

## 建立动态高玩池(特征工程, 统一表 + Compute Graph)

10w blk ~ 2.3day ~ 日线  
100w blk ~ 23day ~ 月线  
1000w blk ~ 230day ~ 年线

特征张量索引: `User * Time(per 10w bucket) * Feature`

行业(N):
Crypto_Price
Crypto_Market
Sports_Basketball
Sports_Football
Sports_Soccer
Sports_Individual
Politics_US
Politics_World
Economy_Finance
Tech
Entertainment
Weather
Society
Unknown

时序特征(名称,数量,统计方式,描述):
注: 数量 N+1 表示 N 个行业 + 1 个综合(tag_id=-1)
    近期平均持仓token数        N+1  时间加权           近10w块移动平均持仓token数(分行业+综合)
    近月平均持仓token数        N+1  等权               近100w块移动平均持仓token数(分行业+综合)
    近年平均持仓token数        N+1  等权               近1000w块移动平均持仓token数(分行业+综合)
    近期平均持仓暴露额         N+1  时间加权           近10w块移动平均持仓暴露额(分行业+综合)
    近月平均持仓暴露额         N+1  等权               近100w块移动平均持仓暴露额(分行业+综合)
    近年平均持仓暴露额         N+1  等权               近1000w块移动平均持仓暴露额(分行业+综合)
    近期总和交易额             N+1  加总               近10w块总和交易额(分行业+综合)
    近月平均交易额             N+1  等权               近100w块平均交易额(分行业+综合)
    近年平均交易额             N+1  等权               近1000w块平均交易额(分行业+综合)
    近期平均持仓周期           N+1  时间,金额加权      近10w块移动平均持仓周期(分行业+综合)
    近月平均持仓周期           N+1  等权               近100w块移动平均持仓周期(分行业+综合)
    近年平均持仓周期           N+1  等权               近1000w块移动平均持仓周期(分行业+综合)
    近期夏普                   1    精确计算           近10w块全账户夏普(仅全账户)
    近月夏普                   1    精确计算           近100w块全账户夏普(仅全账户)
    近年夏普                   1    精确计算           近1000w块全账户夏普(仅全账户)
截面特征:
    输入: anchor bucket + 多条自由表达式(filters) + 排序表达式(sort_expr)
    输出: 过滤 + 排序后的 top 100 用户地址
    目标: 支持“分行业 nested 特征”的横截面对比与选人

注:
  1. 交易额里: 只记录会直接创造头寸暴露的操作(比如铸币, 合币就不应该记入), 暴露方向不重要
  2. 平均持仓: 需要统计周期内多个事件(非均匀)的持仓快照(记录不同token的平均持仓周期), 再按照token金额, 事件时间加权
  3. 夏普: 无风险=0, 基于账户级 `pnl/exposure` 的 block-merge 增量计算（与其他特征一致动态维护）
     - 仅统计全账户 Sharpe（tag_id=-1），不支持分行业 Sharpe
     - `sharpe_10w / sharpe_100w / sharpe_1000w` 最终统一 normalize 到 `1000w block` 量纲
     - 采样点: 同一 block 内多事件先合并，只取块末 `(block, pnl, account_exposure)`
     - 其中: `pnl = realized_cum + unrealized_pnl`
     - 相邻采样点区间:
       - `Δpnl_i = pnl_i - pnl_{i-1}`
       - `Δt_i = block_i - block_{i-1}`
       - `cap_i = RMS(exp_{i-1}, exp_i) = sqrt((exp_{i-1}^2 + exp_i^2) / 2)`
       - `r_i = Δpnl_i / cap_i`
     - 增量累加:
       - `sharpe_sum_r += r_i`
       - `sharpe_sum_r2_over_dt += r_i^2 / Δt_i`
       - `sharpe_time_sum += Δt_i`
     - 时间加权平均收益率: `μ = sharpe_sum_r / T`
     - 时间加权方差: `σ² = sharpe_sum_r2_over_dt / T - μ²`
     - raw Sharpe = `μ / σ`
     - 输出 Sharpe = `raw_sharpe * sqrt(10000000)`，即统一归一到 `1000w block`
     - `cap_i < 0.001 USD` 或 `Δt_i <= 0` 或 `T <= 0` 时，该区间/窗口 Sharpe 贡献记 0
  4. 本次 Sharpe 口径变更不做兼容迁移；切换代码后必须重建 `data/stage3`

```

normalize 已经接进去了：现在三个窗口的 Sharpe 都会统一乘 `sqrt(10000000)`，口径就是“1000W block 年化”。我再扫一遍 lints，确认这次小改没有引入新问题。

已经改成统一 `1000W block normalize` 了。

现在 `core-backend/src/stage3/stage3_sync_runtime.cpp` 里的 `calc_sharpe_from_returns()` 会先算 raw Sharpe，再统一乘 `sqrt(10000000)`，所以：

- `sharpe_10w`
- `sharpe_100w`
- `sharpe_1000w`

三者都落在同一个 `1000W block` 量纲上，可以直接横向比较。

`README_stage3.md` 我也同步改了，明确写成：

- 先算 raw Sharpe = `μ / σ`
- 最终输出 = `raw_sharpe * sqrt(10000000)`

我还检查了这次改动相关文件的 lints，目前没有新报错。

注意一点：这次只是统一了输出量纲，不会改变用户排序相对关系，只会把数值尺度放大到你想要的“年化/1000W”口径。你后面重建 `stage3` 数据后再看分布会更直观。

