# Ravo GPU admission gates

Ravo is CPU-only. A GPU backend may be added only as an Engine adapter after a
complete Ravo CPU workload, correctness baseline, ownership model, and measured
end-to-end benefit are accepted. Frozen 0.9 OpenCL code is static reference
only; it must not be configured, built, run, ported, or used as a live oracle.

## Required workloads

Use four versioned, redistributable photo inputs. The manifest records source
and recipe SHA-256, license, camera, dimensions, Ravo/dependency commit, output
profile, and operation order.

| ID | Content | Required coverage |
| --- | --- | --- |
| `raw-detail` | High-resolution, fine-texture, low-ISO Bayer RAW | demosaic, lens, input/output colour, local detail |
| `raw-noise` | High-ISO Bayer or X-Trans RAW with deep shadows | demosaic, denoise, exposure, tone mapping |
| `raw-geometry` | Architectural or grid RAW | Canvas/Perspective/crop, lens geometry, interpolation |
| `raw-mask` | Mixed luminance and saturated colour | canonical mask graph, blend, colour operation, overlay parity |

Each workload measures the same immutable recipe through:

1. 960px interactive preview, including intent-to-owned-image publication;
2. 1600px settled preview, including cache publication;
3. fixed 100% ROI inspection with no window or zoom-state dependency;
4. full-size high-precision export with no downscale.

Private inputs and reports stay outside the repository. Committed manifests and
goldens must use redistributable data.

## CPU baseline

The designated Release commit is rendered only through Ravo's Engine,
CatalogService, CLI, and existing probe/export contracts. Record:

- exact input, recipe, dependency, binary, and profile identities;
- output dimensions, ROI, channels, alpha/mask state, and finite-value checks;
- 32-bit float or another engine-owned high-precision reference at critical
  stage boundaries, plus the final display result;
- warmups, at least seven measured runs, median, P90, dispersion, peak memory,
  cancellation latency, and resource destruction after close;
- fixed host, OS, compiler, CPU/thread budget, power state, and background load.

An 8-bit JPEG alone is not a correctness reference. A single timing run is a
smoke check, not performance evidence.

## Correctness gates

Every GPU candidate first satisfies:

- identical dimensions, channels, ROI, geometry mapping, alpha, and mask
  semantics; discrete labels and indices are bit-exact;
- finite output with no NaN/Inf repair hidden by packing;
- ordinary floating-point operations: normalized-channel RMSE at most `2e-6`
  and maximum absolute error at most `2e-5`;
- iterative, reduction, denoise, or demosaic operations: RMSE at most `2e-5`
  and maximum absolute error at most `2e-4`;
- final display result: CIEDE2000 P99 at most `0.25` and maximum at most `1.0`,
  followed by direct artifact inspection of edges, shadows, highlights, masks,
  and geometry boundaries.

Platform-specific CPU drift must be measured independently. An exception is
operation-, parameter-, and platform-scoped; one failing candidate must not
loosen a repository-wide tolerance.

## Ownership and failure behavior

- Engine owns the backend-neutral request and result contract. The GPU adapter
  owns device objects, command queues, buffers, synchronization, and teardown.
- Recipe, services, CLI, Studio, QML, and the catalog do not gain GPU-specific
  business state or a second renderer.
- Every asynchronous result carries asset/request revision and cancellation.
  Close or destruction waits for owned work and releases device resources.
- Allocation, compilation, device-loss, timeout, non-finite, and cancellation
  failures publish no partial image. Any CPU retry policy is explicit in the
  request/result contract and tests; there is no silent per-operation fallback.
- Cache identity includes backend and implementation version wherever CPU/GPU
  output is not bit-identical.

## Performance admission

Choose a contiguous operation chain from measured end-to-end cost, not source
file or kernel order. It must amortize upload/download and format conversion,
show a repeatable complete-workflow median improvement, avoid P90 regression,
and stay within an accepted memory/energy budget. A faster isolated kernel does
not qualify if preview publication or export becomes slower.

## Exit checklist

- [ ] Four-workload manifest and redistributable inputs.
- [ ] Release CPU goldens for all four product paths.
- [ ] Versioned Ravo CPU/GPU comparison report schema and runner.
- [ ] Cancellation, failure, cache, and destruction tests.
- [ ] First contiguous chain selected from measured profiles.
- [ ] Correctness thresholds pass on every supported target backend.
- [ ] End-to-end median/P90, peak-memory, and energy gates pass.

Only after every item is accepted may production code add a GPU adapter.
