CREATE TABLE IF NOT EXISTS pm_condition_static (
  -- ========= A. condition / market 主体（单值） =========
  id                            BIGINT NOT NULL,       -- 顶层 id
  condition_id                  BLOB PRIMARY KEY,      -- conditionId
  question_id                   BLOB NOT NULL,         -- questionID

  market_slug                   TEXT NOT NULL,         -- slug
  market_question               TEXT NOT NULL,         -- question
  market_description            TEXT,                  -- description

  market_start_date             TIMESTAMP,             -- startDate
  market_end_date               TIMESTAMP,             -- endDate
  market_created_at             TIMESTAMP,             -- createdAt
  market_image                  TEXT,                  -- image
  market_icon                   TEXT,                  -- icon

  market_submitted_by           BLOB,                  -- submitted_by
  market_resolved_by            BLOB,                  -- resolvedBy

  market_restricted             BOOLEAN,               -- restricted
  market_neg_risk               BOOLEAN NOT NULL,      -- negRisk
  market_neg_risk_request_id    TEXT,                  -- negRiskRequestID
  market_cyom                   BOOLEAN,               -- cyom

  market_group_item_title       TEXT,                  -- groupItemTitle
  market_group_item_threshold   TEXT,                  -- groupItemThreshold

  market_enable_order_book      BOOLEAN,               -- enableOrderBook
  market_order_min_size         DOUBLE,                -- orderMinSize
  market_order_min_tick         DOUBLE,                -- orderPriceMinTickSize
  market_clear_book_on_start    BOOLEAN,               -- clearBookOnStart
  market_manual_activation      BOOLEAN,               -- manualActivation
  market_automatically_active   BOOLEAN,               -- automaticallyActive

  market_uma_bond               TEXT,                  -- umaBond
  market_uma_reward             TEXT,                  -- umaReward

  market_rewards_min_size       DOUBLE,                -- rewardsMinSize
  market_rewards_max_spread     DOUBLE,                -- rewardsMaxSpread
  market_holding_rewards_enable BOOLEAN,               -- holdingRewardsEnabled
  market_rfq_enabled            BOOLEAN,               -- rfqEnabled
  market_fees_enabled           BOOLEAN,               -- feesEnabled
  market_fee_type               TEXT,                  -- feeType

  market_series_color           TEXT,                  -- seriesColor
  market_show_gmp_series        BOOLEAN,               -- showGmpSeries
  market_show_gmp_outcome       BOOLEAN,               -- showGmpOutcome

  -- ========= B. events[*]（独立 entry，按索引对齐） =========
  event_ids                     BIGINT[],              -- events[*].id
  event_tickers                 TEXT[],                -- events[*].ticker
  event_slugs                   TEXT[],                -- events[*].slug
  event_titles                  TEXT[],                -- events[*].title
  event_descriptions            TEXT[],                -- events[*].description
  event_resolution_sources      TEXT[],                -- events[*].resolutionSource
  event_start_dates             TIMESTAMP[],           -- events[*].startDate
  event_creation_dates          TIMESTAMP[],           -- events[*].creationDate
  event_end_dates               TIMESTAMP[],           -- events[*].endDate
  event_created_ats             TIMESTAMP[],           -- events[*].createdAt
  event_images                  TEXT[],                -- events[*].image
  event_icons                   TEXT[],                -- events[*].icon
  event_start_times             TIMESTAMP[],           -- events[*].startTime
  event_gmp_chart_modes         TEXT[],                -- events[*].gmpChartMode
  event_enable_order_books      BOOLEAN[],             -- events[*].enableOrderBook
  event_neg_risks               BOOLEAN[],             -- events[*].negRisk
  event_enable_neg_risks        BOOLEAN[],             -- events[*].enableNegRisk
  event_show_all_outcomes       BOOLEAN[],             -- events[*].showAllOutcomes
  event_show_market_images      BOOLEAN[],             -- events[*].showMarketImages
  event_auto_resolveds          BOOLEAN[],             -- events[*].automaticallyResolved
  event_auto_actives            BOOLEAN[],             -- events[*].automaticallyActive
  event_cyoms                   BOOLEAN[],             -- events[*].cyom
  event_requires_translations   BOOLEAN[],             -- events[*].requiresTranslation

  -- ========= C. tags[*]（独立 entry，按索引对齐） =========
  tag_ids                       BIGINT[],              -- tags[*].id
  tag_labels                    TEXT[],                -- tags[*].label
  tag_slugs                     TEXT[],                -- tags[*].slug
  tag_created_ats               TIMESTAMP[],           -- tags[*].createdAt

  -- ========= D. clobRewards[*]（独立 entry，按索引对齐） =========
  reward_ids                    BIGINT[],              -- clobRewards[*].id
  reward_condition_ids          BLOB[],                -- clobRewards[*].conditionId
  reward_asset_addresses        BLOB[],                -- clobRewards[*].assetAddress
  reward_start_dates            DATE[],                -- clobRewards[*].startDate
  reward_end_dates              DATE[],                -- clobRewards[*].endDate

  -- ========= E. 同步元信息 =========
  first_seen_block              BIGINT NOT NULL,
  first_seen_ms                 BIGINT NOT NULL
);