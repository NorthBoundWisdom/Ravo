# Code retirement execution

Scope and preservation rules are owned by
[ADR-0181](adr/0181-retire-unused-dependencies-and-product-stubs.md).
Only unfinished work remains here. Current retired surfaces, scalar descriptors,
test-only smoke and historical-input isolation belong to the ADR, architecture,
capability reference and testing authority.

1. Consolidate remaining nested/enum Develop field metadata and Qt projections
   where semantics are independent. Remove heuristic range/type inference only
   after specifying context-dependent constraints; do not encode an identity-only
   probe as a universal range. Preserve IDs, units, strict rejection, coupled/group
   resets, multi-instances and mask behavior. Gate: compare complete CLI field
   inventories, prove ordinary recipe round-trip/fixtures unchanged, and run full
   service/CLI/desktop functional contracts. Retain explicit special handlers when
   a descriptor would hide their invariants.
2. Qualify Windows/Linux testing-on/off builds for the shared static QML resource
   owner and test-only workflow-smoke composition. Gate: native compiler/linker,
   Qt resource/type registration, runtime DLL/plugin deployment, localisation,
   workflow smoke and unchanged CPU/recipe fixtures on each actual host.
3. Re-run clean installed-package startup/video acceptance for the changed resource
   linkage. Package-checker unit tests and build-tree smoke do not establish native
   installed-package qualification. Keep original/backup/recovery and accepted XMP
   import contracts; do not restore retired providers or unsupported probes.

Validation uses the active host preset and supported machine interfaces. Run the
full Ravo functional suite for public API/broad changes; performance observations
may remain deferred per the user. Windows/Linux and native installed-package
qualification require their actual toolchains/hosts; do not infer them from macOS.
