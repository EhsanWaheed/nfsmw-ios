# Metal 4 renderer (branch `metal4`)

Goal: the Direct3D 9 renderer (`kit/host/gpu/metal/d3d9_metal.mm`) encodes on a Metal 4 queue, with MetalFX 4
(`MTL4FXSpatialScaler` / `MTL4FXTemporalScaler`), while the classic path stays as it is. v3 (`main`, tag
`v3.0-ios`) is the stable release and is not touched.

## Switch

Diagnostics → *Renderer*: **Metal 4** (default since 17 Oct 2026) / **Metal (compatibility)**, next launch
(`RECOMP_METAL4=1`). The
Metal 4 path is used only when the device supports it (`supportsFamily:MTLGPUFamilyMetal4`); otherwise the classic
path runs and the log says why. Any stage can be turned off again with the switch, with no reinstall.

## What changes (and what does not)

- **Unchanged:** the generated shaders (argument-table slots are the same `[[buffer(n)]]` / `[[texture(n)]]` /
  `[[sampler(n)]]` numbers), the pipeline states (`MTL4RenderCommandEncoder` takes `id<MTLRenderPipelineState>`
  made by `newRenderPipelineStateWithDescriptor:`), libraries, textures, buffers, samplers, depth-stencil states.
- **Encoder calls** (`[enc_ ...]`, ~120 sites): go through one small wrapper that drives either encoder. Metal 4
  encoders have no per-stage bind calls: buffers and textures go into an `MTL4ArgumentTable` per stage (vertex,
  fragment, compute) bound once per encoder; `setVertexBytes` / `setFragmentBytes` become a copy into a per-frame
  upload ring (a shared `MTLBuffer`) and `setAddress:` of that copy's GPU address.
- **Command buffers:** `MTL4CommandBuffer` from the device, one `MTL4CommandAllocator` per frame in flight (3),
  committed to an `MTL4CommandQueue`. Completion (`completed_`, the frame semaphore, GPU time) from an
  `MTLSharedEvent` the queue signals with the frame serial (`signalEvent:value:`), plus the commit feedback handler.
  `wait()` / `settle()` wait on the event value instead of `waitUntilCompleted`.
- **Hazards:** Metal 4 tracks nothing. Stage 3 starts with one conservative consumer barrier at the start of every
  pass (`barrierAfterQueueStages:all beforeStages:all`), the same ordering the classic path gets for free, then
  narrows it (render → fragment-read only where a pass samples an earlier target).
- **Lifetime:** Metal 4 command buffers do not retain resources. A texture or buffer the game drops while a frame
  that used it is in flight goes to a graveyard with the current serial and is released when the event passes it.
- **Residency:** every buffer and texture the renderer makes joins one `MTLResidencySet` (the `note_alloc` call
  sites already see every allocation); it is committed before each command buffer commit and added to the queue.
- **Presenter:** it keeps the classic queue. Today the renderer shares that queue, so order is free; with Metal 4 the
  presenter's command buffer waits on the shared event (`encodeWaitForEvent:value:`) for the frame it shows.
- **MetalFX 4:** `MTLFXSpatialScalerDescriptor newSpatialScalerWithDevice:compiler:` /
  `MTLFXTemporalScalerDescriptor newTemporalScalerWithDevice:compiler:` (needs an `MTL4Compiler`), encoded into the
  `MTL4CommandBuffer`; the same inputs as today (scene, depth, per-draw motion, jitter).
- **Later (stage 5):** `MTL4Compiler` for pipelines (async builds, pipeline data sets) to cut first-use hitches.

## Stages (each one built, device-tested and committed before the next)

1. Switch + Metal 4 queue, allocators, command buffers, shared-event completion, residency set, graveyard; the
   presenter waits on the event. Encoders still classic? No: an MTL4 command buffer only takes MTL4 encoders, so
   stage 1 also brings the encoder wrapper with the upload ring and argument tables (stage 2's work) for the render
   path, with the conservative barrier (stage 3's first step).
2. Compute and blit passes (`MTL4ComputeCommandEncoder` does both), occlusion queries (visibility result buffer),
   readbacks, the foundation's motion kernel, the depth copy.
3. Narrower barriers (performance).
4. MetalFX 4 spatial and temporal.
5. `MTL4Compiler` pipelines and shader warm-up.

Test checklist per stage: menus, a race (day, rain), the mirror, car reflections, a pause, lock/unlock, the intro
movie, a resolution change, temporal on/off; log has no `command buffer failed` / validation errors.
