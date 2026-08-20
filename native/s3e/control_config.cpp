#include "control_config.hpp"

#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace codboz {
namespace {

std::string trim(std::string value) {
    auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool endsWithIgnoreCase(std::string_view value, std::string_view suffix) {
    if (value.size() < suffix.size())
        return false;
    return std::equal(
        suffix.rbegin(), suffix.rend(), value.rbegin(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}

} // namespace

ControlConfig ControlConfig::load(const std::string &path) {
    ControlConfig result;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "player_name" && validPlayerName(value)) {
            result.playerName_ = value;
        } else if (key == "multiplayer_server" && validMultiplayerServer(value)) {
            result.multiplayerServer_ = value;
        } else if (key == "voice_chat_enabled" && (value == "0" || value == "1")) {
            result.voiceChatEnabled_ = value == "1";
        }
    }
    return result;
}

std::string ControlConfig::resolveHostname(std::string_view hostname) const {
    constexpr std::string_view kDemonwareSuffix = ".demonware.net";
    if (onlineEnabled() && endsWithIgnoreCase(hostname, kDemonwareSuffix)) {
        return multiplayerServer_;
    }
    return std::string(hostname);
}

bool ControlConfig::validPlayerName(std::string_view value) {
    if (value.empty() || value.size() > 13)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == ' ' || c == '.' || c == '_' || c == '-';
    });
}

bool ControlConfig::validMultiplayerServer(std::string_view value) {
    if (value.empty())
        return true;
    if (value.size() > 253 || value.front() == '.' || value.back() == '.')
        return false;

    const bool numeric = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || c == '.';
    });
    if (numeric) {
        unsigned octet = 0;
        std::size_t octetLength = 0;
        unsigned octetCount = 1;
        for (const unsigned char c : value) {
            if (c == '.') {
                if (octetLength == 0 || octetLength > 3 || octet > 255)
                    return false;
                octet = 0;
                octetLength = 0;
                ++octetCount;
                continue;
            }
            if (octetLength == 1 && octet == 0)
                return false;
            octet = octet * 10 + (c - '0');
            ++octetLength;
        }
        return octetCount == 4 && octetLength != 0 && octetLength <= 3 && octet <= 255;
    }

    std::size_t labelLength = 0;
    bool labelStartsWithHyphen = false;
    bool labelEndsWithHyphen = false;
    for (const unsigned char c : value) {
        if (c == '.') {
            if (labelLength == 0 || labelLength > 63 || labelStartsWithHyphen ||
                labelEndsWithHyphen)
                return false;
            labelLength = 0;
            labelStartsWithHyphen = false;
            labelEndsWithHyphen = false;
            continue;
        }
        const bool valid =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!valid)
            return false;
        if (labelLength == 0)
            labelStartsWithHyphen = c == '-';
        labelEndsWithHyphen = c == '-';
        ++labelLength;
        if (labelLength > 63)
            return false;
    }
    return labelLength != 0 && !labelStartsWithHyphen && !labelEndsWithHyphen;
}

void applyControlConfig(S3eConfig &config, const ControlConfig &control) {
    config.set("GAME", "VoiceChatEnabled", control.voiceChatEnabled() ? "1" : "0");
    if (control.onlineEnabled()) {
        const std::string gameEndpoint = control.multiplayerServer() + ":3074";
        config.set("GAME", "OnlineAccount", "GENERIC");
        config.set("GAME", "GameVersion", "1.0.11");
        config.set("Demonware", "LSGServer", gameEndpoint);
        config.set("Demonware", "AuthServer", gameEndpoint);
        config.set("STUN", "STUNServer", control.multiplayerServer());
        return;
    }

    config.set("GAME", "OnlineAccount", "NONE");
    config.set("GAME", "OnlineUseGameCenterMM", "0");
    config.set("GAME", "MatchmakingSearchAndPublishMode", "0");
    config.set("Demonware", "OnlineAccount", "NONE");
    config.set("Demonware", "LSGServer", "");
    config.set("Demonware", "AuthServer", "");
    config.set("STUN", "STUNServer", "");
    config.set("ONLINE", "dispatcher", "");
}

} // namespace codboz
