import { Injectable, inject } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';

import { ApplicationConfigService } from '../core/config/application-config.service';
import { Order } from './order.model';

const API = 'api';

@Injectable({ providedIn: 'root' })
export class OrderService {
  private readonly http = inject(HttpClient);
  private readonly applicationConfigService = inject(ApplicationConfigService);
  private readonly resourceUrl = this.applicationConfigService.getEndpointFor('api/orders');

  list(): Observable<Order[]> {
    return this.http.get<Order[]>('api/orders');
  }

  create(order: Order): Observable<Order> {
    return this.http.post<Order>(`${API}/orders`, order);
  }

  find(id: number): Observable<Order>;
  find(customer: string): Observable<Order[]>;
  find(key: number | string): Observable<Order> | Observable<Order[]> {
    return typeof key === 'number' ? this.get(key) : this.list();
  }

  get(id: number): Observable<Order> {
    return this.http.get<Order>(`${this.resourceUrl}/${id}`);
  }

  download(location: string): Observable<string> {
    return this.http.get<string>(location);
  }
}
