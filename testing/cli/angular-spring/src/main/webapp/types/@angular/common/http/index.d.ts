// Minimal stand-in for @angular/common/http.
import { Observable } from 'rxjs';

export declare class HttpClient {
  get<T>(url: string, options?: object): Observable<T>;
  post<T>(url: string, body: unknown, options?: object): Observable<T>;
  put<T>(url: string, body: unknown, options?: object): Observable<T>;
  patch<T>(url: string, body: unknown, options?: object): Observable<T>;
  delete<T>(url: string, options?: object): Observable<T>;
}
