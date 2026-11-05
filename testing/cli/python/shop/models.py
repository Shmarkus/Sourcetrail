from dataclasses import dataclass, field

DEFAULT_COUNTRY = "EE"


class User:
    def __init__(self, name: str, email: str):
        self.name = name
        self.email = email
        self.country = DEFAULT_COUNTRY

    @property
    def display_name(self) -> str:
        return self.name.title()

    def greet(self) -> str:
        return f"Hello, {self.display_name}"

    def can_refund(self) -> bool:
        return False


class Admin(User):
    def __init__(self, name: str, email: str, level: int = 1):
        super().__init__(name, email)
        self.level = level

    def can_refund(self) -> bool:
        return self.level > 1


@dataclass
class Order:
    owner: User
    items: list = field(default_factory=list)
    total: float = 0.0

    def add(self, item: str, price: float) -> None:
        self.items.append(item)
        self.total += price
