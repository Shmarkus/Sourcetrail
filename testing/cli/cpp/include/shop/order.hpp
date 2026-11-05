#pragma once

#include <memory>
#include <vector>

#include "shop/box.hpp"
#include "shop/item.hpp"

namespace shop {

class Order {
public:
  void add(std::shared_ptr<Item> item);
  double total() const;
  std::size_t size() const;

private:
  std::vector<std::shared_ptr<Item>> m_items;
};

double discount(double amount, int percent);
double discount(double amount, double rate);

Box<double> totalWithTax(const Order& order);

}    // namespace shop
