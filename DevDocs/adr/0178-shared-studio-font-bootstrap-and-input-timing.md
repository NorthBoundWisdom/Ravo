# ADR-0178: Shared Studio font bootstrap and input timing

- Status: Accepted
- Date: 2026-10-11
- Scope: LC-00/08, PERF-01

## Decision

Studio's existing system-font and installed-family resolution moves unchanged
from main into a desktop-private C++ helper. Main remains the application font
owner and reapplies it on language changes. Desktop tests initialize the same
policy once, after QGuiApplication construction and before loading QML. Repeated
test setup must not overwrite a language/font deliberately selected by a test.

The helper runs synchronously on the GUI thread, owns no settings, cache,
executor or downloaded font. Existing family order, language fallback and empty
inventory behavior remain unchanged. There is no new dependency, public machine
contract, rendering algorithm, persistence schema or fallback.

Import keyboard observations retain their complete wall-clock boundary and
50 ms focus / 100 ms page-scroll ceilings. Additional correlated samples split
focus/pre-dispatch events, synchronous event dispatch and post-dispatch events.
Process CPU time is recorded separately when available;
it includes all process threads and cannot be interpreted as GUI-thread CPU.
macOS/Linux use the standard process clock. Windows uses kernel+user
[GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes),
because Microsoft's [CRT clock](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/clock?view=msvc-170)
reports elapsed wall time. The report identifies its clock; query/overflow
failure leaves an explicit null instead of substituting wall time.
Font families, QPA identity and output dimensions identify the workload. Cold
font/bootstrap cost is not a warm key sample, and an over-budget wall sample
remains over budget regardless of its CPU or phase attribution.

## Validation

Test generic-family resolution, preservation of explicit platform faces,
language family order and application/bootstrap parity. Retain the original
ordered keyboard identities, model-change bounds and timing thresholds. Collect
fresh-process Release before/after observations with full samples and explicit
font/QPA metadata, then run keyboard/focus, localization, layout/smoke and full
Ravo regression checks. Host contention and physical frame presentation require
separate evidence; Windows/Linux remain untested without their toolchains.
