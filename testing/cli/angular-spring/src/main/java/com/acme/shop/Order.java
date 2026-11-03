package com.acme.shop;

import jakarta.persistence.Entity;
import jakarta.persistence.Id;

@Entity
public class Order {
    @Id
    private Long id;
    private String customer;
    private long amountCents;

    public Long getId() {
        return id;
    }

    public String getCustomer() {
        return customer;
    }

    public long getAmountCents() {
        return amountCents;
    }
}
