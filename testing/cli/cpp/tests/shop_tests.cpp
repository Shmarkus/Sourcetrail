#include <cstdlib>
#include <iostream>
#include <memory>

#include "shop/order.hpp"

namespace {
int failures = 0;

void check(bool condition, const char* what) {
  if(!condition) {
    std::cerr << "failed: " << what << '\n';
    ++failures;
  }
}
}    // namespace

int main() {
  shop::Order order;
  order.add(std::make_shared<shop::Book>("Dune", 10.0, "Herbert"));
  check(order.total() == 10.0, "total");
  check(shop::discount(100.0, 10) == 90.0, "discount by percent");
  check(shop::discount(100.0, 0.5) == 50.0, "discount by rate");
  shop::Box<int> box(1);
  box.set(2);
  check(box.get() == 2, "box");
  check(shop::Book("A", 1.0, "B") == shop::Book("A", 1.0, "C"), "operator==");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
