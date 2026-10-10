# Lightroom integration acceptance gates

Scope: the supplied RAWmakase catalog and Lightroom interchange capabilities.
[ADR-0166](adr/0166-lightroom-data-preservation-and-develop.md) owns policy;
[Ravo/README.md](../Ravo/README.md) owns implemented behavior. Raw-source
preservation does not complete the following operational capabilities.

## Library and persistence

- Register offline photos as stable foreign identities without inventing byte
  hashes; qualify later verified relink and independent offline virtual copies.
- Add first-class copy names, custom/localized label text and keyword export
  flags through persistence, recovery, presentation and metadata export.
- Add nested collection-set ownership and read-only imported membership;
  preserve smart definitions while distinguishing stored static membership
  from evaluated Ravo queries. Do not interpret Adobe rules implicitly.
- Qualify catalog capture dates, camera/lens/dimensions and orientations when
  source metadata is absent. Prevent double application of camera orientation.
- Add per-computer root/folder locations, descendant precedence, clearing and
  ambiguity handling through shared services/CLI. Preserve source identity
  checks rather than relinking by filename alone.
- Qualify stage completion, idempotence and recovery-publication checks for
  automatic retry of partial photos. ADR-0167 commits business proofs atomically
  and resumes untouched work, but does not establish whole-photo completion.

## Develop and dependencies

- Qualify custom/named/Auto white balance against the sensor coefficient owner;
  preserve Kelvin/tint without guessing sensor RGB multipliers.
- Integrate DCP/enhanced profile identity, missing-profile diagnostics and
  Profile Amount through an admitted Engine owner and licensed fixtures.
- Translate compatible lens corrections, manual Transform and stored Upright
  with parameter/coordinate contracts and pixel fixtures.
- Translate brush/gradient/radial/luminance masks, spot removal, red eye and
  Pet Eye using Ravo-owned operations. Keep unsupported vendor masks explicit.
- Qualify legacy 2003/2010 and current process versions with real frozen inputs;
  exact legacy exposure EV is mapped, while other active legacy tonal controls
  remain explicit omissions.
- Expose raw settings and unsupported states per photo/history step for later
  inspection/re-application. The immutable source remains the preservation
  authority rather than an editable foreign store.
- Inventory external `.lrcat-data`/ACR dependencies; archiving only `.lrcat`
  does not establish preservation of external AI editing data.

## Evidence

- Obtain licensed real catalogs/version fixtures; keep private media/paths out
  of Git. Compare converted pixels before claiming appearance equivalence.
- Exercise 2 GB/expanded-record/membership bounds, disk exhaustion, cancellation
  during hash/SQL/recipe writes, late Studio results and owner destruction.
- Qualify corrupt archive chunks, full backup-package restore/readback and
  concurrent output competitors beyond existing archive transaction tests.
- Run native CLI success/errors, Studio offscreen checks, the full Ravo
  unit/contract suite and available Windows/macOS/Linux builds.
- Verify catalog, originals and pre-existing sidecars remain byte-identical.
