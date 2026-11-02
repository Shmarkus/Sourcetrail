package com.acme.shop.app;

import com.acme.shop.core.Price;

public class Checkout {
    public String total(Price first, Price second) {
        return new Price(first.getCents() + second.getCents()).format("EUR");
    }
}
