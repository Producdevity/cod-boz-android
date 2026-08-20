#include "guest/guest_handles.hpp"

#include "test_macros.hpp"

#include <string>

int main() {
    codboz::HandleTable<std::string> table;
    auto h1 = table.create("file");
    auto h2 = table.create("socket");
    CHECK(h1 != 0);
    CHECK(h2 != 0);
    CHECK(h1 != h2);

    CHECK(table.get(h1) != nullptr);
    CHECK(*table.get(h1) == "file");
    CHECK(*table.get(h2) == "socket");

    // Unknown / zero handle resolves to nothing.
    CHECK(table.get(0) == nullptr);
    CHECK(table.get(0x12345678) == nullptr);

    // Release invalidates the handle.
    CHECK(table.release(h1));
    CHECK(table.get(h1) == nullptr);
    CHECK(!table.release(h1)); // already released

    // Reusing the slot bumps the generation; the old handle must not resolve.
    auto h3 = table.create("timer");
    CHECK(table.get(h3) != nullptr);
    CHECK(*table.get(h3) == "timer");
    CHECK(table.get(h1) == nullptr); // stale, different generation
    return 0;
}
