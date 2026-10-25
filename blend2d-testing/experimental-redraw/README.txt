These are optional Blend2D-only native correctness fixtures and caller examples.
Start with REVIEW.txt at the repository root.
No Rasterizer/wgpu code or runtime is needed. review_draw.h is a plain input-record
layout; the scene importer and external GPU benchmark are not part of this tree.

CMake option: BLEND2D_EXPERIMENTAL_REDRAW=ON (default OFF; static POSIX build).
Targets: bl_review_equivalence, bl_review_split_fill, bl_review_render,
bl_review_type_predicates. The public API remains the dependency of the ordinary
fixtures; private C hooks are used by the retained-redraw caller fixture.

Sources preserve the measured experimental implementation, including rejected
outline modes for diagnosis. For accepted retention use PRESTROKE=6 and RECORD=1;
use BORROW=2 for the lighter path route. TILE_MODE=clip and public-outline mode2
were rejected for pixel differences and should not be reported as correct wins.
The environment controls have the BLEND_PROBE_ prefix; see the caller source and
results/cpu16-selected-plans.json for exact measured choices.

Build and test instructions, scope, lifetime rules and caveats are in root REVIEW.txt.
The benchmark fixture is a research caller example, not part of the stable API.
