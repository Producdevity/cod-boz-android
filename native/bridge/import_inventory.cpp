#include "import_inventory.hpp"

#include "bridge/generated_imports.hpp"

#include <algorithm>

namespace codboz {

std::span<const std::string_view> sourceImportInventory() {
    return kSourceImports;
}

bool matchesSourceImportInventory(const std::vector<std::string> &symbols) {
    return symbols.size() == kSourceImports.size() &&
           std::equal(symbols.begin(), symbols.end(), kSourceImports.begin());
}

} // namespace codboz
