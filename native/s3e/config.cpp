#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace codboz {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return lower(a) == lower(b);
}

bool containsCI(std::string_view text, std::string_view needle) {
    return lower(text).find(lower(needle)) != std::string::npos;
}

std::string stripComment(std::string_view line) {
    bool inQuote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            inQuote = !inQuote;
        } else if (c == '#' && !inQuote) {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

std::string stripQuotes(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return std::string(value.substr(1, value.size() - 2));
    }
    return std::string(value);
}

} // namespace

void S3eConfig::set(std::string_view section, std::string_view key, std::string_view value) {
    for (auto &e : entries_) {
        if (iequals(e.section, section) && iequals(e.key, key)) {
            e.value = std::string(value);
            return;
        }
    }
    entries_.push_back({std::string(section), std::string(key), std::string(value)});
}

const std::string *S3eConfig::find(std::string_view section, std::string_view key) const {
    for (const auto &e : entries_) {
        if (iequals(e.section, section) && iequals(e.key, key)) {
            return &e.value;
        }
    }
    return nullptr;
}

bool S3eConfig::parseIntValue(std::string_view value, int depth, int32_t &out) const {
    if (depth > 4) {
        return false;
    }
    auto text = trim(value);
    if (text.empty()) {
        return false;
    }
    if (text.front() == '[') {
        auto sectionEnd = text.find(']');
        if (sectionEnd == std::string_view::npos) {
            return false;
        }
        std::string_view section = trim(text.substr(1, sectionEnd - 1));
        std::string_view rest = trim(text.substr(sectionEnd + 1));
        char op = 0;
        int32_t adjustment = 0;
        auto plus = rest.find_first_of("+-");
        std::string_view key = rest;
        if (plus != std::string_view::npos) {
            key = trim(rest.substr(0, plus));
            op = rest[plus];
            auto adjText = trim(rest.substr(plus + 1));
            try {
                adjustment = static_cast<int32_t>(std::stoi(std::string(adjText), nullptr, 0));
            } catch (...) {
                return false;
            }
        }
        const std::string *ref = find(section, key);
        if (!ref) {
            return false;
        }
        int32_t base = 0;
        if (!parseIntValue(*ref, depth + 1, base)) {
            return false;
        }
        out = op == '+' ? base + adjustment : (op == '-' ? base - adjustment : base);
        return true;
    }
    try {
        out = static_cast<int32_t>(std::stoi(std::string(text), nullptr, 0));
        return true;
    } catch (...) {
        return false;
    }
}

bool S3eConfig::getInt(std::string_view section, std::string_view key, int32_t &out) const {
    const std::string *value = find(section, key);
    if (!value) {
        return false;
    }
    return parseIntValue(*value, 0, out);
}

bool S3eConfig::conditionMatches(std::string_view expression) const {
    const auto evaluate = [this](std::string_view term) {
        term = trim(term);
        if (term.empty()) {
            return true;
        }
        if (term.front() == '[') {
            const auto sectionEnd = term.find(']');
            const auto comparison = term.find("==", sectionEnd);
            if (sectionEnd == std::string_view::npos || comparison == std::string_view::npos) {
                return false;
            }
            const auto section = trim(term.substr(1, sectionEnd - 1));
            const auto key = trim(term.substr(sectionEnd + 1, comparison - sectionEnd - 1));
            const auto expected = stripQuotes(trim(term.substr(comparison + 2)));
            const auto *actual = find(section, key);
            return actual && iequals(trim(*actual), trim(expected));
        }

        const auto equals = term.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const auto name = trim(term.substr(0, equals));
        const auto value = trim(term.substr(equals + 1));
        if (iequals(name, "OS")) {
            return containsCI(value, "ANY") || containsCI(value, "ANDROID");
        }
        if (iequals(name, "ID")) {
            return containsCI(value, "ANY");
        }
        return containsCI(value, "ANY");
    };

    bool matches = true;
    std::string_view remaining = trim(expression);
    while (!remaining.empty()) {
        const auto lowered = lower(remaining);
        const auto andPosition = lowered.find(" and ");
        const auto commaPosition = remaining.find_first_of(",;");
        auto separator = std::string_view::npos;
        std::size_t separatorLength = 1;
        if (andPosition != std::string::npos &&
            (commaPosition == std::string_view::npos || andPosition < commaPosition)) {
            separator = andPosition;
            separatorLength = 5;
        } else {
            separator = commaPosition;
        }
        matches = matches && evaluate(remaining.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + separatorLength);
        remaining = trim(remaining);
    }
    return matches;
}

void S3eConfig::parse(std::string_view text) {
    entries_.clear();
    std::string section;
    bool active = true;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        std::string line =
            std::string(text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos));
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;
        auto cr = line.find('\r');
        if (cr != std::string::npos) {
            line.erase(cr);
        }
        line = stripComment(line);
        auto clean = trim(line);
        if (clean.empty()) {
            continue;
        }
        if (clean.front() == '{' && clean.back() == '}') {
            active = conditionMatches(clean.substr(1, clean.size() - 2));
            continue;
        }
        if (clean.front() == '[' && clean.back() == ']') {
            section = std::string(trim(clean.substr(1, clean.size() - 2)));
            continue;
        }
        if (!active) {
            continue;
        }
        auto eq = clean.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        auto key = trim(clean.substr(0, eq));
        auto value = stripQuotes(trim(clean.substr(eq + 1)));
        set(section, key, value);
    }
}

} // namespace codboz
