#include "scanner.h"

#include <vector>
#include <filesystem>
#include <string>
#include <sstream>
#include <ranges>

#include <fmt/core.h>

#include "util/Instrumentor.h"
#include "tvdb_api/tvdb_models.h"

#include "se_regex.h"
#include "file_intent.h"

// for debugging the scanning function
//#include "util/console_colours.h"

namespace app {

namespace fs = std::filesystem;

// create filename from given parameters
// {title}-S{season:02d}E{episode:02d}-{name}.[TAG1][TAG2].ext
template <typename T>
std::string create_filename(
    const std::string &title, int season, int episode, 
    const std::string &name, const std::string &ext, 
    T &tags)
{
    std::stringstream ss;
    ss << fmt::format("{}-S{:02d}E{:02d}", title, season, episode);
    if (name.size() > 0) {
        ss << "-" << name;
    }

    // create tag group
    bool created_prepend = false;
    for (auto &tag: tags) {
        // prepend delimiter if there are tags
        if (!created_prepend) {
            ss << ".";
            created_prepend = true;
        }
        ss << '[' << tag << ']';
    }
    ss << "." << ext;

    return ss.str();
}

std::vector<FileIntent> scan_directory(
    const fs::path &root, 
    const tvdb_api::TVDB_Cache &cache, 
    const RenamingConfig &cfg) 
{
    PROFILE_FUNC();

    const auto &ep_table = cache.episodes;
    std::vector<FileIntent> intents;

    if (!fs::is_directory(root)) {
        return intents;
    }

    // get required actions into folder diff
    auto dir = fs::recursive_directory_iterator(root);
    for (auto &e: dir) {
        if (!fs::is_regular_file(e)) {
            continue;
        }


        PROFILE_SCOPE("File processing");

        const auto &path = e.path();
        const auto &old_rel_path = path.lexically_relative(root);
        const auto &filename_path = path.filename();

        const auto filename = filename_path.string();
        const auto old_rel_path_str = old_rel_path.string();

        // for each file determine action to perform, given cache
        const auto ext = filename_path.extension().string();

        FileIntent intent;
        intent.src = old_rel_path_str;
        intent.action = FileIntent::Action::IGNORE;
        intent.is_active = false;
        intent.is_conflict = false;

        // delete if blacklisted extension
        PROFILE_MANUAL_BEGIN(blacklist);
        bool is_invalid_ext = std::find_if(
            cfg.blacklist_extensions.begin(), 
            cfg.blacklist_extensions.end(), 
            [&ext](const std::string &invalid_ext) { 
                return ext.compare(invalid_ext) == 0;
            }) != cfg.blacklist_extensions.end();

        if (is_invalid_ext) {
            // std::cout << FMAG("[D] ") << old_rel_path << std::endl;
            intent.action = FileIntent::Action::DELETE;
            intent.is_active = false;
            intents.push_back(intent);
            continue;
        }
        PROFILE_MANUAL_END(blacklist);

        // ignore if in a special folder or whitelisted filename
        PROFILE_MANUAL_BEGIN(whitelist_check);
        bool is_whitelisted = std::find_if(
            cfg.whitelist_folders.begin(),
            cfg.whitelist_folders.end(),
            [&old_rel_path_str] (const std::string &folder) {
                return old_rel_path_str.compare(0, folder.size(), folder) == 0;
            }) != cfg.whitelist_folders.end();

        is_whitelisted = is_whitelisted || std::find_if(
            cfg.whitelist_files.begin(),
            cfg.whitelist_files.end(),
            [&filename] (const std::string &other) {
                return filename.compare(0, other.size(), other) == 0;
            }) != cfg.whitelist_files.end();
        
        if (is_whitelisted) {
            // std::cout << FCYN("[I] ") << old_rel_path << std::endl;
            intent.action = FileIntent::Action::WHITELIST;
            intent.is_active = false;
            intents.push_back(intent);
            continue;
        }
        PROFILE_MANUAL_END(whitelist_check);

        // try to rename
        PROFILE_MANUAL_BEGIN(rename_block);

        auto mr = app::find_se_match(filename.c_str(), app::SE_REGEXES);
        // ignore if cant rename
        if (!mr) {
            // std::cout << FCYN("[I] ") << old_rel_path << std::endl;
            intent.action = FileIntent::Action::IGNORE;
            intent.is_active = false;
            intents.push_back(intent);
            continue;
        }

        PROFILE_MANUAL_END(rename_block);

        // if able to rename, it is a pending rename
        PROFILE_MANUAL_BEGIN(pending_block);

        const auto &match = mr.value();
        tvdb_api::EpisodeKey key { match.season, match.episode };

        const auto new_parent_dir = root / fmt::format("Season {:02d}", key.season);

        // create tags
        auto valid_tags = match.tags | std::views::filter([&cfg](const std::string &tag) {
            return std::find_if(
                cfg.whitelist_tags.begin(),
                cfg.whitelist_tags.end(),
                [&tag] (const std::string &valid_tag) {
                    return tag.compare(0, valid_tag.size(), valid_tag) == 0;
                }) != cfg.whitelist_tags.end();
        });

        std::string dest_fn;
        // if unable to retrieve tvdb data, give generic rename
        if (ep_table.find(key) == ep_table.end()) {
            PROFILE_SCOPE("pending_block::no_metadata");
            dest_fn = create_filename(
                app::clean_se_title(cache.series.name),
                match.season,
                match.episode,
                "",
                match.ext,
                valid_tags
            );
        // rename according to the metadata
        } else {
            PROFILE_SCOPE("pending_block::metadata");
            auto &ep = ep_table.at(key);
            dest_fn = create_filename(
                app::clean_se_title(cache.series.name),
                match.season,
                match.episode,
                app::clean_se_name(ep.name),
                match.ext,
                valid_tags
            );
        }
        PROFILE_MANUAL_END(pending_block);

        PROFILE_MANUAL_BEGIN(complete_check);
        auto new_path = (new_parent_dir / dest_fn);
        auto new_rel_path = new_path.lexically_relative(root);
        auto new_rel_path_str = new_rel_path.string();

        if (old_rel_path == new_rel_path) {
            // std::cout << FGRN("[C] ") << old_rel_path_str << std::endl;
            intent.action = FileIntent::Action::COMPLETE;
            intent.is_active = false;
            intents.push_back(intent);
        } else {
            // std::cout << FYEL("[R] ") << old_rel_path_str << " ==> "  << new_rel_path_str << std::endl;
            intent.action = FileIntent::Action::RENAME;
            intent.dest = new_rel_path_str;
            intent.is_active = true;
            intents.push_back(intent);
        }
        PROFILE_MANUAL_END(complete_check);
    }

    return intents;
}

};