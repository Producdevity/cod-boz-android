#include "game/codboz_player_name.hpp"
#include "s3e/config.hpp"
#include "s3e/control_config.hpp"

#include "test_macros.hpp"

#include <filesystem>
#include <fstream>

int main() {
    using namespace codboz;

    const auto path = std::filesystem::temp_directory_path() / "codboz-control-config-test.txt";
    {
        std::ofstream output(path);
        output << "player_name=Producdevity\n"
               << "multiplayer_server=boz-online.xubi.org\n"
               << "voice_chat_enabled=1\n";
    }
    const auto config = ControlConfig::load(path.string());
    CHECK(config.playerName() == "Producdevity");
    CHECK(config.multiplayerServer() == "boz-online.xubi.org");
    CHECK(config.onlineEnabled());
    CHECK(config.voiceChatEnabled());
    CHECK(config.resolveHostname("codboh-iphone-auth.prod.demonware.net") == "boz-online.xubi.org");
    CHECK(config.resolveHostname("host.Demonware.net") == "boz-online.xubi.org");
    CHECK(config.resolveHostname("example.net") == "example.net");

    S3eConfig onlineEmbedded;
    applyControlConfig(onlineEmbedded, config);
    CHECK(*onlineEmbedded.find("GAME", "OnlineAccount") == "GENERIC");
    CHECK(*onlineEmbedded.find("Demonware", "LSGServer") == "boz-online.xubi.org:3074");
    CHECK(*onlineEmbedded.find("Demonware", "AuthServer") == "boz-online.xubi.org:3074");
    CHECK(*onlineEmbedded.find("STUN", "STUNServer") == "boz-online.xubi.org");
    CHECK(*onlineEmbedded.find("GAME", "VoiceChatEnabled") == "1");
    CHECK(!ControlConfig::validPlayerName("too-long-player-name"));
    CHECK(ControlConfig::validMultiplayerServer("192.0.2.10"));
    CHECK(!ControlConfig::validMultiplayerServer("192.0.2.999"));
    CHECK(!ControlConfig::validMultiplayerServer("192.168.001.1"));
    CHECK(ControlConfig::validMultiplayerServer("server.example.org"));
    CHECK(ControlConfig::validMultiplayerServer(""));
    CHECK(!ControlConfig::validMultiplayerServer("https://server.example.org"));
    CHECK(!ControlConfig::validMultiplayerServer("server.example.org:3074"));
    CHECK(!ControlConfig::validMultiplayerServer("-server.example.org"));
    CHECK(!ControlConfig::validMultiplayerServer("server-.example.org"));
    std::filesystem::remove(path);

    {
        std::ofstream output(path);
        output << "multiplayer_server=\n";
    }
    const auto offline = ControlConfig::load(path.string());
    CHECK(!offline.onlineEnabled());
    CHECK(!offline.voiceChatEnabled());
    CHECK(offline.resolveHostname("auth.demonware.net") == "auth.demonware.net");

    S3eConfig embedded;
    embedded.parse("[GAME]\nOnlineAccount=GENERIC\n"
                   "[Demonware]\nLSGServer=lsg.example\nAuthServer=auth.example\n"
                   "[STUN]\nSTUNServer=stun.example\n"
                   "[ONLINE]\ndispatcher=dispatcher.example\n");
    applyControlConfig(embedded, offline);
    CHECK(*embedded.find("GAME", "OnlineAccount") == "NONE");
    CHECK(*embedded.find("GAME", "VoiceChatEnabled") == "0");
    CHECK(embedded.find("Demonware", "LSGServer")->empty());
    CHECK(embedded.find("Demonware", "AuthServer")->empty());
    CHECK(embedded.find("STUN", "STUNServer")->empty());
    CHECK(embedded.find("ONLINE", "dispatcher")->empty());
    std::filesystem::remove(path);

    constexpr GuestAddr base = 0x4a000000u;
    constexpr GuestAddr referenceOffset = 0x18f74cu;
    constexpr GuestAddr referencePcOffset = 0x18f60au;
    constexpr GuestAddr formatOffset = 0x3af131u;
    constexpr GuestAddr playerName = 0x60000000u;
    GuestMemory memory;
    memory.mapRegion(base, formatOffset + 0x100u);
    memory.writeString(base + formatOffset, "Player-%d");
    memory.write32(base + referenceOffset, formatOffset - referencePcOffset);
    CHECK(applyPlayerName(memory, base, playerName));
    CHECK(memory.read32(base + referenceOffset) == playerName - (base + referencePcOffset));
    return 0;
}
