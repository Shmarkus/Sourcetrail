// Minimal stand-in for @angular/core, enough for the fixture to type-check without npm install. Decorators are
// declared as constants, as in the real package.
export interface ComponentDecorator {
  (meta: { selector?: string; template?: string }): ClassDecorator;
}
export interface InjectableDecorator {
  (meta?: { providedIn?: 'root' }): ClassDecorator;
}
export declare const Component: ComponentDecorator;
export declare const Injectable: InjectableDecorator;
export declare function inject<T>(token: abstract new (...args: any[]) => T): T;
export interface OnInit {
  ngOnInit(): void;
}
