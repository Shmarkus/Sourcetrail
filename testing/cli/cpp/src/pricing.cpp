#include "shop/order.hpp"

namespace shop {

namespace {
constexpr double kTaxRate = 0.2;
}    // namespace

double discount(double amount, int percent) {
  return discount(amount, percent / 100.0);
}

double discount(double amount, double rate) {
  return amount * (1.0 - rate);
}

Box<double> totalWithTax(const Order& order) {
  return boxed(order.total() * (1.0 + kTaxRate));
}

}    // namespace shop
