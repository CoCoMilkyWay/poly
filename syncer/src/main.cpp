#include <iostream>
#include <string>
#include <cstring>
#include "config.hpp"
#include "db.hpp"
#include "syncer.hpp"

void print_usage(const char* prog) {
    std::cout << "用法: " << prog << " --config <config.json>" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string config_path = "config.json";
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    std::cout << "========================================" << std::endl;
    std::cout << "    Polymarket Data Syncer" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // 加载配置
    std::cout << "[Main] 加载配置: " << config_path << std::endl;
    Config config = Config::load(config_path);
    
    std::cout << "[Main] API Key: " << config.api_key.substr(0, 8) << "..." << std::endl;
    std::cout << "[Main] DB Path: " << config.db_path << std::endl;
    std::cout << "[Main] Subgraphs: " << config.subgraphs.size() << " 个" << std::endl;
    
    // 初始化数据库
    std::cout << "[Main] 初始化数据库..." << std::endl;
    Database db(config.db_path);
    
    // 创建同步器并运行
    std::cout << "[Main] 启动同步器..." << std::endl;
    Syncer syncer(config, db);
    syncer.run();
    
    return 0;
}
