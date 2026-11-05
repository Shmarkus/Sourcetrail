#include <iostream>
#include <memory>

#include "shop/order.hpp"

int main() {
  shop::Order order;
  order.add(std::make_shared<shop::Book>("Dune", 12.5, "Herbert"));
  order.add(std::make_shared<shop::Food>("Apples", 2.0, 1.5));
  std::cout << shop::discount(shop::totalWithTax(order).get(), 10) << '\n';
  return 0;
}
