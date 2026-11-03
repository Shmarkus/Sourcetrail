import { Component, inject } from '@angular/core';

import * as shop from '../orders';
import { Order, OrderApi, OrderService } from '../orders';

@Component({ selector: 'acme-home', template: '' })
export class HomeComponent {
  latest: Order[] = [];
  private readonly orders = inject(OrderService);
  private readonly api = inject(OrderApi);
  private readonly viaNamespace = inject(shop.OrderService);

  load(): void {
    this.orders.list().subscribe(orders => (this.latest = orders));
  }

  reload(): void {
    this.api.list().subscribe(orders => (this.latest = orders));
  }

  refresh(): void {
    this.viaNamespace.list().subscribe(orders => (this.latest = orders));
  }
}
