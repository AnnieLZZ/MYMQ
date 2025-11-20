#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <map>
#include <shared_mutex>
#include <memory>
#include <fstream>
#include <stdexcept>
#include<iostream>
#include<algorithm>
#include <utility>
#include<filesystem>
#include<mutex>


class Config_manager{
public:
    Config_manager(const std::string& filename) : filename_(filename), ifs() {
        if (!reload()) {
            throw std::runtime_error("Failed to load configuration from " + filename);
        }
    }

    ~Config_manager() {
        if (ifs.is_open()) {
            ifs.close();
        }
    }

public:
    static  bool ensure_path_existed(const std::string& filename_){
        std::filesystem::path file_path(filename_);
        std::filesystem::path parent_path = file_path.parent_path();

        if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
            std::cerr << "父目录 '" << parent_path << "' 不存在，尝试创建...\n";
            try {
                std::filesystem::create_directories(parent_path);
                std::cerr << "父目录 '" << parent_path << "' 创建成功。\n";
            } catch (const std::filesystem::filesystem_error& e) {
                std::cerr << "错误：无法创建父目录 '" << parent_path << "': " << e.what() << '\n';
                return false;
            }
        }
        return 1;
    }

    bool reload() {
        std::string line;
        std::unique_lock<std::shared_mutex> ulock(mtx_map);

        map_config.clear();

        if (ifs.is_open()) {
            ifs.close();
        }


        ensure_path_existed(filename_);

        ifs.open(filename_);

        if (!ifs.is_open()) {
            std::cerr << "FILE '" << filename_ << "' not exists . Try to create it...\n";
            std::ofstream create_file_ofs(filename_);
            if (!create_file_ofs.is_open()) {
                std::cerr << "Error: Failed to create '" << filename_ << "'.\n";
                return false;
            }
            create_file_ofs.close();

            ifs.open(filename_);
            if (!ifs.is_open()) {
                std::cerr << "ERROR: Failed to open '" << filename_ << "' after creating it\n";
                return false;
            }
        }

        ifs.clear();
        ifs.seekg(0, std::ios::beg);

        while (std::getline(ifs, line)) {
            auto kv = trim(line);
            if (!kv.first.empty()) {
                map_config[kv.first] = kv.second;
            }
        }

        bool success = !ifs.bad() && (!ifs.fail() || ifs.eof());
        ifs.close();

        if (!success) {
            std::cerr << "Warning: An unexpected error occurred while reading file '" << filename_ << "'\n";
            if (ifs.bad()) {
                std::cerr << "A serious error occurred (badbit)\n";
            }
            if (ifs.fail() && !ifs.eof()) {
                std::cerr << "A read error (failbit) occurred, but the end of the file has not been reached\n";
            }
        } else {
//            std::cout << "File '" << filename_ << "' Reloaded successfully\n";
        }
        return success;
    }

    bool haskey(const std::string& key){
        std::shared_lock<std::shared_mutex> slock(mtx_map);
        auto it = map_config.find(key);
        return (it != map_config.end());
    }

    bool getbool(const std::string& key){
        std::shared_lock<std::shared_mutex> slock(mtx_map);
        auto it = map_config.find(key);
        if(it == map_config.end()){
            throw std::out_of_range("Key '" + key + "' not found in configuration.");
        }

        const std::string& val_str = it->second;
        if(val_str == "1" || val_str == "true"){
            return true;
        }
        else if(val_str == "0" || val_str == "false"){
            return false;
        }
        else {
            throw std::invalid_argument("Invalid boolean value for key '" + key + "': '" + val_str + "'. Expected '1', 'true', '0', or 'false'.");
        }
    }

    int getint(const std::string& key){
        std::shared_lock<std::shared_mutex> slock(mtx_map);
        auto it = map_config.find(key);
        if(it == map_config.end()){
            throw std::out_of_range("Key '" + key + "' not found in configuration.");
        }
        return std::stoi(it->second);
    }

    std::string getstring(const std::string& key){
        std::shared_lock<std::shared_mutex> slock(mtx_map);
        auto it = map_config.find(key);
        if(it == map_config.end()){
            throw std::out_of_range("Key '" + key + "' not found in configuration.");
        }
        return it->second;
    }

    size_t get_size_t(const std::string& key){
        return static_cast<size_t>(getull(key));
    }

    uint16_t get_uint16(const std::string& key){
        return static_cast<uint16_t>(getull(key));
    }
    long long getll(const std::string& key){
        std::shared_lock<std::shared_mutex> slock(mtx_map);
        auto it = map_config.find(key);
        if(it == map_config.end()){
            throw std::out_of_range("Key '" + key + "' not found in configuration.");
        }
        return std::stoll(it->second);
    }

    unsigned long long getull(const std::string& key){
        std::shared_lock<std::shared_mutex> slock(mtx_map);
        auto it = map_config.find(key);
        if(it == map_config.end()){
            throw std::out_of_range("Key '" + key + "' not found in configuration.");
        }
        return std::stoull(it->second);
    }

private:
    std::pair<std::string,std::string> trim(const std::string& sentence){
        std::string s(sentence) ;
        removeAllWhitespace(s);
        if(s.size()<2){
            return std::pair<std::string,std::string>{};
        }
        if(s[0]=='/'&&s[1]=='/'){
            return std::pair<std::string,std::string>{};
        }

        auto it_es = s.find('=');
        std::pair<std::string,std::string> pair;
        if(it_es != std::string::npos){
            pair.first = s.substr(0, it_es);
            pair.second = s.substr(it_es + 1);
        }
        return pair;
    }

    void removeAllWhitespace(std::string& s) {
        s.erase(std::remove_if(s.begin(), s.end(),
                               [](unsigned char c){ return std::isspace(c); }),
                s.end());
    }

private:
    std::shared_mutex mtx_map;
    std::map<std::string,std::string> map_config;
    std::string filename_;
    std::ifstream ifs;
};

#endif
