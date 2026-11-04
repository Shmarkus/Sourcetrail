import functools

from shop import User
from shop.models import Admin, Order


def logged(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)

    return wrapper


class OrderService:
    def __init__(self):
        self.orders = []

    @logged
    def place(self, name: str, email: str, item: str) -> Order:
        user = User(name, email)
        user.greet()
        order = Order(user)
        order.add(item, 9.5)
        self.orders.append(order)
        return order

    def refund(self, admin: Admin, order: Order) -> bool:
        return admin.can_refund() and order in self.orders


def default_service() -> OrderService:
    return OrderService()
