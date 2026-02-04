#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define PARALLEL_PER_SOURCE 4       // 每个 source 内最多并行 entity 数
#define PARALLEL_TOTAL 16           // 全局最大并发请求数
#define GRAPHQL_BATCH_SIZE 1000     // 每次请求的 limit
#define DB_FLUSH_THRESHOLD 5000     // 累积多少条刷入 DB

#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <cassert>
#include <nlohmann/json.hpp>

#include "config.hpp"
#include "db.hpp"
#include "entities.hpp"
#include "https_pool.hpp"

using json = nlohmann::json;

// 前向声明
class Puller;
class SourceScheduler;

// ============================================================================
// GraphQL 查询构建
// ============================================================================
namespace graphql {

inline std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            default:   result += c; break;
        }
    }
    return result;
}

inline std::string build_query(const char* plural, const char* fields,
                               const std::string& cursor, int limit) {
    std::ostringstream oss;
    oss << R"({"query":"query Q($limit:Int!){)" << plural;
    
    if (cursor.empty()) {
        // 首次查询：无 where 条件
        oss << R"((first:$limit,orderBy:id,orderDirection:asc){)";
    } else {
        // 增量查询：从 cursor 继续
        oss << R"((first:$limit,orderBy:id,orderDirection:asc,where:{id_gt:\")"
            << escape_json(cursor) << R"(\"}){)";
    }
    
    oss << fields << R"(}}","variables":{"limit":)" << limit << "}}";
    return oss.str();
}

inline std::string build_target(const std::string& subgraph_id) {
    return "/api/subgraphs/id/" + subgraph_id;
}

} // namespace graphql

// ============================================================================
// EntityPuller - 单个 entity 的拉取器
// ============================================================================
class EntityPuller {
public:
    EntityPuller(const std::string& subgraph_id, const std::string& source_name,
                 const entities::EntityDef* entity, Database& db, HttpsPool& pool,
                 SourceScheduler* scheduler)
        : source_name_(source_name)
        , entity_(entity)
        , db_(db)
        , pool_(pool)
        , scheduler_(scheduler)
        , target_(graphql::build_target(subgraph_id))
    {
        buffer_.reserve(DB_FLUSH_THRESHOLD);
    }
    
    void start(const std::string& api_key);
    void on_response(const std::string& body);
    
    bool is_done() const { return done_; }
    const char* name() const { return entity_->name; }
    int total_synced() const { return total_synced_; }

private:
    void send_request();
    void flush_buffer();
    
    // 配置
    std::string source_name_;
    const entities::EntityDef* entity_;
    Database& db_;
    HttpsPool& pool_;
    SourceScheduler* scheduler_;
    std::string target_;
    
    // 状态
    std::string api_key_;
    std::string cursor_;
    std::string last_cursor_;
    std::vector<std::string> buffer_;
    int total_synced_ = 0;
    bool done_ = false;
};

// ============================================================================
// SourceScheduler - 单个 source 的调度器
// ============================================================================
class SourceScheduler {
public:
    SourceScheduler(const SourceConfig& config, Database& db, HttpsPool& pool, 
                    Puller* puller, bool is_pnl)
        : source_name_(config.name)
        , db_(db)
        , pool_(pool)
        , puller_(puller)
    {
        auto entity_list = is_pnl ? entities::PNL_ENTITIES : entities::MAIN_ENTITIES;
        auto entity_count = is_pnl ? entities::PNL_ENTITY_COUNT : entities::MAIN_ENTITY_COUNT;
        
        for (const auto& name : config.entities) {
            auto* e = entities::find_entity(name.c_str(), entity_list, entity_count);
            if (e) {
                pullers_.emplace_back(config.subgraph_id, source_name_, e, db_, pool_, this);
                db_.init_entity(e);
            } else {
                std::cerr << "[Scheduler] 未知 entity: " << name << std::endl;
            }
        }
    }
    
    void start(const std::string& api_key);
    void on_entity_done(EntityPuller* puller);
    
    bool all_done() const {
        for (const auto& p : pullers_) {
            if (!p.is_done()) return false;
        }
        return true;
    }
    
    const std::string& name() const { return source_name_; }
    int active_count() const { return active_count_; }

private:
    void start_next();
    
    // 配置
    std::string source_name_;
    Database& db_;
    HttpsPool& pool_;
    Puller* puller_;
    
    // 状态
    std::string api_key_;
    std::vector<EntityPuller> pullers_;
    size_t next_idx_ = 0;
    int active_count_ = 0;
};

// ============================================================================
// Puller - 全局协调器
// ============================================================================
class Puller {
public:
    Puller(const Config& config, Database& db, HttpsPool& pool)
        : config_(config), db_(db), pool_(pool) 
    {
        db_.init_sync_state();
        
        // 预分配空间，避免向量重新分配导致 scheduler_ 指针失效
        schedulers_.reserve(config.sources.size());
        for (const auto& src : config.sources) {
            bool is_pnl = (src.name == "pnl");
            schedulers_.emplace_back(src, db_, pool_, this, is_pnl);
        }
    }
    
    void run(asio::io_context& ioc) {
        std::cout << "[Puller] 启动，共 " << schedulers_.size() << " 个 source" << std::endl;
        
        for (auto& s : schedulers_) {
            s.start(config_.api_key);
        }
        
        ioc.run();
        std::cout << "[Puller] 所有 source 同步完成" << std::endl;
    }
    
    bool try_acquire_slot() {
        if (total_active_ < PARALLEL_TOTAL) {
            ++total_active_;
            return true;
        }
        return false;
    }
    
    void release_slot() {
        --total_active_;
        for (auto& s : schedulers_) {
            if (!s.all_done() && s.active_count() < PARALLEL_PER_SOURCE) {
                s.on_entity_done(nullptr);
                break;
            }
        }
    }

private:
    const Config& config_;
    Database& db_;
    HttpsPool& pool_;
    std::vector<SourceScheduler> schedulers_;
    int total_active_ = 0;
};

// ============================================================================
// EntityPuller 实现
// ============================================================================

inline void EntityPuller::start(const std::string& api_key) {
    api_key_ = api_key;
    cursor_ = db_.get_cursor(source_name_, entity_->name);
    std::cout << "[Pull] " << source_name_ << "/" << entity_->name 
              << " 开始，cursor=" << (cursor_.empty() ? "(empty)" : cursor_.substr(0, 20) + "...") 
              << std::endl;
    send_request();
}

inline void EntityPuller::send_request() {
    std::string query = graphql::build_query(
        entity_->plural, entity_->fields, cursor_, GRAPHQL_BATCH_SIZE
    );
    
    pool_.async_post(target_, query, [this](std::string body) {
        on_response(body);
    });
}

inline void EntityPuller::flush_buffer() {
    if (buffer_.empty()) return;
    
    db_.atomic_insert_and_save_cursor(
        entity_->table, entity_->columns, buffer_,
        source_name_, entity_->name, last_cursor_
    );
    
    std::cout << "[Pull] " << source_name_ << "/" << entity_->name 
              << " flush " << buffer_.size() << " 条 (total=" << total_synced_ << ")" << std::endl;
    
    buffer_.clear();
}

inline void EntityPuller::on_response(const std::string& body) {
    // 请求失败
    if (body.empty()) {
        std::cerr << "[Pull] " << entity_->name << " 请求失败" << std::endl;
        flush_buffer();
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    // JSON 解析
    json j;
    try {
        j = json::parse(body);
    } catch (...) {
        std::cerr << "[Pull] " << entity_->name << " JSON 解析失败" << std::endl;
        flush_buffer();
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    // GraphQL 错误
    if (j.contains("errors")) {
        std::cerr << "[Pull] " << entity_->name << " GraphQL 错误: " << j["errors"].dump() << std::endl;
        flush_buffer();
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    // 响应格式错误
    if (!j.contains("data") || !j["data"].contains(entity_->plural)) {
        std::cerr << "[Pull] " << entity_->name << " 响应格式错误" << std::endl;
        flush_buffer();
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    auto& items = j["data"][entity_->plural];
    
    // 没有更多数据
    if (items.empty()) {
        flush_buffer();
        std::cout << "[Pull] " << source_name_ << "/" << entity_->name 
                  << " 完成，共 " << total_synced_ << " 条" << std::endl;
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    // 处理数据
    for (const auto& item : items) {
        std::string values = entity_->to_values(item);
        if (!values.empty()) {
            buffer_.push_back(std::move(values));
        }
    }
    
    last_cursor_ = items.back()["id"].get<std::string>();
    cursor_ = last_cursor_;
    total_synced_ += items.size();
    
    // 达到阈值则刷入
    if (buffer_.size() >= DB_FLUSH_THRESHOLD) {
        flush_buffer();
    }
    
    // 最后一批
    if (items.size() < GRAPHQL_BATCH_SIZE) {
        flush_buffer();
        std::cout << "[Pull] " << source_name_ << "/" << entity_->name 
                  << " 完成，共 " << total_synced_ << " 条" << std::endl;
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    // 继续请求
    send_request();
}

// ============================================================================
// SourceScheduler 实现
// ============================================================================

inline void SourceScheduler::start(const std::string& api_key) {
    api_key_ = api_key;
    std::cout << "[Scheduler] " << source_name_ << " 启动，共 " 
              << pullers_.size() << " 个 entity" << std::endl;
    
    while (next_idx_ < pullers_.size() && 
           active_count_ < PARALLEL_PER_SOURCE && 
           puller_->try_acquire_slot()) {
        pullers_[next_idx_].start(api_key_);
        ++active_count_;
        ++next_idx_;
    }
}

inline void SourceScheduler::on_entity_done(EntityPuller* puller) {
    if (puller != nullptr) {
        --active_count_;
        puller_->release_slot();
    }
    start_next();
}

inline void SourceScheduler::start_next() {
    while (next_idx_ < pullers_.size() && 
           active_count_ < PARALLEL_PER_SOURCE && 
           puller_->try_acquire_slot()) {
        pullers_[next_idx_].start(api_key_);
        ++active_count_;
        ++next_idx_;
    }
}
