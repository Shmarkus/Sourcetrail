// Minimal stand-in for the jasmine globals the spec uses.
declare function describe(description: string, body: () => void): void;
declare function it(expectation: string, body: () => void): void;
declare function expect<T>(actual: T): { toBeTruthy(): void };
