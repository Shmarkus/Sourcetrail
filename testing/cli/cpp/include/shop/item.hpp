#pragma once

#include <string>

namespace shop {

class Item {
public:
  explicit Item(std::string name);
  virtual ~Item() = default;

  const std::string& name() const;
  virtual double price() const = 0;
  virtual std::string describe() const;

private:
  std::string m_name;
};

class Book : public Item {
public:
  Book(std::string name, double price, std::string author);

  double price() const override;
  std::string describe() const override;

private:
  double m_price;
  std::string m_author;
};

class Food : public Item {
public:
  Food(std::string name, double pricePerKg, double kg);

  double price() const override;

private:
  double m_pricePerKg;
  double m_kg;
};

bool operator==(const Item& left, const Item& right);

}    // namespace shop
