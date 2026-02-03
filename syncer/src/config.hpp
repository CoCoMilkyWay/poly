#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>
#include <cassert>

using json = nlohmann::json;

struct EntityConfig {
    std::string name;
};

struct SubgraphConfig {
    std::string id;
    std::string name;
    std::vector<std::string> entities;
};

struct Config {
    std::string api_key;
    std::string db_path;
    int sync_interval_seconds;
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
        
        for (const auto& sg : j["subgraphs"]) {
            SubgraphConfig sc;
            sc.id = sg["id"].get<std::string>();
            sc.name = sg["name"].get<std::string>();
            for (const auto& e : sg["entities"]) {
                sc.entities.push_back(e.get<std::string>());
            }
            config.subgraphs.push_back(sc);
        }
        
        return config;
    }
};
