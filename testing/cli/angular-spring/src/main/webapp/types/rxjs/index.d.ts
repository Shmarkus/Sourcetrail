// Minimal stand-in for rxjs.
export declare class Observable<T> {
  subscribe(next?: (value: T) => void): { unsubscribe(): void };
}
