import { Component } from '@angular/core';

import { Order } from './order.model';
import { OrderService } from './order.service';

@Component({ selector: 'acme-order-list', template: '' })
export class OrderListComponent {
  orders: Order[] = [];

  constructor(private orderService: OrderService) {}

  load(): void {
    this.orderService.list().subscribe(this.show);
  }

  refresh(): void {
    this.load?.();
  }

  show(orders: Order[]): void {
    this.orders = orders;
  }
}
