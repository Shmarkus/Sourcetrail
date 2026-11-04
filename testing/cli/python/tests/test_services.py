from shop.services import OrderService


def test_place():
    service = OrderService()
    order = service.place("ada", "ada@example.com", "book")
    assert order.total == 9.5
