---
name: sync-upstream-release
description: Publish a static release of this fork for a new upstream Slang release. Rebases master onto the upstream tag, verifies the build with a draft tag, then force-pushes master and pushes the release tag.
allowed-tools:
  - Bash
  - Read
  - Grep
  - Glob
---

# Sync Upstream Release

Publish `vX-static` on `Giesch/slang` for the upstream release `vX`.

Usage: `/sync-upstream-release [vX]`. The argument is optional.

## Layout

- Remotes: `upstream` is `shader-slang/slang`, `fork` is `Giesch/slang`.
- Branch: `master`. The fork has no `main`.
- History: `fork/master` is the upstream tag plus the fork's commits, rebased on top. There are no merge commits from upstream.
- Release tag: annotated `vX-static`. Message: `Static Slang vX: embedded slang-glslang, bundled archive`.
- Draft tag: `vX-static-draft`. The workflow publishes it as a draft prerelease with assets named `slang-static-X-draft-<platform>.*`.
- Workflow: `.github/workflows/release-static.yml`. It runs on every `v20*` tag push.
- Release assets: 3 platforms (`linux-x86_64`, `macos-aarch64`, `windows-x86_64`) times 3 formats (`tar.gz`, `tar.xz`, `zip`), plus `SHA256SUMS`. 10 files in total.
- The upstream tag `vX` is not pushed to the fork. Pushing it triggers a second release run.

## Gates

Stop and ask the user before each of these actions:

- Continuing a rebase after a conflict.
- Pushing the draft tag.
- Force-pushing `master`.
- Pushing the release tag.
- Deleting the draft release and draft tag.

Do not run `git rebase --abort` without telling the user.

## Step 0: Preconditions

```bash
gh auth status
git remote -v            # must list fork and upstream
git status --porcelain   # must be empty
git branch --show-current # must be master
git fetch fork upstream --tags
git rev-parse master fork/master # must match
```

Stop if any check fails. Report the failing check.

## Step 1: Determine the version

- With an argument: `VERSION` is the argument. Add the leading `v` if missing.
- Without an argument: use the newest upstream release that is not a draft or prerelease. Print it and ask the user to confirm.

```bash
gh release list -R shader-slang/slang --exclude-pre-releases --exclude-drafts --limit 1 --json tagName -q '.[0].tagName'
```

Validate:

```bash
git rev-parse "$VERSION^{commit}"                 # tag must exist locally after the fetch
git ls-remote --tags fork "refs/tags/$VERSION-static" # must print nothing
```

Stop if `$VERSION-static` already exists on `fork`.

## Step 2: Rebase master onto the upstream tag

Find the upstream tag `master` currently sits on, and record the fork commit count:

```bash
PREV=$(git describe --tags --match 'v20*' --exclude '*-static*' --abbrev=0 master)
BEFORE=$(git rev-list --count "$PREV..master")
git log --oneline "$PREV..master"
```

Rebase:

```bash
git rebase --onto "$VERSION" "$PREV" master
```

On a conflict:

1. Resolve the conflict. Prefer the upstream change for files the fork does not own. Keep the fork's intent for the static build files (`CMakeLists.txt`, `external/CMakeLists.txt`, `cmake/BundleStaticLibrary*.cmake`, `source/slang/CMakeLists.txt`, `source/slang-glslang/`, `extras/static-release/`, `.github/workflows/release-static.yml`).
2. `git add` the resolved files.
3. Show `git diff --cached` to the user. Wait for approval.
4. `git rebase --continue`.

After the rebase:

```bash
git merge-base --is-ancestor "$VERSION" master    # must succeed
AFTER=$(git rev-list --count "$VERSION..master")
test "$AFTER" -eq "$BEFORE"                        # same fork commit count
git log --oneline "$VERSION..master"               # only fork commits
```

Stop if a check fails.

## Step 3: Verify with a draft release

Create and push the draft tag. This does not change `fork/master`.

```bash
git tag -a "$VERSION-static-draft" -m "Static Slang $VERSION: embedded slang-glslang, bundled archive (draft)"
git push fork "$VERSION-static-draft"
```

Watch the run:

```bash
RUN=$(gh run list -R Giesch/slang --workflow=release-static.yml --branch "$VERSION-static-draft" --limit 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN" -R Giesch/slang --exit-status
```

The run takes roughly 30 minutes. If `gh run list` returns nothing, wait 30 seconds and retry.

On failure:

```bash
gh run view "$RUN" -R Giesch/slang --log-failed
```

Report the failed jobs and stop. Leave `master` unpushed. Leave the draft tag in place for investigation.

On success, verify the draft assets:

```bash
gh release view "$VERSION-static-draft" -R Giesch/slang --json assets -q '.assets[].name'
```

Expect 10 names: 9 archives with the `-draft` infix and `SHA256SUMS`.

## Step 4: Publish master and the release tag

```bash
git push --force-with-lease fork master
git tag -a "$VERSION-static" -m "Static Slang $VERSION: embedded slang-glslang, bundled archive"
git push fork "$VERSION-static"
```

Watch the run as in Step 3, with `--branch "$VERSION-static"`. On success, verify the release assets:

```bash
gh release view "$VERSION-static" -R Giesch/slang --json assets,url -q '.url, .assets[].name'
```

Expect 10 names: 9 archives named `slang-static-X-<platform>.<ext>` and `SHA256SUMS`.

## Step 5: Delete the draft

```bash
gh release delete "$VERSION-static-draft" -R Giesch/slang --yes --cleanup-tag
git tag -d "$VERSION-static-draft"
```

If `--cleanup-tag` is not available, run `git push fork --delete "$VERSION-static-draft"` after the release delete.

## Final report

- Release URL.
- Asset list.
- Fork commits on top of upstream: `git log --oneline "$VERSION..$VERSION-static"`.

## Interactive Workflow

1. Check preconditions (Step 0).
2. Determine `VERSION` (Step 1). Confirm it with the user when auto-detected.
3. Rebase `master` onto `VERSION` (Step 2). Gate on conflicts.
4. Push the draft tag (gate) and watch the run (Step 3). Stop on failure.
5. Force-push `master` (gate), push the release tag (gate), watch the run (Step 4).
6. Delete the draft release and tag (gate) (Step 5).
7. Print the final report.
