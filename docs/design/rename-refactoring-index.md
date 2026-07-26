# Design notes: `textDocument/rename` and a reference index for slangd

Working document for [shader-slang/slang#6335](https://github.com/shader-slang/slang/issues/6335)
("Slangd - Support `textDocument/rename`").

This is a findings-and-options document, not an implementation plan of record. It captures what
exists in the Slang language server today, how rust-analyzer models the same problem (with emphasis
on local variables, per the original request), what the established C++ precedents look like
(clangd, clang-rename, ccls), and which of those translate to Slang's architecture. Decisions are
recorded at the end.

---

## 1. What the issue asks for

`slangd` does not advertise `renameProvider`, so editors report "rename is not supported by the
language server". The issue asks for LSP `textDocument/rename` — and, in practice, the companion
`textDocument/prepareRename`, which editors use to (a) validate that the cursor is on a renameable
symbol before prompting and (b) supply the placeholder text for the prompt.

The user-visible contract is:

- `prepareRename(uri, position)` → the `Range` of the identifier being renamed, or `null` /
  an error if this position cannot be renamed.
- `rename(uri, position, newName)` → a `WorkspaceEdit` containing every edit needed, across every
  affected file, applied atomically by the editor.

The hard part is not the protocol plumbing. It is answering "given a symbol, where are all of its
references?" — which the language server currently has no mechanism for in either direction.

---

## 2. Inventory: what slangd has today

### 2.1 Nothing rename-shaped

`ServerCapabilities` (`source/compiler-core/slang-language-server-protocol.h:283`) has no
`renameProvider`; `LanguageServer::init` (`source/slang/slang-language-server.cpp:141`) sets
`hoverProvider`, `definitionProvider`, `documentSymbolProvider`, formatting, completion, semantic
tokens, signature help, inlay hints — and nothing else. The JSON-RPC dispatch
(`slang-language-server.cpp:2738` and `:2882`) has no `textDocument/rename` branch.

### 2.2 But most of the primitives already exist

Three pieces of existing machinery cover most of what a rename needs:

**(a) Position → AST node path.** `findASTNodesAt`
(`source/slang/slang-language-server-ast-lookup.h:33`) walks the module AST, pruning by source
range, and returns `List<ASTLookupResult>` where each result is a `List<SyntaxNode*> path` from the
module down to the node under the cursor. This is what goto-definition, hover, and completion all
build on.

Crucially, `_findAstNodeImpl` (`slang-language-server-ast-lookup.cpp:753`) already handles the
cursor being on a **declaration's own name**, not just on a reference — it range-checks
`decl->nameAndLoc.loc` and skips decls carrying `SynthesizedModifier` or
`ImplicitParameterGroupElementTypeModifier`. So "cursor on `int x;`" and "cursor on `x + 1`" both
resolve, which is exactly the two entry points rename needs.

**(b) Node → symbol identity.** `LanguageServerCore::gotoDefinition`
(`slang-language-server.cpp:1082`) shows the classification logic in miniature: the leaf node is
either a `DeclRefExpr` (→ `declRef.getDecl()`, with `maybeRedirectToConstructor` for ctor calls), an
`OverloadedExpr` (→ ambiguous, several candidate `DeclRef`s), an `ImportDecl`, or an
`IncludeDeclBase`. For a rename, `Decl*` pointer identity within one `WorkspaceVersion` is a
perfectly good symbol identity — see §5.1 for where that stops being true.

**(c) Whole-AST traversal with a callback.** `iterateAST` / `iterateASTWithLanguageServerFilter`
(`source/slang/slang-ast-iterator.h:608`, `:626`) is a full `ExprVisitor`/`StmtVisitor`-based walk
that invokes a callback on every node, with a filter that (by default) restricts descent to decls
whose `HumaneLoc` path matches a given file. `getSemanticTokens`
(`slang-language-server-semantic-tokens.cpp:147`) is the model consumer: it walks the module and,
for every `DeclRefExpr` / `OverloadedExpr` / named `Decl`, emits a `(line, col, length, kind)`
record.

**Semantic tokens is, structurally, already a reference index — it just throws away the `Decl*` and
keeps the token kind.** A references index is the same walk keeping `(Decl*, SourceLoc)` and
discarding the kind. That is the single most important observation in this document: the traversal
work is done, tested, and maintained; only the collection policy differs.

### 2.3 Workspace and version model

```
Workspace                       (source/slang/slang-workspace-version.h:162)
 ├─ openedDocuments: Dictionary<String, RefPtr<DocumentVersion>>
 ├─ rootDirectories / workspaceSearchPaths
 └─ currentVersion: RefPtr<WorkspaceVersion>

WorkspaceVersion                (slang-workspace-version.h:138)
 ├─ linkage: RefPtr<Linkage>
 ├─ modules: Dictionary<String, Module*>
 ├─ markupASTs: Dictionary<ModuleDecl*, RefPtr<ASTMarkup>>   ← lazy per-module cache precedent
 └─ getOrLoadModule(path)
```

Two facts drive everything downstream:

1. **Invalidation is total.** `Workspace::invalidate()`
   (`slang-workspace-version.cpp:208`) is literally `currentVersion = nullptr;`. Every document
   change, settings change, or search-path change drops the entire `Linkage` and all parsed modules.
   There is no salsa-style dependency graph; the next request rebuilds from scratch, lazily.
2. **`getOrLoadModule` only serves opened documents.** `slang-workspace-version.cpp:802` returns
   `nullptr` if the path is not in `workspace->openedDocuments`. The workspace _does_ enumerate the
   root directories at `init` (`:167`) but only to collect **search paths** — it never builds a list
   of workspace `.slang` files. Nothing on disk that the user has not opened is reachable today.

`markupASTs` is the precedent worth copying: a `Dictionary<ModuleDecl*, RefPtr<...>>` on
`WorkspaceVersion`, populated lazily, destroyed wholesale with the version. A reference index
belongs in exactly that slot.

### 2.4 The constraint that shapes the whole design

`SemanticsVisitor::shouldSkipChecking` (`source/slang/slang-check-decl.cpp:2177`):

```cpp
// If we are in language server, we should skip checking all the function bodies
// except for the module or function that the user cared about.
// This optimization helps reduce the response time.
if (!getLinkage()->isInLanguageServer())
    return false;
if (auto funcDecl = as<FunctionDeclBase>(decl))
{
    auto& assistInfo = getLinkage()->contentAssistInfo;
    auto moduleDecl = getModuleDecl(decl);
    if (moduleDecl && moduleDecl->module->getNameObj() != assistInfo.primaryModuleName &&
        moduleDecl->getName() != assistInfo.primaryModuleName)
        return true;
    if (funcDecl->body)
    {
        auto humaneLoc = ...getHumaneLoc(decl->loc, SourceLocType::Actual);
        if (humaneLoc.pathInfo.foundPath != assistInfo.primaryModulePath)
            return true;
        ...
    }
}
```

**In language-server mode, function bodies outside the primary file are never checked.** Their
`DeclRefExpr`s are never resolved, so no AST walk can find references inside them. `getOrLoadModule`
sets `primaryModuleName` / `primaryModulePath` per request (`:816`) and deliberately reloads the
module from source each time rather than reusing an imported copy.

The consequence is sharp and useful:

| Rename target                                                  | Where its references can live | Reachable today?                                       |
| -------------------------------------------------------------- | ----------------------------- | ------------------------------------------------------ |
| Local variable, parameter, `let`/`var` binding                 | One function body             | **Yes** — that body is the primary file, fully checked |
| File-private struct / function / field, used only in this file | This file                     | **Yes**, same reason                                   |
| Anything used from another file                                | Other files' bodies           | **No** — those bodies are unchecked                    |

That table is the phasing plan writing itself, and it lands on exactly the same split rust-analyzer
makes for locals.

---

## 3. How rust-analyzer does it

Sources read: `crates/ide/src/rename.rs`, `crates/ide-db/src/rename.rs`, `crates/ide-db/src/search.rs`.

### 3.1 The headline finding: there is no reference index

This is worth stating plainly because it inverts the expectation the request started from.
rust-analyzer maintains a persistent index of **declarations** (`ide-db/src/symbol_index.rs`, an
fst-backed name → symbol map, used for workspace-symbol search and name lookup) — but it has **no
persistent index of references**. References are computed on demand, every time, by:

1. **Compute a search scope** from the definition's visibility.
2. **Textual prefilter**: substring-search the _raw text_ of each file in scope for the symbol's
   name, using a `memchr::memmem::Finder`, with word-boundary checks so `foo` does not match
   `foobar`.
3. **Semantic verification**: for each textual hit, map the offset back to a syntax token, classify
   it with `NameRefClass::classify` / `NameClass::classify`, and keep it only if the resolved
   `Definition` equals the target.

From `search.rs`:

```rust
let finder = &Finder::new(name);
for offset in Self::match_indices(&text, finder, search_range) {
    let usages = FindUsages::find_nodes(sema, name, file_id, &tree, offset)
        .filter_map(ast::NameRef::cast);
    ...
}
// verification:
match NameRefClass::classify(self.sema, name_ref) {
    Some(NameRefClass::Definition(def, _)) if self.def == def => { sink(file_id, reference) }
    _ => false,
}
```

The insight is **cheap candidate generation, expensive verification only at candidates**. Name
resolution — the costly part — runs at a handful of offsets rather than over every node in every
file. And it needs no index to invalidate, which is why it survives rust-analyzer's aggressive
incrementality without any extra bookkeeping.

What makes this affordable at all is salsa: `sema.classify` at an offset pulls in the file's parse
tree and its body-inference results, all of which are memoized queries. rust-analyzer is not
re-typechecking the crate per keystroke; it is re-running a memoized query whose inputs mostly did
not change. **Slang has no equivalent memoization** (see §2.3) — which is precisely why the textual
prefilter matters _more_ for us, not less: it is what keeps the number of files we must re-check
small.

### 3.2 Local variables specifically

`Definition::search_scope` in `search.rs` special-cases locals by walking to the body owner:

```rust
let def = match var.parent(db) {
    ExpressionStoreOwner::Body(def) => match def {
        DefWithBody::Function(f) => f.source(db).map(|src| src.syntax().cloned()),
        // const / static / variant ...
    }
};
return match def {
    Some(def) => SearchScope::file_range(def.as_ref().original_file_range_with_macro_call_input(db)),
    None => SearchScope::single_file(file_id),
};
```

So for a local, the search scope collapses to **a single `TextRange` inside a single file** — the
enclosing function/const/static body. The textual scan then covers a few dozen lines. This is why
local rename in rust-analyzer is instantaneous and why it needs no index at all.

Compare the non-local cases in the same function: `Visibility::Public =>
SearchScope::reverse_dependencies(db, krate)`, `pub(crate) => SearchScope::krate(...)`,
module-private → that module and its children. Visibility _is_ the scope calculation.

For identity, a local is `Definition::Local(Local { parent: DefWithBodyId, binding: BindingId })` —
i.e. "the Nth binding of this body", derived from body lowering. It is stable across reparses of
unrelated code and needs no global symbol id, because it can never be referenced from outside the
body. **Slang's `Decl*` for a local occupies the same role**: unique within a `WorkspaceVersion`,
never referenced from another file, dies with the version.

### 3.3 Rename mechanics worth stealing

- `prepare_rename` resolves the position to a `Definition` and returns only the identifier
  `TextRange`, asserting all candidate ranges agree. Failure paths `bail!` with a user-facing
  message.
- `IdentifierKind::classify` lexes the proposed name with the real lexer and rejects keywords before
  any edit is computed. Errors surface as "not a valid identifier", not as a broken file.
- `Definition::rename` dispatches per kind (`rename_mod` for a module → file rename;
  `rename_reference` for everything else), and refuses non-local crates:
  `bail!("Cannot rename a non-local definition")` — our analogue is the core module / stdlib.
- Edits are `source_edit_from_references` over the usages, plus one `source_edit_from_def` for the
  declaration site (via `def.range_for_rename(sema)`). Note the _definition site is a separate,
  explicit step_ — it is not one of the "references".
- `source_edit_from_references` tracks `edited_ranges` to **deduplicate**, because macro expansion
  can yield several references mapping to the same source span. Slang's `#include`-heavy and
  generic-instantiation-heavy AST has the same hazard from a different cause.
- Syntax-sensitive rewrites: renaming a local bound by record-field shorthand (`Foo { x }`) must
  rewrite it to `Foo { x: y }` rather than replacing the identifier in place. Slang's analogues
  are smaller but non-empty (see §5.4).

---

## 4. C++ precedents

### 4.1 clangd — the closest structural analogue

clangd (`clang-tools-extra/clangd/refactor/Rename.cpp`) uses an explicitly **hybrid** strategy, and
documents it in a comment worth quoting:

> "To make cross-file rename work for local symbol, we use a hybrid solution: run AST-based rename
> on the main file; run index-based rename on other affected files."

- **Main file, always AST-based.** `findOccurrencesWithinFile` walks the AST's top-level decls
  calling `findExplicitReferences`, and canonicalizes the target with `canonicalRenameDecl` (template
  specialization → primary template, constructor/destructor → parent class, instantiated member →
  original). This is a `RecursiveASTVisitor` + callback — structurally identical to Slang's
  `iterateAST` + lambda.
- **Function-local symbols stop there**, explicitly, "by design our index don't index these
  symbols."
- **Other files, index-based.** `renameOutsideFile` issues a `RefsRequest` keyed by `SymbolID` (a
  hash of the USR) to get _which files_ contain references, then re-parses those files and renames in
  them; `insertTransitiveOverrides` extends the set through `OverriddenBy` relations for virtuals.
- **Staleness reconciliation.** Because the index can lag the on-disk/dirty text, `adjustRenameRanges`
  re-lexes the file to collect candidate identifier ranges (`collectRenameIdentifierRanges`) and
  matches indexed occurrences against lexed ones via `findNearMiss`, accepting only "simple"
  displacements (a line shift _or_ a column shift, never both). This is a large amount of machinery
  that exists solely because the index and the buffer can disagree — a strong argument for not
  having a persistent index unless you must.
- **Validation is a first-class, three-stage pipeline**: `renameable` (rejects namespaces, overloaded
  operators, virtuals without opt-in, system headers), `checkName` (keyword collision, identifier
  validity, `lookupSiblingWithName` for shadowing conflicts), and a `ReasonToReject` enum
  (`NoSymbolFound`, `AmbiguousSymbol`, `UnsupportedSymbol`, `NonIndexable`, `SameName`) mapped to
  user-facing messages. Slang should copy this shape wholesale; it is what makes rename feel safe.

### 4.2 clang-rename — the minimal version

`clang::tooling`'s `USRFindingAction` + `RenamingAction`: find the decl's USR, run `USRLocFinder` (a
`RecursiveASTVisitor`) to collect `SourceLocation`s in the TU, emit `tooling::Replacements`.
`Replacements` / `AtomicChange` are the idiomatic C++ **edit-set** types — they enforce sorted,
non-overlapping edits and fail loudly on conflict rather than silently producing corrupt text. The
lesson for Slang is the type, not the algorithm: collect edits into a structure that detects
overlap, then serialize to LSP `TextEdit`s.

### 4.3 ccls / cquery — the actual persistent inverted index

ccls is the C++ project that does build the thing the request originally imagined: a per-file
`IndexFile` serialized to disk, symbols keyed by a `Usr` (a 64-bit hash of the USR string), each
carrying explicit `uses` / `declarations` / `definitions` location lists, merged into a global map.
It gives O(1) find-references across a huge codebase.

It also demonstrates the costs: a symbol id independent of pointers, a serialization format that
must be versioned, a background indexer with its own thread pool and queue, and staleness handling
when files change under the index. That price is right for a C++ codebase of millions of lines
across thousands of TUs. Slang shader workspaces are typically tens of files.

### 4.4 What "idiomatic C++" means for this specific codebase

Slang's own conventions already answer most of the design questions, and deviating from them would
be the unidiomatic choice:

- **Traversal**: `iterateAST(node, filter, callback)` with a lambda — do not write a new visitor.
- **Symbol identity**: `Decl*` within a version; `getMangledName` only if a cross-version/persistent
  id is ever needed (and note locals have no mangled name — which is fine, per §3.2).
- **Caching**: `Dictionary<K, V>` member on `WorkspaceVersion`, lazily populated, dying with the
  version — mirroring `markupASTs`.
- **Storage**: `Dictionary<Decl*, List<SourceLoc>>` (or a flat `List<(Decl*, SourceLoc)>` sorted
  once, if we want cache-friendly iteration and cheap grouping).
- **Failure**: `SLANG_RELEASE_ASSERT` on out-of-contract input; explicit error enums for
  user-reachable refusals, per CLAUDE.md's "fail loudly" rule.

---

## 5. Slang-specific complications

### 5.1 `Decl*` identity is version-scoped

`Workspace::invalidate()` drops the `Linkage`, and `getOrLoadModule` reloads modules from source, so
every `Decl*` is invalidated on each edit. Any index must be owned by `WorkspaceVersion` and must
never outlive it. A rename request must resolve the position → `Decl*` and compute all edits within
a single version, with `SLANG_AST_BUILDER_RAII` held, exactly as `gotoDefinition` does
(`slang-language-server.cpp:1095`).

### 5.2 Overloads and generics

`gotoDefinition` handles `OverloadedExpr` by returning _all_ candidates. For rename that is an
error, not a result — clangd's `AmbiguousSymbol`. Separately, renaming one overload of a function
must rename only that overload's declaration and the call sites that resolved to it; call sites are
already resolved to a specific `DeclRef`, so this falls out naturally, but the _declaration-site_
edit must not accidentally hit sibling overloads.

Generic decls need the same unwrapping semantic tokens already does
(`slang-language-server-semantic-tokens.cpp:76`): `if (auto genDecl = as<GenericDecl>(decl)) decl =
genDecl->inner;`. And references through a generic instantiation must map back to the original decl,
mirroring clangd's `canonicalRenameDecl`.

### 5.3 Non-renameable symbols

Anything in the core module / stdlib (`slang-synth://core`, `BuiltinTypeModifier`,
`SynthesizedModifier`), anything in a file the user has not opened and we cannot write, `$init` and
other synthetic names (`isHighlightableName` in semantic-tokens.cpp:44 already encodes the "is this
a real source identifier" test), and — at least initially — anything reached through a macro
expansion or an `#include`d file. These want an explicit rejection enum with distinct messages.

### 5.4 Syntax-sensitive edits

Slang has fewer shorthand forms than Rust, but the analogues to check are: constructor/initializer
syntax where the decl name and the type name coincide (`gotoDefinition` already needs
`maybeRedirectToConstructor` at `slang-language-server.cpp:1128`), swizzle-like member access,
attribute arguments that are `VarExpr`s referring to capability names
(semantic-tokens.cpp:291 shows these exist), and `#include`/`import` module names which are _file
paths_, not identifiers, and must be refused rather than edited.

### 5.5 Protocol gaps

`TextEdit` exists (`slang-language-server-protocol.h:180`). `RenameParams`, `PrepareRenameParams`,
`RenameOptions`, `WorkspaceEdit`, and `TextDocumentEdit` do not, and `ServerCapabilities` needs
`renameProvider`.

One concrete wrinkle: LSP's `WorkspaceEdit.changes` is a **map** from URI to `TextEdit[]`, and the
`StructRttiInfo` reflection used by these protocol structs has no dictionary support — the only
freeform escape in the header today is a raw `JSONValue` (`:818`). The clean fix is to use
`WorkspaceEdit.documentChanges: TextDocumentEdit[]` instead, which is a plain `List` of structs and
maps directly onto the existing reflection. `documentChanges` is the preferred form in LSP ≥3.13
anyway; it requires advertising nothing extra for the pure-text-edit case.

### 5.6 Testing

`tests/language-server/` + `runLanguageServerTest` (`tools/slang-test/slang-test-main.cpp:2321`)
already drives a real JSON-RPC server from `//TEST:LANG_SERVER(filecheck=CHECK):` files with
directives parsed at `:2416` — `//HOVER:line,col`, `//COMPLETE:line,col`,
`//SIGNATURE:line,col`. Adding `//RENAME:line,col,newName` (and `//PREPARE_RENAME:line,col`) that
sends the request and dumps the resulting edits for FileCheck is a small, well-precedented change.
Multi-file rename tests will need the harness to open more than one document, which it does not do
today.

---

## 6. Options

### Option A — Scoped on-demand search, no index (rust-analyzer's actual model)

For the symbol under the cursor, compute a **scope** (body range for a local; file for a
file-private decl; set of files otherwise), textually prefilter each file in scope for the name,
then resolve and verify each hit through the existing AST lookup.

- **+** No index, therefore no invalidation, no staleness, no memory growth — the entire class of
  bugs clangd's `adjustRenameRanges` exists to paper over never arises.
- **+** Smallest diff; reuses `findASTNodesAt` unchanged.
- **+** Degrades gracefully: an unopened file simply is not searched, which is a correct-and-visible
  limitation rather than a wrong answer.
- **−** Verification cost per candidate is a `findASTNodesAt` call, which re-walks the module from
  the root for _each_ hit. rust-analyzer can afford per-offset resolution because salsa memoizes;
  we would be doing an O(module) walk per candidate. Fine for a local (a handful of hits in one
  body), potentially bad for a common name at file scope.

### Option B — Lazy per-module reference index (`Dictionary<Decl*, List<SourceLoc>>`)

One `iterateAST` walk per module, on first rename/references request, caching
`Decl* → List<SourceLoc>` on `WorkspaceVersion` next to `markupASTs`. Rename = one dictionary
lookup.

- **+** One O(module) walk answers _all_ queries for that module, versus Option A's O(module) per
  candidate. Strictly better asymptotics once there is more than a couple of hits.
- **+** Reuses the semantic-tokens traversal pattern verbatim; the collection callback is ~20 lines.
- **+** Immediately also gives us `textDocument/references` and `documentHighlight`, which are the
  natural follow-ons to this issue.
- **+** Invalidation is free: the index dies with `WorkspaceVersion`, and that is already total.
- **−** Pays the full walk even for a local rename, where Option A would touch one body. Mitigable
  by indexing lazily per module and/or short-circuiting locals to a body-scoped walk.
- **−** Memory: one entry per reference in the module. For shader-sized files this is trivial;
  worth measuring, not worth pre-optimizing.

### Option C — Persistent workspace-wide index (ccls model)

Background-index every `.slang`/`.hlsl` file under the workspace roots into a stable-id → locations
map that survives edits, with incremental re-indexing.

- **+** The only option that makes rename correct for files the user never opened.
- **−** Requires a stable symbol id (mangled names work for global decls, do not exist for locals), a
  background indexing thread, incremental invalidation, staleness reconciliation against dirty
  buffers, and probably an on-disk format. This is the largest single feature in the language server
  by a wide margin.
- **−** Blocked anyway by §2.4: bodies outside the primary file are not checked, so an index built
  from the current checking mode would contain declarations but almost no references. Fixing that
  means either re-checking each file as primary (N full checks) or relaxing `shouldSkipChecking`,
  which directly regresses the response-time optimization it exists for.

### Cross-file, in any option

Cross-file rename needs three things that are independent of the index question: a workspace file
list (§2.3 — `Workspace::init` enumerates directories but keeps only search paths), the ability to
load and check a file the user has not opened, and a way to get references out of bodies that
`shouldSkipChecking` currently skips. clangd's answer — re-parse each affected file and run the
AST-based rename in it — is directly transferable: use a textual prefilter to find the _candidate
files_, then for each candidate call `getOrLoadModule` with that file as primary (which checks its
bodies) and run the same intra-file reference collection. Cost is one module check per candidate
file, and the prefilter is what keeps the candidate set small.

---

## 7. Recommendation

**Phase 1 — locals and file-scoped symbols, index-free or lightly indexed.**
Implement `prepareRename` + `rename` for symbols whose references provably cannot escape the
current file: locals, parameters, and decls not visible outside the file. Resolve the position with
`findASTNodesAt`, collect references with a scoped `iterateAST` walk over the enclosing body (for a
local) or the module (for a file-scoped decl), build a `WorkspaceEdit` with `documentChanges`, and
**explicitly refuse everything else** with a clear message. This is the subset that (a) is fully
correct under §2.4's constraint, (b) is what rust-analyzer's local path does, and (c) is what clangd
does before it consults its index.

**Phase 2 — promote the walk to a cached per-module index (Option B).**
Move collection into a `Dictionary<Decl*, List<SourceLoc>>` on `WorkspaceVersion`, and light up
`textDocument/references` and `documentHighlight` from the same structure. Do this once Phase 1's
correctness questions (overloads, generics, ctor redirection, dedup) are settled against real tests,
so the index is not being designed and debugged at the same time.

**Phase 3 — cross-file, clangd-style.**
Add a workspace file list, a textual prefilter to nominate candidate files, and per-candidate
re-check with that file as primary. Revisit Option C only if profiling on a real shader workspace
says the re-check cost is unacceptable.

The through-line: **do not build a persistent reference index.** rust-analyzer, the system this was
modeled on, does not have one; clangd has one only because C++ TUs are prohibitively expensive to
re-parse, and it pays for it with `adjustRenameRanges`-shaped complexity. Slang's constraint is not
index lookup speed, it is that reference-bearing bodies are not checked at all outside the primary
file — and no index fixes that.

---

## 8. Open questions / decisions needed

1. **Scope of the first PR.** Locals-only, or locals + file-scoped decls? Locals-only is a smaller
   and sharper correctness story; file-scoped adds structs/functions, which is what most users will
   actually reach for first.
2. **Refuse or best-effort for cross-file symbols in Phase 1?** Refusing with "renaming `Foo` may
   affect other files; not yet supported" is honest. Silently renaming only the current file is a
   correctness trap. Recommendation: refuse, with a distinct message per reason (clangd's
   `ReasonToReject`).
3. **`documentChanges` vs `changes`** in `WorkspaceEdit` — recommendation is `documentChanges` (§5.5)
   purely because it fits `StructRttiInfo` without new reflection support. Worth confirming no
   editor in the support matrix needs `changes`.
4. **New-name validation.** Reuse the real Slang lexer to reject keywords and non-identifiers
   (rust-analyzer's `IdentifierKind::classify`); additionally check for shadowing collisions in the
   target scope (clangd's `lookupSiblingWithName`)? The second is more work and can be deferred.
5. **Should this land with `textDocument/references` too?** They share all the machinery, and
   references is strictly easier (read-only, no validation, partial results are acceptable). It may
   be the better first PR — it exercises the index/search path with none of the destructive risk.
6. **Multi-document test harness.** Phase 3 needs `runLanguageServerTest` to open several documents;
   worth deciding whether to extend the existing `LANG_SERVER` test type or add a new one.

## References

- Slang: `slang-language-server.cpp:1082` (gotoDefinition), `slang-language-server-ast-lookup.h:33`
  (findASTNodesAt), `slang-ast-iterator.h:608` (iterateAST),
  `slang-language-server-semantic-tokens.cpp:147` (full-AST reference walk),
  `slang-workspace-version.h:138` (WorkspaceVersion), `slang-check-decl.cpp:2177`
  (shouldSkipChecking), `slang-language-server-protocol.h:180` (TextEdit),
  `tools/slang-test/slang-test-main.cpp:2321` (LANG_SERVER test harness).
- rust-analyzer: `crates/ide/src/rename.rs`, `crates/ide-db/src/rename.rs`,
  `crates/ide-db/src/search.rs`, `crates/ide-db/src/symbol_index.rs`.
- clangd: `clang-tools-extra/clangd/refactor/Rename.cpp`, `clangd/index/Ref.h`,
  `clangd/FindTarget.cpp`.
- clang-rename: `clang/lib/Tooling/Refactoring/Rename/`, `clang::tooling::Replacements`.
- ccls: `src/indexer.hh` (`IndexFile`, `Usr`).
