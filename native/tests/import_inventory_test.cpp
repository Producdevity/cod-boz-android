#include "bridge/import_inventory.hpp"

#include "test_macros.hpp"

#include <string>
#include <vector>

int main() {
    const auto imports = codboz::sourceImportInventory();
    CHECK(imports.size() == 391);
    CHECK(imports.front() == "s3eMallocBase");
    CHECK(imports.back() == "glBeginQuery");

    std::vector<std::string> exact;
    exact.reserve(imports.size());
    for (const auto name : imports) {
        exact.emplace_back(name);
    }
    CHECK(codboz::matchesSourceImportInventory(exact));
    std::swap(exact[0], exact[1]);
    CHECK(!codboz::matchesSourceImportInventory(exact));
    return 0;
}
