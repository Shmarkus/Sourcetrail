## Code navigation with the `LSP` tool (Eclipse JDT language server)

The `LSP` tool talks to a Java language server (jdtls) that has imported this Gradle project, with Lombok support.
It answers Java structure from the compiler's resolved model, not from text. Use it for structure. Use grep for text,
config, strings and log messages.

| Question | LSP operation |
|---|---|
| Who calls X? | `incomingCalls` at X's name; repeat on each caller to go up a level |
| What does X call? | `outgoingCalls` at X's name; repeat to go down |
| How does A reach B? | `incomingCalls` from B upward until A appears (or `outgoingCalls` from A) |
| What breaks if X's signature changes? | `findReferences` at X (every call, method reference and override) |
| Implementations or overrides of an interface method | `goToImplementation` at the method's name |
| A stack-trace line: its method and callers | read the line, then `incomingCalls` at the method's name |
| Where is symbol X? | `workspaceSymbol` with query X |
| What does a name resolve to? | `goToDefinition`, `hover` |
| Members of a file | `documentSymbol` |

- **Positions:** every operation except `workspaceSymbol` takes a file path and a 1-based line and character on the
  symbol's name. Get them from `workspaceSymbol`, `documentSymbol` or by reading the declaration.
- **One level per call:** call hierarchy answers one level at a time. Callers through an interface show up at the
  interface method's implementation. A caller can be a lambda (`apply(...)`, `lambda$0`): name the enclosing method
  instead. The same caller can appear once per call site.
- **Trust the server's results** without re-reading source, unless a result looks incomplete. Blind spots: text,
  config and strings; reflection and Spring wiring by name; generated sources that are not in the build.
- The server writes Eclipse files into the project (`.project`, `.classpath`, `.settings/`, `bin/`). `bin/` holds
  compiled classes and copies of resources: leave it out of text searches (`--exclude-dir=bin`).
