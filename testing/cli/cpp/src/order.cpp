#include "shop/order.hpp"

#include <utility>

namespace shop {

void Order::add(std::shared_ptr<Item> item) {
  m_items.push_back(std::move(item));
}

double Order::total() const {
  double sum = 0;
  for(const auto& item : m_items) {
    sum += item->price();
  }
  return sum;
}

std::size_t Order::size() const {
  return m_items.size();
}

}    // namespace shop
