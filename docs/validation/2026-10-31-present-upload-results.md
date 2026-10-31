# NES24 presentation upload evaluation

Deferred: neither tested region-upload prototype earns runtime adoption.
Engine base`a9455b5`, Zelda base`b2bd8fb`; no runtime commit or PR.

Four initial actual-SDL diagnostic runs covered stock SMB3 and Zelda's full
4096x1472 seamless atlas with all128room simulations, software and accelerated
Windows D3D11. Atlas uploaded7.2GB across299presents. The nested counter
scopes measure CPU-side wall costs, not exclusive GPU execution or recoverable
savings; renderer name/flags do not identify the physical adapter.

The prototype retains the complete CPU picture and registers optional producer
damage separately from the existing host-extras ABI. Its independent pending
ledger covers old actor restoration, new actors and HUD until successful GPU
ACK; ownership/dimension/camera/load/texture-reset and unsupported paths force
full uploads. Failure retries the complete image. This design is not adopted.

| Version | Backend | Reference CPU s | Candidate CPU s | Reduction |
| --- | --- | ---: | ---: | ---: |
| v1 | software | 13.562500 | 14.906250 | -9.91% |
| v1 | direct3d11 | 13.046875 | 13.968750 | -7.07% |
| v2 | software | 12.609375 | 13.031250 | -3.35% |
| v2 | direct3d11 | 13.421875 | 22.093750 | -64.61% |

Each row is one matched1200frame screening pair on this PC, after excluded
setup/load/window/menu work. These are early-stop rejection receipts, not a
qualification matrix or a universal regression estimate. v1's64x32tiles exceed
the256rectangle cap on every eligible present and fall back to full upload.
A separate120frame diagnostic isolates that cause. v2's128x64tiles reduce
traffic43.62%, but require158193rectangle uploads across1199presents. D3D11
upload wall cost rises1.53s to10.54s; software0.936s to0.965s. Smaller transfers
do not outweigh call/planning cost. After two unsuccessful versions, stop tuning.

The root uploader unit and independent frozen-source CPU/GPU-buffer oracles
pass:55CPU pictures/83pixel comparisons per version, repeated CPU-only pictures,
skipped GPU frames, old/new actors/HUD, stale ACK, geometry/storage/camera/
revision/visibility invalidations, ownership epochs and injected upload failures.
Actual candidate GPU pixel/boundary qualification was not expanded after the
negative runtime screen. Synthetic correctness is not game qualification.

Eligibility also covers sufficiently large seamless Follow views, but those
were not measured. Built SMB3 controls were not launched after the selected
target failed; other titles, HD and voxel paths remain unchanged/unmeasured.
Indexed GPU conversion, broader resident producer designs and other upload
APIs remain explicit residuals, not exhausted opportunities. Native partial-frame
CPU picture semantics must remain correct in any future implementation.

Raw samples, sources, exact compile/link/object/binary identities, settings,
muted-audio/backend commands and capacity telemetry are in four verified
archives. Disposable three staging roots are cleaned after retention; bounded
prototypes/products and required evidence remain outside delivery folders.
See the [machine-readable record](2026-10-31-present-upload-results.json).

Final combined supported five-game builds/features/performance and owner visual
approval remain open. No merge or release; FDS stays parked.
