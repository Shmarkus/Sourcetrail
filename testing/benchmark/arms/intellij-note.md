## Code navigation with the IntelliJ MCP tools

An IntelliJ IDEA instance has this project open and fully indexed (Gradle model, libraries, Lombok). Its MCP tools
(`mcp__intellij__*`) answer Java structure from the IDE's resolved code model, not from text. Use them for structure.
Use grep for text, config, strings and log messages.

| Question | Tool |
|---|---|
| Who calls X, directly or N levels up (through interfaces)? | `analyze_calls` symbolFqn=`pkg.Cls.method` analysisKind=`INCOMING_CALLS` (depth default 5) |
| What does X call, down to Y? | `analyze_calls` ... analysisKind=`OUTGOING_CALLS` |
| How does A reach B? | `analyze_calls` on B with `INCOMING_CALLS` and find A in the tree, or on A with `OUTGOING_CALLS` |
| What breaks if X's signature changes? | `analyze_calls` on X with `INCOMING_CALLS`, depth 1 for direct callers |
| A stack-trace line: its method and callers | read the line to name the method, then `analyze_calls` `INCOMING_CALLS` |
| Where is symbol X, what is its full name? | `search_symbol` q=`X` |
| What does the name at file:line:column resolve to? | `get_symbol_info` |
| Text in the project (IDE index) | `search_text`, `search_regex`, `search_file` |

- **Names:** `analyze_calls` takes a fully qualified name (`io.acme.OrderService.place`). If you only know a short name,
  find it with `search_symbol` first. An ambiguous name returns exact signatures: pass one back as `symbolFqn`.
- **Always pass `projectPath`** = the current working directory, or the tools answer "Unable to determine the target
  project".
- **Large trees:** the result is a text tree; each node has a `treePath`. Pass it back to expand a cut-off subtree;
  `maxChildren` and `maxNodes` bound the size.
- **Trust the call tree** without re-reading source, unless the question touches a blind spot: Lombok-generated
  methods (getters, setters, builders) can't be targeted ("No callable symbol found"), and a name that also exists in a
  library may resolve to the library's class; check the file path of the root node. For those, search text instead.
- There is no find-usages tool: for uses that are not calls (method references, field reads, types in signatures),
  combine `analyze_calls` with a text search.
