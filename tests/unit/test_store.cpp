#include "doctest.h"
#include "nsp-store.hpp"

TEST_CASE("store: file_size is fixed and matches layout") {
    using namespace nsp;
    uint32_t cap = 51, slots = 360;
    size_t expected =
        sizeof(Header)
        + (size_t)cap * NSP_NAME_MAX
        + (size_t)slots * sizeof(SlotMeta)
        + (size_t)slots * cap * sizeof(Cell);
    CHECK(file_size(cap, slots) == expected);
    CHECK(sizeof(Cell) == 28);
}

TEST_CASE("store: cell_index is row-major by slot") {
    using namespace nsp;
    CHECK(cell_index(0, 0, 51) == 0);
    CHECK(cell_index(0, 5, 51) == 5);
    CHECK(cell_index(1, 0, 51) == 51);
    CHECK(cell_index(2, 3, 51) == 105);
}
