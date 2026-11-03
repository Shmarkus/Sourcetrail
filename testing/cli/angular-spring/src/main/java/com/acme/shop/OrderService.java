package com.acme.shop;

import java.util.List;
import org.springframework.stereotype.Service;

@Service
public class OrderService {
    private final OrderRepository orderRepository;
    private final PaymentClient paymentClient;

    public OrderService(OrderRepository orderRepository, PaymentClient paymentClient) {
        this.orderRepository = orderRepository;
        this.paymentClient = paymentClient;
    }

    public List<Order> list() {
        return orderRepository.findAll();
    }

    public Order create(Order order) {
        if (!paymentClient.authorize(order)) {
            throw new IllegalStateException("payment declined");
        }
        return orderRepository.save(order);
    }

    public Order get(Long id) {
        return orderRepository.findById(id).orElseThrow();
    }
}
