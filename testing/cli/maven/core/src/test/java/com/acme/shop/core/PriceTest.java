package com.acme.shop.core;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class PriceTest {
    @Test
    public void formatsCents() {
        assertEquals("   3.05 EUR", new Price(305).format("EUR"));
    }
}
