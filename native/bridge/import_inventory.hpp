#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace codboz {

std::span<const std::string_view> sourceImportInventory();
bool matchesSourceImportInventory(const std::vector<std::string> &symbols);

} // namespace codboz
