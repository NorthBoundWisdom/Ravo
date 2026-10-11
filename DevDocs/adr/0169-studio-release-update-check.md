# ADR-0169: Studio GitHub release update checks

- Date: 2026-10-11
- Status: Accepted

## Decision

The user-requested update notification is a bounded desktop capability. Studio
checks the public `NorthBoundWisdom/Ravo` GitHub latest-release endpoint manually
from Help and once after startup handoff. The per-user preference
`desktop/updates/automatic` defaults to true. It is independent of catalog state;
manual checks remain available when automatic checks are disabled.

`StudioUpdateController` owns one Qt Network manager/reply and an absolute
15-second deadline on the main-thread event loop. It introduces no dependency,
thread, service/catalog schema, download manager or installer. The response is
bounded to 1 MiB, redirects are rejected, TLS verification remains enabled,
and no credentials, catalog or image information are sent. The User-Agent names
Studio and its compiled version. Failed requests are not retried automatically.
Shutdown cancels and disconnects the reply; stale signals cannot publish a result.

CMake's project version is the application and comparison authority. Only
three-component numeric tags with an optional `v` prefix are accepted. Components
are compared numerically without overflow; same/older versions do not prompt an
upgrade. Drafts, prereleases, malformed tags, invalid release links, HTTP/network
failures, rate limits and oversized responses have explicit error states. No
failure becomes an up-to-date result and no alternate update source is used.

Manual checks show progress and results. Automatic checks only request a dialog
when a newer formal release is available. Presentation waits for an active main
window and existing modal surfaces to clear. Dismissal does not change the
automatic-check preference. A manual intent joins an outstanding automatic
request rather than issuing another one. The explicit release button alone
opens the verified HTTPS GitHub page for that tag through QDesktopServices;
browser-open failure remains visible. No binary is downloaded or installed.

Settings persistence reports failures without publishing a new typed value.
Malformed stored preferences are removed synchronously and use the documented
default only if repair succeeds. QML presents state and forwards intents;
version parsing, HTTP and preferences remain in C++.

## Validation

Desktop contracts use an injected Qt Network manager/reply and URL opener, never
live GitHub or a real browser. They cover numeric versions, invalid/unsupported
release metadata, URL identity, offline/HTTP/timeout/size failures, request
coalescing, cancellation, destruction, late signals, settings and modal deferral.
Production QML, command registry and translation/package smoke remain required.
Smoke startup disables automatic HTTP. Platform browser launch is a user-run
installed-package check; headless tests establish dispatch, not browser behavior.

See [architecture](../ARCHITECTURE.md), [test policy](../TESTING.md) and
[GitHub's release API](https://docs.github.com/en/rest/releases/releases#get-the-latest-release).
