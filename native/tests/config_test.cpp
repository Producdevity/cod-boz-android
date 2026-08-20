#include "s3e/config.hpp"

#include "test_macros.hpp"

int main() {
    codboz::S3eConfig config;
    config.parse(R"(
[GAME]
DeployConfig=0
iAndroidReleaseBuild=0
{OS=ANDROID}
[GAME]
iAndroidReleaseBuild=1
{}
{[GAME] DeployConfig==artbuild}
[GX]
DataCacheSizeHW=5000000
{}
{[GAME] iAndroidReleaseBuild == 1}
[GAME]
ResourceDownloader=1
{}
{OS=WINDOWS}
[GAME]
WindowsOnly=1
{}
{ID=ANDROID "Emulator"}
[S3E]
DisableSound=1
{}
{CLASS=ANY}
[GAME]
Wildcard=enabled
{}
)");

    CHECK(*config.find("GAME", "iAndroidReleaseBuild") == "1");
    CHECK(*config.find("GAME", "ResourceDownloader") == "1");
    CHECK(config.find("GX", "DataCacheSizeHW") == nullptr);
    CHECK(config.find("GAME", "WindowsOnly") == nullptr);
    CHECK(config.find("S3E", "DisableSound") == nullptr);
    CHECK(*config.find("GAME", "Wildcard") == "enabled");
    return 0;
}
