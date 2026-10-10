# app-github

esp-claw WASM app `gh`: read GitHub notifications, pull requests, issues, Actions runs and releases through the device's `github` credential profile. The app never sees the token; the firmware adds it to each request. The app is read-only.

## Tools

| Tool | Purpose |
|---|---|
| `gh_notifications(all?)` | Unread notifications (`all=true` adds read ones): repo, type, title, reason, updated |
| `gh_prs(repo?)` | Open PRs that involve you (`is:open is:pr involves:@me`), or all open PRs of one repo: repo, number, title, author, updated, draft |
| `gh_issues(repo, state?)` | Issues without PRs, most recently updated first: number, title, author, comments, labels, updated |
| `gh_ci(repo, branch?)` | The latest 8 workflow runs: run_id, workflow, branch, event, status, conclusion, created, title |
| `gh_failed_log(repo, run_id)` | Jobs of the run with their failed steps, plus the `##[error]` lines and last 40 lines of the first failed job's log |
| `gh_releases(repo)` | The latest 5 releases: tag, name, published, draft/pre flag, asset count, downloads |
| `gh_repo(repo)` | Stars, forks, open issues+PRs, watchers, default branch, last push, language, license |
| `gh_set_owner(owner)` | Sets the default owner, stored in the app's kv as `owner`. With it, `repo` can be just `name`. An empty string clears it |

`repo` is `owner/name`, or `name` once a default owner is set. Results are compact JSON (`cols` plus `rows`, trimmed fields). A result says `"truncated": true` when it hit the 32 KB response cap or the 16 KB result cap.

## Device profile

Add this entry to `api_profiles` in the device config:

```json
{"name": "github", "base_url": "https://api.github.com", "hosts": ["api.github.com", "*.blob.core.windows.net"],
 "auth": {"type": "bearer", "token": "<GITHUB_TOKEN>"}}
```

- The app sends `Accept: application/vnd.github+json`, `X-GitHub-Api-Version: 2022-11-28` and `User-Agent: esp-claw-app-gh/1.0`, which GitHub requires. The firmware passes app headers through and drops only `Authorization`.
- `*.blob.core.windows.net` is only needed for the log part of `gh_failed_log`. GitHub answers the job-log request with a 302 to a short-lived Azure blob URL, and the firmware follows a redirect only to hosts in the profile. Without this host, `gh_failed_log` still returns the jobs and failed step names and reports that the log download failed. **Caution:** the firmware currently keeps the `Authorization` header when it follows the redirect, so your GitHub token would also go to the blob host. See "Open points" below.
- Token: a classic PAT with `repo` (or `public_repo`) and `notifications`. Fine-grained tokens cannot read `/notifications`. Everything else works with a fine-grained token that has read access to Contents, Issues, Pull requests, Actions and Metadata.

## Build

```sh
git clone --recursive https://github.com/esp-claw/app-github.git
cd app-github && ./sdk/build-app.sh .
```

The build needs clang with the wasm32 target and wasm-ld (Debian/Ubuntu: `apt install clang lld`). It writes `build/app.wasm` (about 28 KB) and `build/manifest.json` (128 KB memory, 15 s per call). CI builds every push, checks the manifest and the module with the SDK's `manifest_check.py` and `wasm_check.py`, and attaches both files to releases.

## Install

Install a signed release in Telegram (see [Releases](#releases)):

```
/app install https://github.com/esp-claw/app-github/releases/download/v1.0.0/gh-1.0.0.ecapp
```

The device must trust the esp-claw key: `app_trusted_keys` must contain `XHQYDdKaRZ6pNzXYcrGAB8bTyPa3D22NU+lVzAt2Acw=` (see [trusted keys](https://github.com/esp-claw/app-sdk#trusted-keys)). The device asks for confirmation before installing. To update, send `/app update gh <url of the newer .ecapp>`. `/app remove gh` deletes the app and its data.

To test a local build, use a development device with `app_allow_unsigned` set to `true`. Copy `build/manifest.json` and `build/app.wasm` to `/apps/gh/`, for example over WebDAV (`dav://<device>/dav/apps/gh/`). Then send `/apps reload gh` in Telegram.

## Releases

Each release `v<version>` carries the signed package `gh-<version>.ecapp`, signed with the esp-claw key (key_id `7087d6a8`). It also carries the CI build it was made from (`app.wasm`, `manifest.json`). Install a release with:

```
/app install https://github.com/esp-claw/app-github/releases/download/v<version>/gh-<version>.ecapp
```

Maintainers: bump `version` in `manifest.json` and publish a release tagged `v<version>`. CI attaches the build. Then sign it locally and upload the `.ecapp`, as described in the [app-sdk release steps](https://github.com/esp-claw/app-sdk#releasing-an-app).

## Status

Runtime v1 in [esp-claw/esp-claw](https://github.com/esp-claw/esp-claw). The app passes a host harness (WAMR with mocked host functions and hand-written API fixtures) but has not run on a device yet.

How the app handles response sizes: a notification carries a ~5 KB repository object and a workflow run ~10-15 KB, so the app asks for small pages (5 notifications or 2 runs per request) and pages through them. Elements cut off by the 32 KB response cap are skipped and reported as truncated.

## Open points

These are not verified on a device:
- Job log download. The redirect target host, whether Azure accepts the request while it still carries the GitHub `Authorization` header, and whether the blob host honors `Range: bytes=-24576` (tail only). Without Range support the device returns the first 32 KB of the log, and the result shows `"head_only": true`.
- Azure SAS URLs can be longer than the firmware's 512-byte redirect buffer. That buffer is used only for the host check, so this is probably harmless.

Part of [esp-claw (community)](https://github.com/esp-claw), an unofficial community fork of espressif/esp-claw. It is not affiliated with Espressif.

## License

Apache-2.0
