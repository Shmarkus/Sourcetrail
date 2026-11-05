#pragma once

// Header-only: a value holder with a default.
namespace shop {

template <typename T>
class Box {
public:
  explicit Box(T value) : m_value(value) {}

  T get() const {
    return m_value;
  }

  void set(T value) {
    m_value = value;
  }

private:
  T m_value;
};

template <typename T>
Box<T> boxed(T value) {
  return Box<T>(value);
}

}    // namespace shop
