#pragma once

#include <string>
#include <string_view>

namespace codboz {

class S3eConfig;

class ControlConfig {
  public:
    static ControlConfig load(const std::string &path);

    const std::string &playerName() const {
        return playerName_;
    }
    const std::string &multiplayerServer() const {
        return multiplayerServer_;
    }
    bool onlineEnabled() const {
        return !multiplayerServer_.empty();
    }
    bool voiceChatEnabled() const {
        return voiceChatEnabled_;
    }
    std::string resolveHostname(std::string_view hostname) const;

    static bool validPlayerName(std::string_view value);
    static bool validMultiplayerServer(std::string_view value);

  private:
    std::string playerName_ = "Player";
    std::string multiplayerServer_;
    bool voiceChatEnabled_ = false;
};

void applyControlConfig(S3eConfig &config, const ControlConfig &control);

} // namespace codboz
