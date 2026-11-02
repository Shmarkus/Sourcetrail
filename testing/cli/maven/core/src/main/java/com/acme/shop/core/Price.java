package com.acme.shop.core;

import org.apache.commons.lang3.StringUtils;

public class Price {
    private final long cents;

    public Price(long cents) {
        this.cents = cents;
    }

    public long getCents() {
        return cents;
    }

    public String format(String currency) {
        return StringUtils.leftPad(Long.toString(cents / 100), 4) + "." + StringUtils.leftPad(Long.toString(cents % 100), 2, '0') + " " + currency;
    }
}
