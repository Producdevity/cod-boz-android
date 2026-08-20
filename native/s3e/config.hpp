#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codboz {

// Parsed Marmalade S3E configuration (the config block embedded in the S3E
// image). Mirrors the reference loader's parser: [Section] headers, {cond}
// platform blocks, key=value lines, quoted values, comments, and the
// [Section]Key==Value integer cross-reference form.
class S3eConfig {
  public:
    void parse(std::string_view text);
    void set(std::string_view section, std::string_view key, std::string_view value);

    // Look up a string value by section and key (case-insensitive).
    const std::string *find(std::string_view section, std::string_view key) const;
    // Resolve an int value, expanding [Section]Key references and +/- offsets.
    bool getInt(std::string_view section, std::string_view key, int32_t &out) const;

  private:
    struct Entry {
        std::string section;
        std::string key;
        std::string value;
    };
    std::vector<Entry> entries_;

    bool conditionMatches(std::string_view expression) const;
    bool parseIntValue(std::string_view value, int depth, int32_t &out) const;
};

} // namespace codboz
