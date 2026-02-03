#pragma once

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <nlohmann/json.hpp>
#include <cassert>

using json = nlohmann::json;

struct SubgraphConfig {
    std::string id;      // 实际的 subgraph ID
    std::string name;    // subgraph 名称（用于日志和表名区分）
    std::vector<std::string> entities;
};

struct Config {
    std::string api_key;
    std::string db_path;
    int sync_interval_seconds;
    std::map<std::string, std::string> subgraph_ids;  // name -> id 映射
    std::vector<SubgraphConfig> subgraphs;
    
    static Config load(const std::string& path) {
        std::ifstream f(path);
        assert(f.is_open() && "无法打开配置文件");
        
        json j;
        f >> j;
        
        Config config;
        config.api_key = j["api_key"].get<std::string>();
        config.db_path = j["db_path"].get<std::string>();
        config.sync_interval_seconds = j["sync_interval_seconds"].get<int>();
        
        // 解析 subgraph_ids 映射表
        if (j.contains("subgraph_ids")) {
            for (auto& [key, value] : j["subgraph_ids"].items()) {
                config.subgraph_ids[key] = value.get<std::string>();
            }
        }
        
        // 解析 subgraphs
        for (const auto& sg : j["subgraphs"]) {
            SubgraphConfig sc;
            sc.name = sg["name"].get<std::string>();
            
            // 支持两种格式：
            // 1. 直接指定 "id": "xxx"
            // 2. 使用引用 "id_ref": "activity" (从 subgraph_ids 中查找)
            if (sg.contains("id")) {
                sc.id = sg["id"].get<std::string>();
            } else if (sg.contains("id_ref")) {
                std::string ref = sg["id_ref"].get<std::string>();
                assert(config.subgraph_ids.count(ref) && "subgraph_ids 中找不到引用的 ID");
                sc.id = config.subgraph_ids[ref];
            } else {
                assert(false && "subgraph 必须指定 id 或 id_ref");
            }
            
            for (const auto& e : sg["entities"]) {
                sc.entities.push_back(e.get<std::string>());
            }
            config.subgraphs.push_back(sc);
        }
        
        return config;
    }
    
    // 检查是否为占位符 ID（以 SELF_DEPLOY_ 开头）
    static bool is_placeholder_id(const std::string& id) {
        return id.find("SELF_DEPLOY_") == 0;
    }
    
    // 获取有效的 subgraphs（过滤掉占位符 ID）
    std::vector<SubgraphConfig> get_active_subgraphs() const {
        std::vector<SubgraphConfig> active;
        for (const auto& sg : subgraphs) {
            if (!is_placeholder_id(sg.id)) {
                active.push_back(sg);
            }
        }
        return active;
    }
};
