# Glossary

| Term | Meaning in Ravo |
| --- | --- |
| Asset | One catalog record pointing to one original file. |
| Catalog / library | The local SQLite database that stores assets, review state, metadata, recipes, and history. Studio calls it a library. |
| Original | The user-owned source file imported by reference. Ravo keeps it read-only during normal workflows. |
| Preview | A rendered or embedded image used for Gallery, Loupe, or Edit display. |
| Preview cache | Rebuildable PNG files stored outside the SQLite catalog. |
| Recovery mirror | A checksummed, catalog-owned `.ravo.json` generation derived from durable SQLite state under `<catalog>.ravo/sidecars/`. It is not adjacent interchange metadata or a live authority. |
| Catalog backup | An immutable verified directory containing a preview-free SQLite snapshot, manifest, and recovery mirrors. It excludes originals and previews. |
| Recipe | A versioned JSON description of the Develop operations and parameters for an asset. |
| Recipe style / preset | A `.rstyle.json` recipe template. Schema v1 replaces a complete recipe; schema v2 overlays explicitly selected logical fields. |
| Baseline | The Ravo product rendering state before user edits. RAW baseline rendering includes the default Sigmoid Standard SDR display transform and a mild Lab unsharp mask. |
| Develop | Ravo's non-destructive editing workflow. The Studio view is labeled **Edit**. |
| Active photo | The primary selected asset shown in Loupe, Edit, and Inspector, and used by single-photo export. Batch export can use the wider selection. |
| Review state | Rating, color label, and rejected/kept state stored in the catalog. |
| Snapshot | A labeled copy of the current recipe stored in recipe history. |
| History entry | A stored recipe state that can be restored. It may be an automatic history record or a labeled snapshot. |
| Input profile | The declared color profile used to interpret source pixels. |
| Working profile | The color space in which the recipe's scene or color operations run. |
| Output profile | The declared color profile used for final display and encoded export. |
| Soft proof | A preview through a declared proof profile and rendering intent. |
| Deflicker | RAW exposure analysis based on the original sensor data histogram. |
| RAW | A camera sensor file decoded through the pinned LibRaw path. |
| RGB parade | A channel-separated scope showing red, green, and blue distributions across image position. |
| No-replace publication | The output rule that refuses an existing destination instead of overwriting it. |
| Structured error | A typed error with a stable code, message, and context; CLI JSON returns it under `error`. |
