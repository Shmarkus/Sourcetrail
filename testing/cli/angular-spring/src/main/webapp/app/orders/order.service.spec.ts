import { OrderService } from './order.service';

describe('OrderService', () => {
  it('lists orders', () => {
    const service = new OrderService();
    expect(service.list()).toBeTruthy();
  });
});
