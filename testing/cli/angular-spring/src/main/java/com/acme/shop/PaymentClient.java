package com.acme.shop;

import org.springframework.cloud.openfeign.FeignClient;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;

@FeignClient(name = "payments")
public interface PaymentClient {
    @PostMapping("/authorizations")
    boolean authorize(@RequestBody Order order);
}
