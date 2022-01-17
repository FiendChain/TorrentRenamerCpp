#pragma once

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <list>
#include <atomic>
#include <mutex>

#include "tvdb_api.h"
#include "se_regex.h"
#include "scanner.h"
#include "file_loading.h"
#include "ctpl_stl.h"

namespace app 
{

// App object for managing a folder
class SeriesFolder 
{
public:
    enum Status {
        UNKNOWN         = 1<<0, 
        PENDING_DELETES = 1<<1, 
        CONFLICTS       = 1<<2, 
        PENDING_RENAME  = 1<<3, 
        COMPLETED       = 1<<4, 
        EMPTY           = 1<<5,
    };
public:
    const std::filesystem::path m_path;
    app_schema_t &m_schema;
    RenamingConfig &m_cfg;

    // keep a mutex on members which are used in rendering and undergo mutation during actions
    // cache of tvdb data
    TVDB_Cache m_cache;
    bool m_is_info_cached;
    std::mutex m_cache_mutex;

    // set of current actions
    FolderDiff m_state;
    std::atomic<Status> m_status;
    std::mutex m_state_mutex;

    // errors accumulated from operations
    std::list<std::string> m_errors;
    std::mutex m_errors_mutex;

    // store the search result for a tvdb search query
    std::vector<SeriesInfo> m_search_result;
    std::mutex m_search_mutex;

    // keep track of whether we are busy 
    std::atomic<bool> m_is_busy;
    // use this to keep count of the global count of busy folders
    std::atomic<int> &m_global_busy_count;
public:
    SeriesFolder(
        const std::filesystem::path &path, 
        app_schema_t &schema, 
        RenamingConfig &cfg,
        std::atomic<int> &busy_count);

    bool load_cache_from_tvdb(uint32_t id, const char *token);
    bool load_cache_from_file();
    void update_state_from_cache();
    bool load_search_series_from_tvdb(const char *name, const char *token);
    bool execute_actions();

private:
    void push_error(std::string &str);
};

// Main app object which contains all folders
class App 
{
public:
    std::filesystem::path m_root;
    RenamingConfig m_cfg;
    app_schema_t m_schema;
    std::string m_token;

    std::list<SeriesFolder> m_folders;
    SeriesFolder *m_current_folder;


    ctpl::thread_pool m_thread_pool;
    std::atomic<int> m_global_busy_count;
public:
    App();
    void authenticate();
    void refresh_folders();
};

};