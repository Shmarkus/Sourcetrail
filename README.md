# Sourcetrail

> [!NOTE]
> This is a fork of the open-source [Sourcetrail](https://github.com/CoatiSoftware/Sourcetrail) by Coati Software, whose
> original repository is archived. It is based on the
> [OpenSourceSourceTrail](https://github.com/OpenSourceSourceTrail/Sourcetrail) continuation of the project.

> [!IMPORTANT]
> The fork adds Java 25 support for Gradle and Maven projects, TypeScript and Python indexing, and `st`, a command-line
> tool that gives agents fast, reliable lookups of classes and call chains.

Sourcetrail is a free, offline source explorer that helps you get productive on unfamiliar code. It indexes a project
and lets you browse its symbols and their relationships in a graph next to the code.

![Sourcetrail graph of a call chain from an Angular component through an HTTP endpoint into the Spring backend](docs/readme/user_interface.png)

*A call chain in one graph: from an Angular component through `GET /api/my/home` into the Spring backend.*

## What this fork supports

- **Platform:** Linux x86-64. Windows and macOS builds are not maintained here.
- **Java:** Gradle and Maven projects, Lombok included. Java records are
  [not indexed yet](DOCUMENTATION.md#st-known-limits).
- **TypeScript:** projects with a `tsconfig.json`. In an Angular + Spring app in one folder, frontend HTTP calls are
  linked to their Spring handlers.
- **Python:** projects with a `pyproject.toml`, `setup.py`, `setup.cfg` or `requirements*.txt`, tests included.
  Library calls resolve through the project's virtualenv.
- **Work in progress:** C/C++ indexing is not in the current builds.

## Getting started

1. Download `Sourcetrail_Linux_64bit.tar.gz` from the [releases page](https://github.com/Shmarkus/Sourcetrail/releases)
   and unpack it.
2. Run `sudo Sourcetrail/install.sh` to install to `/opt/sourcetrail` and add `sourcetrail` and `st` to your `PATH`. To
   try it without installing, run `Sourcetrail/Sourcetrail.sh`.
3. In Sourcetrail, choose **New Project** and pick your project's source folder. There is no setup wizard: Sourcetrail
   detects Gradle, Maven, TypeScript and Python from the folder. Start indexing, and `st` then works from anywhere in
   that folder.

Java indexing needs JDK 21 or newer, set in the preferences. TypeScript needs Node.js 22.13 or newer and the project's
`node_modules`. Python needs Node.js 22.13 or newer, and for library calls a `.venv` with the project's dependencies.

To give an agent `st`, add [this snippet](docs/agents/st-claude-md-snippet.md) to your project's `CLAUDE.md`.

## `st`: code navigation for agents

`st` answers "who calls this", "what implements that", "how does A reach B" from an
existing Sourcetrail index, as plain text lines that pipe into each other.

### For agents

| Question | Command |
|---|---|
| Who calls X? | `st callers OrderService.place --depth 2` |
| What does X call? | `st callees OrderService.place` |
| Implementations or overrides (and the reverse) | `st impls PaymentGateway`, `st impls PaymentGateway.charge`; `st supers CardGateway` |
| How does A reach B? | `st path OrderController.create OrderRepository.save` |
| What breaks if X changes? | `st impact OrderService.place --group file` |
| A stack-trace line: its method and callers | `st at OrderService.java:42 \| st callers --depth 3` |
| Members of a class or file; package or class dependencies | `st outline OrderService`, `st outline src/main/java/com/acme/shop/OrderService.java`; `st deps com.acme.shop.order` (`--in` for incoming) |
| Files in a git diff to symbols | `git diff --name-only \| st symbols-in` |
| What uses a library annotation or type (`@Scheduled`, `TaskScheduler`) | `st refs Scheduled`; a whole library package: `st impact org.springframework.scheduling --depth 1` |
| Angular + Spring: which repositories and Feign clients does this Angular method reach? | `st callees OrderListComponent.load --depth 0 --boundaries` |
| Angular + Spring: which Angular code reaches this Java method? | `st callers com.acme.shop.OrderService.list --depth 0 --side frontend` |
| Narrow a result by annotation, kind or file | `... \| st filter --annotated Test`, `--kind method`, `--not --file '*Test*'` |
| Text, config, strings, log messages | grep, not `st`; quote globs for zsh: `grep -rn --include='*.java' 'text' .` |

Names are a short name (`OrderService.place`) or a key; parameters take simple type names: `'OrderService.place(Order, int)'`.
- **Trust the answer.** No `st: incomplete:` line on stderr means the answer is complete as far as the index goes: don't
  re-read source to confirm it. Only the [blind spots](DOCUMENTATION.md#st-known-limits) need grep or reading: text, config, strings, Java records,
  Lombok `@Builder` setters of a field, JPA metamodel uses (`Order_.status`), calls through a field of an unresolved type.
- **Exit 1** means none. **Exit 2** (ambiguous): pick a key from the candidates on stderr and rerun.
  **Exit 3** (not found): check the name, the parameters (count and simple type names; stderr lists the overloads), and the
  project: `st` picks it from the current directory, `st find <name>` names it (`find: 0 (project shop)`), `--project` overrides.

Faster: `st serve --detach` once per session keeps the index loaded (opt-in; `st status` shows `server running`).
```bash
st at OrderService.java:42 | st callers --depth 4 | st filter --annotated Test            # stack trace -> callers -> tests
git diff --name-only | st symbols-in | st impact | st filter --not --file '*Test*'          # diff -> what it can break, tests left out
st impls PaymentGateway.charge | st callers | st filter --annotated RestController           # interface -> impls -> annotated callers
```

For every command, option and known limit, see the [`st` reference](DOCUMENTATION.md#st-reference).

## Documentation

- [User documentation](DOCUMENTATION.md)
- [Java projects](DOCUMENTATION.md#new-java-projects)
- [TypeScript and the frontend–backend link](DOCUMENTATION.md#typescript-and-the-frontendbackend-link)
- [Python projects](DOCUMENTATION.md#python-projects)
- [`st` reference](DOCUMENTATION.md#st-reference)
- [Building from source and running the tests](DOCUMENTATION.md#how-to-build)
- [Changelog](CHANGELOG.md)
- [Contributing](CONTRIBUTING.md)

Report bugs and ask questions in [Issues](https://github.com/Shmarkus/Sourcetrail/issues).

## License

Sourcetrail is licensed under the [GNU General Public License Version 3](LICENSE.txt).

The "Sourcetrail" name is a trademark owned by Coati Software and is not included within the assets licensed under the GNU GPLv3.
