## Code navigation with `st`

- Use `st` (Sourcetrail CLI) for Java structure questions: callers, callees, implementations, paths from A to B, change
  impact, stack-trace lines, class and file outlines, package dependencies. Use grep for text, config, strings and log messages.
- Trust an `st` answer without re-reading the source unless stderr shows `st: incomplete:` or the question touches a listed
  blind spot (text, config and strings, Java records, Lombok `@Builder` setters, JPA metamodel uses such as `Order_.status`,
  calls through a field of an unresolved type).
- Exit 2: pick a key from the candidates on stderr. Exit 3: check the name, the parameters and the project (`st find <name>` names it on stderr).
- Which command to run, and pipeline recipes: "For agents" in `<path to Sourcetrail README.md>`.
