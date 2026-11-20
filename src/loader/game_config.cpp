#include "scarlettx/loader/game_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace scarlettx {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

bool ExtractAttribute(const std::string& tag, const std::string& attr_name, std::string& out_value) {
    const std::string needle = attr_name + "=";
    size_t pos = tag.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos += needle.size();
    if (pos >= tag.size() || (tag[pos] != '"' && tag[pos] != '\'')) {
        return false;
    }
    const char quote = tag[pos++];
    const size_t end = tag.find(quote, pos);
    if (end == std::string::npos) {
        return false;
    }
    out_value = tag.substr(pos, end - pos);
    return true;
}

bool ExtractElementText(const std::string& xml, const std::string& element, std::string& out_text) {
    const std::string open_prefix = "<" + element;
    size_t start = xml.find(open_prefix);
    if (start == std::string::npos) {
        return false;
    }
    size_t gt = xml.find('>', start);
    if (gt == std::string::npos) {
        return false;
    }
    const std::string close_tag = "</" + element + ">";
    size_t close = xml.find(close_tag, gt + 1);
    if (close == std::string::npos) {
        return false;
    }
    out_text = Trim(xml.substr(gt + 1, close - (gt + 1)));
    return true;
}

bool ExtractTagBlock(const std::string& xml, const std::string& element, std::string& out_tag, size_t search_from = 0, size_t* out_next = nullptr) {
    const std::string open_prefix = "<" + element;
    size_t pos = xml.find(open_prefix, search_from);
    while (pos != std::string::npos) {
        const size_t after = pos + open_prefix.size();
        if (after < xml.size() && (std::isspace(static_cast<unsigned char>(xml[after])) || xml[after] == '>' || xml[after] == '/')) {
            size_t gt = xml.find('>', after);
            if (gt == std::string::npos) {
                return false;
            }
            out_tag = xml.substr(pos, gt - pos + 1);
            if (out_next) {
                *out_next = gt + 1;
            }
            return true;
        }
        pos = xml.find(open_prefix, after);
    }
    return false;
}

}  // namespace

const GameExecutableEntry* GameConfig::PrimaryExecutable() const noexcept {
    for (const auto& exe : executables) {
        const std::string tdf = ToLower(exe.target_device_family);
        if (tdf == "xboxgen9" || tdf == "scarlett") {
            return &exe;
        }
    }
    if (!executables.empty()) {
        return &executables.front();
    }
    return nullptr;
}

Status ParseGameConfigXml(const std::string& xml_content, GameConfig& out_config) {
    out_config = GameConfig{};
    if (xml_content.find("<Game") == std::string::npos) {
        return Status::Error(StatusCode::ParseError, "Missing <Game> root element in MicrosoftGame.Config");
    }

    std::string game_tag;
    if (ExtractTagBlock(xml_content, "Game", game_tag)) {
        ExtractAttribute(game_tag, "configVersion", out_config.config_version);
    }

    std::string identity_tag;
    if (ExtractTagBlock(xml_content, "Identity", identity_tag)) {
        ExtractAttribute(identity_tag, "Name", out_config.identity_name);
        ExtractAttribute(identity_tag, "Publisher", out_config.publisher);
        ExtractAttribute(identity_tag, "Version", out_config.version);
    }

    ExtractElementText(xml_content, "TitleId", out_config.title_id);

    std::string shell_tag;
    if (ExtractTagBlock(xml_content, "ShellVisuals", shell_tag)) {
        ExtractAttribute(shell_tag, "DefaultDisplayName", out_config.display_name);
    }

    size_t cursor = 0;
    std::string exe_tag;
    while (ExtractTagBlock(xml_content, "Executable", exe_tag, cursor, &cursor)) {
        GameExecutableEntry entry;
        ExtractAttribute(exe_tag, "Name", entry.name);
        ExtractAttribute(exe_tag, "Id", entry.id);
        ExtractAttribute(exe_tag, "TargetDeviceFamilyForXboxOne", entry.target_device_family);
        if (entry.target_device_family.empty()) {
            ExtractAttribute(exe_tag, "TargetDeviceFamily", entry.target_device_family);
        }
        ExtractAttribute(exe_tag, "Alias", entry.alias);
        std::string dev_only;
        if (ExtractAttribute(exe_tag, "IsDevOnly", dev_only)) {
            entry.is_dev_only = (ToLower(dev_only) == "true");
        }
        if (!entry.name.empty()) {
            out_config.executables.push_back(std::move(entry));
        }
    }

    // Determine ConsoleTarget from executable TargetDeviceFamily or package metadata.
    const std::string lower_xml = ToLower(xml_content);
    bool has_gen9 = false;
    bool has_gen9_aware = false;
    bool has_xbox_one = false;

    for (const auto& exe : out_config.executables) {
        const std::string tdf = ToLower(exe.target_device_family);
        if (tdf == "xboxgen9" || tdf == "scarlett") {
            has_gen9 = true;
        } else if (tdf == "xboxonegen9aware") {
            has_gen9_aware = true;
        } else if (tdf == "xboxone" || tdf == "durango") {
            has_xbox_one = true;
        }
    }

    if (has_gen9 || lower_xml.find("xboxgen9") != std::string::npos ||
        lower_xml.find("gaming.xbox.scarlett.x64") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxGen9Scarlett;
    } else if (has_gen9_aware || lower_xml.find("xboxonegen9aware") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxOneGen9Aware;
    } else if (has_xbox_one || lower_xml.find("xboxone") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxOne;
    }

    if (out_config.executables.empty()) {
        return Status::Error(StatusCode::ParseError, "MicrosoftGame.Config contains no <Executable> entries");
    }

    return Status::Ok();
}

Status LoadGameConfigFile(const std::string& file_path, GameConfig& out_config) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Cannot open MicrosoftGame.Config: " + file_path);
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    Status st = ParseGameConfigXml(oss.str(), out_config);
    if (st.ok()) {
        out_config.source_path = file_path;
    }
    return st;
}

}  // namespace scarlettx
