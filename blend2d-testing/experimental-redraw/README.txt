Fixed-document redraw review for dev 3645e2e

Configure with BLEND2D_STATIC=ON and BLEND2D_EXPERIMENTAL_REDRAW_TEST=ON.
Run ctest from the build directory. The three review fixtures compare
synchronous rendering with threaded rendering, signed packed stroke outlines,
borrowed paths, repeated recorded batches, and independent band owners.
The split-fill fixture includes conic controls and multiple merge groups.
It also verifies the legacy entry cutoff for paths whose CLOSE commands
have no vertices in the new representation, with exact pixels on both sides
of the threshold.

For a short create/replay/destroy lifetime loop, run bl_review_equivalence
with --lifetime from the build's experimental-redraw directory.
The recorded Valgrind check uses GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0
to release the libc stack cache before inspecting process exit.

The native benchmark wrapper accepts the same input record as the earlier
review. BLEND_PROBE_PRESTROKE=6 enables exact packed A/B/C stroke outlines.
BLEND_PROBE_PREPARED=1 precomputes state-change flags.
BLEND_PROBE_RECORD=1 records commands during bb_prepare().
BLEND_PROBE_BORROW=1 borrows strokes and 2 also borrows small fill paths.
BLEND_PROBE_TILES=2 or 4 creates disjoint raster-band owners. Each owner
uses its share of the requested thread count. Queue limits apply per owner.
BLEND_PROBE_TILE_CUT selects a 64-row-aligned boundary for two owners.
BLEND_PROBE_ASYNC_LIMIT is measured in bytes, matching the new dev API.

All frames clear, regenerate edges, rasterize and composite the full image.
Parsing, outline construction and command recording occur during loading.
Packed arrays and borrowed paths must stay immutable and alive until the
context ends. Rebuild the context when document data, transform, clip or
relevant drawing state changes.

The hooks are private fixed-document experiments. The packed arrays follow
the new separate vertex and reverse control streams, and use the native
stroke sink's sign changes. The original heap-outline and typed-color
experiments were omitted from this port.
