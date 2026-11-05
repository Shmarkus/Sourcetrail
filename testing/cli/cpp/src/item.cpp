#include "shop/item.hpp"

#include <utility>

namespace shop {

Item::Item(std::string name) : m_name(std::move(name)) {}

const std::string& Item::name() const {
  return m_name;
}

std::string Item::describe() const {
  return m_name;
}

Book::Book(std::string name, double price, std::string author)
    : Item(std::move(name)), m_price(price), m_author(std::move(author)) {}

double Book::price() const {
  return m_price;
}

std::string Book::describe() const {
  return Item::describe() + " by " + m_author;
}

Food::Food(std::string name, double pricePerKg, double kg) : Item(std::move(name)), m_pricePerKg(pricePerKg), m_kg(kg) {}

double Food::price() const {
  return m_pricePerKg * m_kg;
}

bool operator==(const Item& left, const Item& right) {
  return left.name() == right.name() && left.price() == right.price();
}

}    // namespace shop
