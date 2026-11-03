import { Component, OnInit, inject } from '@angular/core';

import { Order } from './order.model';
import { OrderService } from './order.service';

@Component({ selector: 'acme-order-detail', template: '' })
export class OrderDetailComponent implements OnInit {
  id = 0;
  order?: Order;
  private readonly orderService = inject(OrderService);

  ngOnInit(): void {
    this.orderService.get(this.id).subscribe(order => (this.order = order));
  }
}
