import { Injectable } from '@angular/core';

@Injectable({ providedIn: 'root' })
export class ApplicationConfigService {
  private endpointPrefix = '';

  getEndpointFor(api: string): string {
    return `${this.endpointPrefix}${api}`;
  }
}
