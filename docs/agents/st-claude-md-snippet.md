## Code navigation with `st`

Use `st` (Sourcetrail CLI) for Java structure, and for TypeScript in an Angular + Spring project. Use grep for text, config, strings and log messages.

| Question | Command |
|---|---|
| Who calls X? What does X call? | `st callers Cls.method --depth 2`, `st callees Cls.method` |
| Implementations, overrides, supertypes | `st impls Iface`, `st impls Iface.method`, `st supers Cls` |
| How does A reach B? | `st path A.method B.method` |
| What breaks if X changes? | `st impact Cls.method --group file` |
| A stack-trace line: its method and callers | `st at File.java:42 \| st callers --depth 3` |
| Members of a class or file | `st outline Cls`, `st outline path/to/File.java` |
| Files of a git diff as symbols | `git diff --name-only \| st symbols-in` |
| Package or class dependencies | `st deps com.acme.order` (`--in` for incoming) |
| What uses a library annotation or type (`@Scheduled`, `TaskScheduler`) | `st refs Scheduled`; a library package: `st impact org.springframework.scheduling --depth 1` |
| Angular + Spring: repositories and Feign clients an Angular method reaches | `st callees Cmp.load --depth 0 --boundaries` |
| Angular + Spring: the Angular code that reaches a Java method | `st callers pkg.Svc.method --depth 0 --side frontend` (`--tests` keeps `*.spec.ts`) |
| Narrow any result | `... \| st filter --annotated Test` (also `--kind`, `--package`, `--file`, `--not`) |

- **Names** are a short name (`Cls.method`) or a full key. Parameters take simple type names: `'Cls.method(Order, int)'`.
  Each result is one line whose first column is the key, so commands pipe into each other.
- **Trust the answer** without re-reading source, unless stderr shows `st: incomplete:` or the question touches a blind
  spot: text, config and strings; Java records; Lombok `@Builder` setters; JPA metamodel uses (`Order_.status`); calls
  through a field of an unresolved type.
- **Exit codes:** 1 means no results. 2 means the name is ambiguous: pick a key from the candidates on stderr and rerun.
  3 means not found: check the name, the parameters and the project (`st find <name>` names the project on stderr).
- **A field and a method with the same name** (Lombok fluent getters, for example) resolve to the field. For the method,
  pass its full key as `st find <name>` prints it.
- **When you grep,** skip Sourcetrail's delombok copies: `grep -r --exclude-dir=sourcetrail_dependencies
  --exclude-dir=.sourcetrail ...`, or `rg -g '!sourcetrail_dependencies' ...`.
- **TypeScript needs Node.js and the project's `node_modules`.** Without them `st` prints `TypeScript skipped: ...` and
  answers for the Java only; run `npm install`, then `st index`. Frontend calls linked to the backend show as
  `HTTP.GET /api/orders` (kind `endpoint`); a call whose URL is not resolved has no link.
- **No index, or one from an older version** (exit 4): run `st index --full` once in the project folder. Later
  queries refresh changed files on their own; `st index` does the same on demand.
- **Faster:** `st serve --detach` once per session keeps the index loaded (opt-in; `st status` shows `server running`).
- More detail is in "For agents" in `<path to Sourcetrail README.md>`. You don't need it to start.
