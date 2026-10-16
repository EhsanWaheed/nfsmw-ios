// d3d9_encoders.h - the command buffer and encoders the Direct3D 9 renderer (d3d9_metal.mm) encodes into: one set
// of objects that answer the same messages as Metal's own (setVertexBytes:length:atIndex:, drawPrimitives:..., and
// so on), so the renderer's call sites read as before, while the objects decide which Metal does the work
// (docs/METAL4_PLAN.md).
//
// Classic Metal: each object forwards to the MTLCommandBuffer / encoder it wraps.
// Metal 4 (D9M4, RECOMP_METAL4=1 on a device of the Metal 4 family): command buffers come from the device and go to
// an MTL4CommandQueue; encoders bind through MTL4ArgumentTables (one per stage, an entry set only when it changes);
// setVertexBytes and the like copy into the command buffer's upload chunks (the same bytes as the slot's last upload
// in this command buffer: that upload again) and bind their GPU address. Every resource the GPU work touches joins
// one long-lived residency set on the queue, which also keeps it alive (Metal 4 command buffers retain nothing)
// until it has been unused for a while and the GPU is past its last use. Hazards (Metal 4 tracks none): "safe" puts a
// consumer barrier at the start of every pass (the classic ordering); "precise" (default) tracks the textures written
// and read since the last barrier and puts one only where a pass reads what an earlier one wrote, or writes what an
// earlier one read or wrote. Completion: the queue signals the shared event with each command buffer's value.
#pragma once
#import <Metal/Metal.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Called once when the command buffer finished on the GPU: `error` nil on success, `gpu_s` the GPU time (0 unknown).
typedef void (^D9CmdDone)(NSError *error, double gpu_s);

@class D9Cmd;

// The Metal 4 renderer's shared state, render thread only (the pools below lock: feedback handlers return to them).
// One flat open-addressing table (no allocation per resource, one lookup per bind) does three jobs per resource:
// residency (the queue's set), lifetime (a strong reference, released once the GPU is past its last use) and the
// precise-barrier hazards (the barrier generation it was last written / read in: a barrier is ++gen, nothing clears).
struct D9M4State {
    struct Slot {
        const void *key = nullptr;
        __strong id obj = nil;
        uint64_t last = 0;
        uint32_t wgen = 0, rgen = 0;
    };
    bool precise = true;   // RECOMP_METAL4_BARRIERS=safe: a barrier at the start of every pass
    id<MTLResidencySet> resident = nil;
    std::vector<Slot> slots;
    size_t count = 0, mask = 0;
    bool resident_dirty = false;
    uint64_t now = 1;   // the value the open command buffer will signal
    uint32_t gen = 1;   // the barrier generation
    // Argument-table entries as last set (kind 0 vertex, 1 fragment, 2 compute); ~0 = never.
    uint64_t addr[3][32];
    uint64_t tex[3][32];
    uint64_t smp[3][16];
    uint64_t barriers = 0, uploaded = 0, reused = 0, table_sets = 0, table_skips = 0;
    // Diagnostics (the perf line): residency changes, and the render thread's time by kind, measured on one frame in 8
    // (sampling = true) so the clock reads stay cheap. Kinds: 0 binds, 1 uploads, 2 draws, 3 passes, 4 commits.
    uint64_t res_adds = 0, res_removes = 0, res_commits = 0, uses = 0;
    bool sampling = false;
    uint64_t t_ns[5] = {}, sampled_frames = 0;
    D9M4State() {
        memset(addr, 0xff, sizeof addr);
        memset(tex, 0xff, sizeof tex);
        memset(smp, 0xff, sizeof smp);
        slots.resize(1u << 16);
        mask = slots.size() - 1;
    }
    size_t home(const void *k) const { return (size_t)(((uintptr_t)k >> 4) * 0x9E3779B97F4A7C15ull >> 20) & mask; }
    Slot *find(const void *k) {
        for (size_t i = home(k);; i = (i + 1) & mask) {
            Slot &x = slots[i];
            if (x.key == k)
                return &x;
            if (!x.key)
                return nullptr;
        }
    }
    void put(Slot &&v) {   // v.key not present
        for (size_t i = home(v.key);; i = (i + 1) & mask)
            if (!slots[i].key) {
                slots[i] = std::move(v);
                ++count;
                return;
            }
    }
    void rebuild(size_t capacity) {   // the survivors into a fresh table (no tombstones)
        std::vector<Slot> old;
        old.swap(slots);
        slots.resize(capacity);
        mask = capacity - 1;
        count = 0;
        for (Slot &x : old)
            if (x.key)
                put(std::move(x));
    }
    // This command buffer uses `r`: resident and alive until the GPU is past it. Nil: nullptr.
    Slot *use(id r) {
        if (!r)
            return nullptr;
        const void *k = (__bridge const void *)r;
        ++uses;
        if (Slot *x = find(k)) {
            x->last = now;
            return x;
        }
        ++res_adds;
        if ((count + 1) * 10 > slots.size() * 9)   // nearly full (the commit-time growth missed it): grow now
            rebuild(slots.size() * 2);
        [resident addAllocation:(id<MTLAllocation>)r];
        resident_dirty = true;
        Slot v;
        v.key = k;
        v.obj = r;
        v.last = now;
        put(std::move(v));
        return find(k);
    }
    // At each commit: every 8 command buffers what the GPU is past and the last 2 did not use leaves (released);
    // the table grows at half full. (Holding for 256 kept every buffer the game replaces each frame alive for ~4 s:
    // 3 GB on the owner's phone.)
    void sweep(uint64_t completed) {
        if (count * 2 > slots.size())
            rebuild(slots.size() * 2);
        if (now % 8 != 0)
            return;
        bool removed = false;
        for (Slot &x : slots)
            if (x.key && x.last + 2 < now && x.last <= completed) {
                [resident removeAllocation:(id<MTLAllocation>)x.obj];
                ++res_removes;
                x.key = nullptr;
                x.obj = nil;
                --count;
                removed = true;
            }
        if (removed) {
            resident_dirty = true;
            rebuild(slots.size());
        }
    }
    void commit_residency() {
        if (resident_dirty) {
            [resident commit];
            resident_dirty = false;
            ++res_commits;
        }
    }
    bool read_conflict(const Slot *x) const { return x && x->wgen == gen; }
    bool write_conflict(const Slot *x) const { return x && (x->wgen == gen || x->rgen == gen); }
    void mark_written(Slot *x) {
        if (x)
            x->wgen = gen;
    }
    void mark_read(Slot *x) {
        if (x)
            x->rgen = gen;
    }
    void mark_written_key(const void *k) { mark_written(find(k)); }
    void barrier_done() {
        ++gen;
        ++barriers;
    }
    size_t held() const { return count; }
};

// A scoped timer for the sampled frames (one clock read pair, only when sampling).
struct D9M4Timer {
    D9M4State *st;
    int kind;
    uint64_t t0;
    D9M4Timer(D9M4State *s, int k) : st(s), kind(k), t0(s && s->sampling ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0) {}
    ~D9M4Timer() {
        if (t0)
            st->t_ns[kind] += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    }
};

// ---- the Metal 4 context: one per renderer --------------------------------------------------------------------------
API_AVAILABLE(macos(26.0), ios(26.0))
@interface D9M4 : NSObject
@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) id<MTL4CommandQueue> queue;
@property(nonatomic, readonly) id<MTL4Compiler> compiler;          // MetalFX 4 scalers, flexible pipelines
@property(nonatomic, readonly) id<MTLSharedEvent> done;            // signalled with each command buffer's value
@property(nonatomic, readonly) id<MTLSharedEvent> presented;       // the presenter's copies (classic queue)
@property(nonatomic, readonly) id<MTL4ArgumentTable> vtab, ftab, ctab;
@property(nonatomic, readonly) D9M4State *st;
// Nil (with the reason in *why) when the device or the OS has no Metal 4.
+ (instancetype)contextWithDevice:(id<MTLDevice>)device why:(NSString **)why;
- (uint64_t)nextValue;
- (id<MTLBuffer>)takeChunk:(NSUInteger)atLeast;
- (void)giveChunk:(id<MTLBuffer>)chunk;
- (id<MTL4CommandAllocator>)takeAllocator;
- (void)giveAllocator:(id<MTL4CommandAllocator>)a;
// The presenter's classic command buffer copied the frame: the next Metal 4 work waits for it.
- (uint64_t)nextPresentedValue;
- (void)waitPresented:(uint64_t)value;
// An object (a MetalFX scaler being replaced) the GPU may still use: kept until the work in flight is done.
- (void)keepAlive:(id)object;
- (uint64_t)takeUploadedBytes;   // since the last call (the perf line)
- (uint64_t)takeBarriers;
- (void)beforeCommit;   // render thread, each commit: the residency set's changes
- (void)afterCommit;    // ... and the sweep of what is unused and done
@end

@interface D9RenderEnc : NSObject
@property(nonatomic, readonly) id<MTLRenderCommandEncoder> raw;
- (instancetype)initWithClassic:(id<MTLRenderCommandEncoder>)enc;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p;
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s;
- (void)setStencilReferenceValue:(uint32_t)v;
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp;
- (void)setCullMode:(MTLCullMode)m;
- (void)setTriangleFillMode:(MTLTriangleFillMode)m;
- (void)setFrontFacingWinding:(MTLWinding)w;
- (void)setViewport:(MTLViewport)v;
- (void)setScissorRect:(MTLScissorRect)r;
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a;
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset;
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index;
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index;
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index;
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index;
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count;
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)itype
                  indexBuffer:(id<MTLBuffer>)ib
            indexBufferOffset:(NSUInteger)offset;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)itype
                  indexBuffer:(id<MTLBuffer>)ib
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance;
- (void)endEncoding;
@end

@interface D9BlitEnc : NSObject
- (instancetype)initWithClassic:(id<MTLBlitCommandEncoder>)enc;
- (void)copyFromTexture:(id<MTLTexture>)src
                 sourceSlice:(NSUInteger)slice
                 sourceLevel:(NSUInteger)level
                sourceOrigin:(MTLOrigin)origin
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)dst
           destinationOffset:(NSUInteger)offset
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image;
- (void)copyFromTexture:(id<MTLTexture>)src
             sourceSlice:(NSUInteger)slice
             sourceLevel:(NSUInteger)level
            sourceOrigin:(MTLOrigin)origin
              sourceSize:(MTLSize)size
               toTexture:(id<MTLTexture>)dst
        destinationSlice:(NSUInteger)dslice
        destinationLevel:(NSUInteger)dlevel
       destinationOrigin:(MTLOrigin)dorigin;
- (void)copyFromBuffer:(id<MTLBuffer>)src
           sourceOffset:(NSUInteger)offset
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)dslice
       destinationLevel:(NSUInteger)dlevel
      destinationOrigin:(MTLOrigin)dorigin;
- (void)endEncoding;
@end

@interface D9ComputeEnc : NSObject
- (instancetype)initWithClassic:(id<MTLComputeCommandEncoder>)enc;
- (void)setComputePipelineState:(id<MTLComputePipelineState>)p;
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index;
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads;
- (void)endEncoding;
@end

@interface D9Cmd : NSObject
@property(nonatomic, readonly) id<MTLCommandBuffer> raw;   // the classic buffer (MetalFX), nil on Metal 4
@property(nonatomic, readonly) id m4;                      // id<MTL4CommandBuffer> on Metal 4 (MetalFX 4), else nil
@property(nonatomic, readonly) uint64_t value;             // Metal 4: the shared event's value at completion (set at commit)
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue;
- (instancetype)initWithM4:(D9M4 *)m4 API_AVAILABLE(macos(26.0), ios(26.0));
- (D9RenderEnc *)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)rp;
- (D9BlitEnc *)blitCommandEncoder;
- (D9ComputeEnc *)computeCommandEncoder;
- (void)addDone:(D9CmdDone)done;   // before commit
- (void)commit;
- (void)waitUntilCompleted;
// Metal 4: keep `r` resident and alive until the GPU is done with it (no-op on classic Metal).
- (void)touch:(id<MTLResource>)r;
// Metal 4: `bytes` in this command buffer's upload memory; its GPU address (0 on classic Metal). `slot` >= 0: the
// same bytes as that slot's last upload in this command buffer reuse it.
- (uint64_t)upload:(const void *)bytes length:(NSUInteger)length slot:(int)slot;
// Metal 4: a barrier on all earlier passes before MetalFX's own work; `written`: what that work writes or reads.
- (void)barrierBeforeForeignWork:(std::initializer_list<id<MTLTexture>>)written;
@end

// ---- classic Metal + Metal 4 ----------------------------------------------------------------------------------------

static const MTLStages kD9AllStages = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit;

@implementation D9M4 {
    std::mutex mutex_;
    NSMutableArray *chunks_, *allocators_;
    std::vector<std::pair<id, uint64_t>> kept_;
    uint64_t value_, presented_value_;
    D9M4State st_;
}
+ (instancetype)contextWithDevice:(id<MTLDevice>)device why:(NSString **)why {
    if (@available(macOS 26.0, iOS 26.0, *)) {
        if (![device supportsFamily:MTLGPUFamilyMetal4]) {
            *why = @"this GPU is not in the Metal 4 family";
            return nil;
        }
        D9M4 *m = [D9M4 new];
        m->_device = device;
        m->_queue = [device newMTL4CommandQueue];
        NSError *error = nil;
        m->_compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
        m->_done = [device newSharedEvent];
        m->_presented = [device newSharedEvent];
        // Each table only as big as its stage's slots (a draw snapshots its tables: a smaller table is a cheaper draw):
        // vertex buffers 0-8 streams, 10 grass, 16-21 constants / params / motion; fragment textures and samplers 0-15
        // the D3D9 stages, 16-18 ours; fragment buffers 0-5; compute 2 buffers, 6 textures.
        auto table = [&](NSUInteger buffers, NSUInteger textures, NSUInteger samplers) {
            MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
            ad.maxBufferBindCount = buffers;
            ad.maxTextureBindCount = textures;
            ad.maxSamplerStateBindCount = samplers;
            ad.initializeBindings = YES;
            return [device newArgumentTableWithDescriptor:ad error:&error];
        };
        m->_vtab = table(22, 1, 1);
        m->_ftab = table(6, 19, 16);
        m->_ctab = table(2, 6, 0);
        m->chunks_ = [NSMutableArray array];
        m->allocators_ = [NSMutableArray array];
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
        rd.initialCapacity = 2048;
        m->st_.resident = [device newResidencySetWithDescriptor:rd error:&error];
        const char *b = getenv("RECOMP_METAL4_BARRIERS");
        m->st_.precise = !(b && !strcmp(b, "safe"));
        if (!m->_queue || !m->_done || !m->_presented || !m->_vtab || !m->_ftab || !m->_ctab || !m->st_.resident) {
            *why = [NSString stringWithFormat:@"setup failed (%@)", error.localizedDescription ?: @"no queue"];
            return nil;
        }
        [m->_queue addResidencySet:m->st_.resident];
        return m;
    }
    *why = @"this OS has no Metal 4";
    return nil;
}
- (D9M4State *)st {
    return &st_;
}
- (uint64_t)nextValue {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++value_;
}
- (id<MTLBuffer>)takeChunk:(NSUInteger)atLeast {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (NSUInteger i = 0; i < chunks_.count; ++i) {
            id<MTLBuffer> b = chunks_[i];
            if (b.length >= atLeast) {
                [chunks_ removeObjectAtIndex:i];
                return b;
            }
        }
    }
    return [_device newBufferWithLength:std::max<NSUInteger>(atLeast, 4u << 20)
                                options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
}
- (void)giveChunk:(id<MTLBuffer>)chunk {
    std::lock_guard<std::mutex> lock(mutex_);
    if (chunks_.count < 16)
        [chunks_ addObject:chunk];
}
- (id<MTL4CommandAllocator>)takeAllocator {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (allocators_.count) {
            id<MTL4CommandAllocator> a = allocators_.lastObject;
            [allocators_ removeLastObject];
            return a;
        }
    }
    return [_device newCommandAllocator];
}
- (void)giveAllocator:(id<MTL4CommandAllocator>)a {
    [a reset];   // the GPU is done with every command buffer that used it
    std::lock_guard<std::mutex> lock(mutex_);
    if (allocators_.count < 8)
        [allocators_ addObject:a];
}
- (uint64_t)nextPresentedValue {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++presented_value_;
}
- (void)waitPresented:(uint64_t)value {
    [_queue waitForEvent:_presented value:value];
}
- (void)keepAlive:(id)object {
    if (object)
        kept_.emplace_back(object, st_.now);
}
// Render thread, at each commit: residency changes go in, and the old kept objects go.
- (void)beforeCommit {
    st_.commit_residency();
}
- (void)afterCommit {
    const uint64_t completed = _done.signaledValue;
    st_.sweep(completed);
    if (!kept_.empty())
        kept_.erase(std::remove_if(kept_.begin(), kept_.end(),
                                   [&](const std::pair<id, uint64_t> &k) { return k.second <= completed; }),
                    kept_.end());
    ++st_.now;
}
- (uint64_t)takeUploadedBytes {
    const uint64_t b = st_.uploaded;
    st_.uploaded = 0;
    return b;
}
- (uint64_t)takeBarriers {
    const uint64_t b = st_.barriers;
    st_.barriers = 0;
    return b;
}
@end

// Argument-table entries, set only when they change (the table keeps its value across encoders and command buffers;
// a draw takes a snapshot of it).
static inline void d9_set_addr(D9M4State *st, int kind, id<MTL4ArgumentTable> t, NSUInteger i, uint64_t a)
    API_AVAILABLE(macos(26.0), ios(26.0)) {
    if (i < 32) {
        if (st->addr[kind][i] == a) {
            ++st->table_skips;
            return;
        }
        st->addr[kind][i] = a;
    }
    ++st->table_sets;
    [t setAddress:a atIndex:i];
}
static inline void d9_set_tex(D9M4State *st, int kind, id<MTL4ArgumentTable> t, NSUInteger i, id<MTLTexture> x)
    API_AVAILABLE(macos(26.0), ios(26.0)) {
    const MTLResourceID r = x ? x.gpuResourceID : MTLResourceID{0};
    uint64_t v;
    memcpy(&v, &r, sizeof v);
    if (i < 32) {
        if (st->tex[kind][i] == v) {
            ++st->table_skips;
            return;
        }
        st->tex[kind][i] = v;
    }
    ++st->table_sets;
    [t setTexture:r atIndex:i];
}
static inline void d9_set_smp(D9M4State *st, int kind, id<MTL4ArgumentTable> t, NSUInteger i, id<MTLSamplerState> x)
    API_AVAILABLE(macos(26.0), ios(26.0)) {
    const MTLResourceID r = x ? x.gpuResourceID : MTLResourceID{0};
    uint64_t v;
    memcpy(&v, &r, sizeof v);
    if (i < 16) {
        if (st->smp[kind][i] == v) {
            ++st->table_skips;
            return;
        }
        st->smp[kind][i] = v;
    }
    ++st->table_sets;
    [t setSamplerState:r atIndex:i];
}

@implementation D9RenderEnc {
    id<MTLRenderCommandEncoder> c_;
    id m_;            // id<MTL4RenderCommandEncoder>
    D9Cmd *cmd_;      // Metal 4: uploads and touches
    id<MTL4ArgumentTable> vt_, ft_;
    D9M4State *st_;
    std::vector<const void *> writes_;   // this pass's attachments (written again after a barrier inside it)
}
- (instancetype)initWithClassic:(id<MTLRenderCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (instancetype)initWithM4:(id)enc
                       cmd:(D9Cmd *)cmd
                      vtab:(id<MTL4ArgumentTable>)vt
                      ftab:(id<MTL4ArgumentTable>)ft
                     state:(D9M4State *)st
               attachments:(const std::vector<id<MTLTexture>> &)att API_AVAILABLE(macos(26.0), ios(26.0)) {
    if (!enc)
        return nil;
    if ((self = [super init])) {
        m_ = enc;
        cmd_ = cmd;
        vt_ = vt;
        ft_ = ft;
        st_ = st;
        id<MTL4RenderCommandEncoder> e = enc;
        bool need = !st->precise;
        for (id<MTLTexture> t : att)
            need = need || st->write_conflict(st->use(t));
        if (need) {
            [e barrierAfterQueueStages:kD9AllStages
                          beforeStages:MTLStageVertex | MTLStageFragment
                     visibilityOptions:MTL4VisibilityOptionDevice];
            st->barrier_done();
        }
        for (id<MTLTexture> t : att) {
            writes_.push_back((__bridge const void *)t);
            st->mark_written(st->use(t));
        }
        [e setArgumentTable:vt atStages:MTLRenderStageVertex];
        [e setArgumentTable:ft atStages:MTLRenderStageFragment];
    }
    return self;
}
- (id<MTLRenderCommandEncoder>)raw {
    return c_;
}
// A resource this pass reads that an earlier pass wrote since the last barrier: a barrier before the next draw.
- (void)m4Read:(id)r texture:(bool)texture API_AVAILABLE(macos(26.0), ios(26.0)) {
    D9M4State::Slot *x = st_->use(r);
    if (!x)
        return;
    if (st_->precise && st_->read_conflict(x)) {
        [(id<MTL4RenderCommandEncoder>)m_ barrierAfterQueueStages:kD9AllStages
                                                      beforeStages:MTLStageVertex | MTLStageFragment
                                                 visibilityOptions:MTL4VisibilityOptionDevice];
        st_->barrier_done();
        for (const void *w : writes_)   // this pass still writes its attachments
            st_->mark_written_key(w);
    }
    if (texture)
        st_->mark_read(x);
}
#define D9_M4(stmt)                                                                                                    \
    if (m_) {                                                                                                          \
        if (@available(macOS 26.0, iOS 26.0, *)) {                                                                     \
            id<MTL4RenderCommandEncoder> e = m_;                                                                       \
            stmt;                                                                                                      \
        }                                                                                                              \
        return;                                                                                                        \
    }
#define D9_M4K(kind, stmt)                                                                                             \
    if (m_) {                                                                                                          \
        if (@available(macOS 26.0, iOS 26.0, *)) {                                                                     \
            D9M4Timer t_(st_, kind);                                                                                   \
            id<MTL4RenderCommandEncoder> e = m_;                                                                       \
            stmt;                                                                                                      \
        }                                                                                                              \
        return;                                                                                                        \
    }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p {
    D9_M4K(0, [e setRenderPipelineState:p]);
    [c_ setRenderPipelineState:p];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s {
    D9_M4K(0, [e setDepthStencilState:s]);
    [c_ setDepthStencilState:s];
}
- (void)setStencilReferenceValue:(uint32_t)v {
    D9_M4K(0, [e setStencilReferenceValue:v]);
    [c_ setStencilReferenceValue:v];
}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
    D9_M4K(0, [e setDepthBias:bias slopeScale:slope clamp:clamp]);
    [c_ setDepthBias:bias slopeScale:slope clamp:clamp];
}
- (void)setCullMode:(MTLCullMode)m {
    D9_M4K(0, [e setCullMode:m]);
    [c_ setCullMode:m];
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)m {
    D9_M4K(0, [e setTriangleFillMode:m]);
    [c_ setTriangleFillMode:m];
}
- (void)setFrontFacingWinding:(MTLWinding)w {
    D9_M4K(0, [e setFrontFacingWinding:w]);
    [c_ setFrontFacingWinding:w];
}
- (void)setViewport:(MTLViewport)v {
    D9_M4K(0, [e setViewport:v]);
    [c_ setViewport:v];
}
- (void)setScissorRect:(MTLScissorRect)r {
    D9_M4K(0, [e setScissorRect:r]);
    [c_ setScissorRect:r];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
    D9_M4K(0, [e setBlendColorRed:r green:g blue:b alpha:a]);
    [c_ setBlendColorRed:r green:g blue:b alpha:a];
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
    D9_M4K(0, [e setVisibilityResultMode:mode offset:offset]);
    [c_ setVisibilityResultMode:mode offset:offset];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    D9_M4K(1, (void)e; d9_set_addr(st_, 0, vt_, index, [cmd_ upload:bytes length:length slot:(int)index]));
    [c_ setVertexBytes:bytes length:length atIndex:index];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    D9_M4K(1, (void)e; d9_set_addr(st_, 1, ft_, index, [cmd_ upload:bytes length:length slot:32 + (int)index]));
    [c_ setFragmentBytes:bytes length:length atIndex:index];
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    D9_M4K(0, (void)e; [self m4Read:b texture:false]; d9_set_addr(st_, 0, vt_, index, b ? b.gpuAddress + offset : 0));
    [c_ setVertexBuffer:b offset:offset atIndex:index];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    D9_M4K(0, (void)e; [self m4Read:b texture:false]; d9_set_addr(st_, 1, ft_, index, b ? b.gpuAddress + offset : 0));
    [c_ setFragmentBuffer:b offset:offset atIndex:index];
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    D9_M4K(0, (void)e; [self m4Read:t texture:true]; d9_set_tex(st_, 0, vt_, index, t));
    [c_ setVertexTexture:t atIndex:index];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    D9_M4K(0, (void)e; [self m4Read:t texture:true]; d9_set_tex(st_, 1, ft_, index, t));
    [c_ setFragmentTexture:t atIndex:index];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index {
    D9_M4K(0, (void)e; d9_set_smp(st_, 0, vt_, index, s));
    [c_ setVertexSamplerState:s atIndex:index];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index {
    D9_M4K(0, (void)e; d9_set_smp(st_, 1, ft_, index, s));
    [c_ setFragmentSamplerState:s atIndex:index];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {
    D9_M4K(2, [e drawPrimitives:type vertexStart:start vertexCount:count]);
    [c_ drawPrimitives:type vertexStart:start vertexCount:count];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances {
    D9_M4K(2, [e drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances]);
    [c_ drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)itype
                  indexBuffer:(id<MTLBuffer>)ib
            indexBufferOffset:(NSUInteger)offset {
    D9_M4K(2, [self m4Read:ib texture:false]; [e drawIndexedPrimitives:type
                                                         indexCount:count
                                                          indexType:itype
                                                        indexBuffer:ib.gpuAddress + offset
                                                  indexBufferLength:ib.length - offset]);
    [c_ drawIndexedPrimitives:type indexCount:count indexType:itype indexBuffer:ib indexBufferOffset:offset];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)itype
                  indexBuffer:(id<MTLBuffer>)ib
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance {
    D9_M4K(2, [self m4Read:ib texture:false]; [e drawIndexedPrimitives:type
                                                         indexCount:count
                                                          indexType:itype
                                                        indexBuffer:ib.gpuAddress + offset
                                                  indexBufferLength:ib.length - offset
                                                      instanceCount:instances
                                                         baseVertex:baseVertex
                                                       baseInstance:baseInstance]);
    [c_ drawIndexedPrimitives:type
                   indexCount:count
                    indexType:itype
                  indexBuffer:ib
            indexBufferOffset:offset
                instanceCount:instances
                   baseVertex:baseVertex
                 baseInstance:baseInstance];
}
- (void)endEncoding {
    D9_M4K(3, [e endEncoding]);
    [c_ endEncoding];
}
#undef D9_M4
#undef D9_M4K
@end

// A blit on Metal 4 is a compute encoder (MTL4ComputeCommandEncoder does both).
@implementation D9BlitEnc {
    id<MTLBlitCommandEncoder> c_;
    id m_;   // id<MTL4ComputeCommandEncoder>
    D9Cmd *cmd_;
    D9M4State *st_;
}
- (instancetype)initWithClassic:(id<MTLBlitCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (instancetype)initWithM4:(id)enc cmd:(D9Cmd *)cmd state:(D9M4State *)st API_AVAILABLE(macos(26.0), ios(26.0)) {
    if (!enc)
        return nil;
    if ((self = [super init])) {
        m_ = enc;
        cmd_ = cmd;
        st_ = st;
        if (!st->precise) {
            [(id<MTL4ComputeCommandEncoder>)enc barrierAfterQueueStages:kD9AllStages
                                                            beforeStages:MTLStageBlit
                                                       visibilityOptions:MTL4VisibilityOptionDevice];
            st->barrier_done();
        }
    }
    return self;
}
// A copy reads `src` and writes `dst`: a barrier first if an earlier pass wrote src, or read or wrote dst.
- (void)m4Copy:(id)src to:(id)dst API_AVAILABLE(macos(26.0), ios(26.0)) {
    st_->use(src);
    st_->use(dst);
    D9M4State::Slot *s = src ? st_->find((__bridge const void *)src) : nullptr;   // after both uses (growth)
    D9M4State::Slot *d = dst ? st_->find((__bridge const void *)dst) : nullptr;
    if (st_->precise && (st_->read_conflict(s) || st_->write_conflict(d))) {
        [(id<MTL4ComputeCommandEncoder>)m_ barrierAfterQueueStages:kD9AllStages
                                                       beforeStages:MTLStageBlit
                                                  visibilityOptions:MTL4VisibilityOptionDevice];
        st_->barrier_done();
    }
    st_->mark_read(s);
    st_->mark_written(d);
}
#define D9_M4(stmt)                                                                                                    \
    if (m_) {                                                                                                          \
        if (@available(macOS 26.0, iOS 26.0, *)) {                                                                     \
            id<MTL4ComputeCommandEncoder> e = m_;                                                                      \
            stmt;                                                                                                      \
        }                                                                                                              \
        return;                                                                                                        \
    }
- (void)copyFromTexture:(id<MTLTexture>)src
                 sourceSlice:(NSUInteger)slice
                 sourceLevel:(NSUInteger)level
                sourceOrigin:(MTLOrigin)origin
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)dst
           destinationOffset:(NSUInteger)offset
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image {
    D9_M4([self m4Copy:src to:dst]; [e copyFromTexture:src
                                            sourceSlice:slice
                                            sourceLevel:level
                                           sourceOrigin:origin
                                             sourceSize:size
                                               toBuffer:dst
                                      destinationOffset:offset
                                 destinationBytesPerRow:row
                               destinationBytesPerImage:image]);
    [c_ copyFromTexture:src
                     sourceSlice:slice
                     sourceLevel:level
                    sourceOrigin:origin
                      sourceSize:size
                        toBuffer:dst
               destinationOffset:offset
          destinationBytesPerRow:row
        destinationBytesPerImage:image];
}
- (void)copyFromTexture:(id<MTLTexture>)src
             sourceSlice:(NSUInteger)slice
             sourceLevel:(NSUInteger)level
            sourceOrigin:(MTLOrigin)origin
              sourceSize:(MTLSize)size
               toTexture:(id<MTLTexture>)dst
        destinationSlice:(NSUInteger)dslice
        destinationLevel:(NSUInteger)dlevel
       destinationOrigin:(MTLOrigin)dorigin {
    D9_M4([self m4Copy:src to:dst]; [e copyFromTexture:src
                                            sourceSlice:slice
                                            sourceLevel:level
                                           sourceOrigin:origin
                                             sourceSize:size
                                              toTexture:dst
                                       destinationSlice:dslice
                                       destinationLevel:dlevel
                                      destinationOrigin:dorigin]);
    [c_ copyFromTexture:src
            sourceSlice:slice
            sourceLevel:level
           sourceOrigin:origin
             sourceSize:size
              toTexture:dst
       destinationSlice:dslice
       destinationLevel:dlevel
      destinationOrigin:dorigin];
}
- (void)copyFromBuffer:(id<MTLBuffer>)src
           sourceOffset:(NSUInteger)offset
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)dslice
       destinationLevel:(NSUInteger)dlevel
      destinationOrigin:(MTLOrigin)dorigin {
    D9_M4([self m4Copy:src to:dst]; [e copyFromBuffer:src
                                          sourceOffset:offset
                                     sourceBytesPerRow:row
                                   sourceBytesPerImage:image
                                            sourceSize:size
                                             toTexture:dst
                                      destinationSlice:dslice
                                      destinationLevel:dlevel
                                     destinationOrigin:dorigin]);
    [c_ copyFromBuffer:src
               sourceOffset:offset
          sourceBytesPerRow:row
        sourceBytesPerImage:image
                 sourceSize:size
                  toTexture:dst
           destinationSlice:dslice
           destinationLevel:dlevel
          destinationOrigin:dorigin];
}
- (void)endEncoding {
    D9_M4([e endEncoding]);
    [c_ endEncoding];
}
#undef D9_M4
@end

@implementation D9ComputeEnc {
    id<MTLComputeCommandEncoder> c_;
    id m_;   // id<MTL4ComputeCommandEncoder>
    D9Cmd *cmd_;
    id<MTL4ArgumentTable> t_;
    D9M4State *st_;
    std::vector<const void *> writes_;   // what this dispatch's kernel may write (every bound resource)
}
- (instancetype)initWithClassic:(id<MTLComputeCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (instancetype)initWithM4:(id)enc cmd:(D9Cmd *)cmd table:(id<MTL4ArgumentTable>)t state:(D9M4State *)st
    API_AVAILABLE(macos(26.0), ios(26.0)) {
    if (!enc)
        return nil;
    if ((self = [super init])) {
        m_ = enc;
        cmd_ = cmd;
        t_ = t;
        st_ = st;
        id<MTL4ComputeCommandEncoder> e = enc;
        if (!st->precise) {
            [e barrierAfterQueueStages:kD9AllStages beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            st->barrier_done();
        }
        [e setArgumentTable:t];
    }
    return self;
}
// A kernel's resource: read and maybe written (a kernel says neither), so both conflicts count.
- (void)m4Bind:(id)r API_AVAILABLE(macos(26.0), ios(26.0)) {
    D9M4State::Slot *x = st_->use(r);
    if (!x)
        return;
    if (st_->precise && st_->write_conflict(x)) {
        [(id<MTL4ComputeCommandEncoder>)m_ barrierAfterQueueStages:kD9AllStages
                                                       beforeStages:MTLStageDispatch
                                                  visibilityOptions:MTL4VisibilityOptionDevice];
        st_->barrier_done();
        for (const void *w : writes_)
            st_->mark_written_key(w);
        x = st_->find((__bridge const void *)r);
    }
    writes_.push_back((__bridge const void *)r);
    st_->mark_written(x);
}
#define D9_M4(stmt)                                                                                                    \
    if (m_) {                                                                                                          \
        if (@available(macOS 26.0, iOS 26.0, *)) {                                                                     \
            id<MTL4ComputeCommandEncoder> e = m_;                                                                      \
            stmt;                                                                                                      \
        }                                                                                                              \
        return;                                                                                                        \
    }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)p {
    D9_M4([e setComputePipelineState:p]);
    [c_ setComputePipelineState:p];
}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    D9_M4((void)e; [self m4Bind:t]; d9_set_tex(st_, 2, t_, index, t));
    [c_ setTexture:t atIndex:index];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    D9_M4((void)e; [self m4Bind:b]; d9_set_addr(st_, 2, t_, index, b ? b.gpuAddress + offset : 0));
    [c_ setBuffer:b offset:offset atIndex:index];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    D9_M4((void)e; d9_set_addr(st_, 2, t_, index, [cmd_ upload:bytes length:length slot:64 + (int)index]));
    [c_ setBytes:bytes length:length atIndex:index];
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads {
    D9_M4([e dispatchThreadgroups:groups threadsPerThreadgroup:threads]);
    [c_ dispatchThreadgroups:groups threadsPerThreadgroup:threads];
}
- (void)endEncoding {
    D9_M4([e endEncoding]);
    [c_ endEncoding];
}
#undef D9_M4
@end

@implementation D9Cmd {
    id<MTLCommandBuffer> c_;
    // Metal 4
    D9M4 *ctx_;
    D9M4State *st_;
    id buf_;                     // id<MTL4CommandBuffer>
    id alloc_;                   // id<MTL4CommandAllocator>
    NSMutableArray *chunks_;     // upload memory, until it completes
    id<MTLBuffer> chunk_;
    NSUInteger used_;
    NSMutableArray<D9CmdDone> *done_;
    uint64_t value_;
    // the last upload per slot (vertex 0-31, fragment 32-63, compute 64-95) in this command buffer
    struct LastUpload {
        const uint8_t *at = nullptr;
        NSUInteger length = 0;
        uint64_t address = 0;
    } last_[96];
}
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (!cb)
        return nil;
    if ((self = [super init]))
        c_ = cb;
    return self;
}
- (instancetype)initWithM4:(D9M4 *)m4 {
    if (!(self = [super init]))
        return nil;
    ctx_ = m4;
    st_ = m4.st;
    alloc_ = [m4 takeAllocator];
    id<MTL4CommandBuffer> cb = [m4.device newCommandBuffer];
    if (!cb || !alloc_)
        return nil;
    [cb beginCommandBufferWithAllocator:alloc_];
    buf_ = cb;
    chunks_ = [NSMutableArray array];
    done_ = [NSMutableArray array];
    return self;
}
- (id<MTLCommandBuffer>)raw {
    return c_;
}
- (id)m4 {
    return buf_;
}
- (uint64_t)value {
    return value_;
}
- (void)touch:(id<MTLResource>)r {
    if (buf_)
        st_->use(r);
}
- (uint64_t)upload:(const void *)bytes length:(NSUInteger)length slot:(int)slot {
    if (!buf_)
        return 0;
    // The same bytes again: only for small blocks (parameter blocks); comparing the 4 KB constant arrays cost more
    // than copying them (~20 MB compared a frame on the owner's phone).
    if (length > 256)
        slot = -1;
    if (slot >= 0 && slot < 96) {
        const LastUpload &l = last_[slot];
        if (l.at && l.length == length && !memcmp(l.at, bytes, length)) {
            ++st_->reused;
            return l.address;
        }
    }
    const NSUInteger n = (length + 255) & ~(NSUInteger)255;   // constant buffer offsets: 256-byte aligned (safe)
    if (!chunk_ || used_ + n > chunk_.length) {
        chunk_ = [ctx_ takeChunk:n];
        used_ = 0;
        [chunks_ addObject:chunk_];
        st_->use(chunk_);
    }
    uint8_t *at = (uint8_t *)chunk_.contents + used_;
    memcpy(at, bytes, length);
    st_->uploaded += length;
    const uint64_t a = chunk_.gpuAddress + used_;
    used_ += n;
    if (slot >= 0 && slot < 96)
        last_[slot] = {at, length, a};
    return a;
}
- (D9RenderEnc *)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)rp {
    if (!buf_)
        return [[D9RenderEnc alloc] initWithClassic:[c_ renderCommandEncoderWithDescriptor:rp]];
    if (@available(macOS 26.0, iOS 26.0, *)) {
        D9M4Timer t_(st_, 3);
        MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
        std::vector<id<MTLTexture>> att;
        for (NSUInteger i = 0; i < 8; ++i) {
            MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
            if (!a.texture)
                continue;
            d.colorAttachments[i] = [a copy];
            st_->use(a.texture);
            att.push_back(a.texture);
            if (a.resolveTexture) {
                st_->use(a.resolveTexture);
                att.push_back(a.resolveTexture);
            }
        }
        if (rp.depthAttachment.texture) {
            d.depthAttachment = [rp.depthAttachment copy];
            st_->use(rp.depthAttachment.texture);
            att.push_back(rp.depthAttachment.texture);
        }
        if (rp.stencilAttachment.texture) {
            d.stencilAttachment = [rp.stencilAttachment copy];
            st_->use(rp.stencilAttachment.texture);
        }
        if (rp.visibilityResultBuffer) {
            d.visibilityResultBuffer = rp.visibilityResultBuffer;
            st_->use(rp.visibilityResultBuffer);
        }
        if (rp.renderTargetWidth)
            d.renderTargetWidth = rp.renderTargetWidth;
        if (rp.renderTargetHeight)
            d.renderTargetHeight = rp.renderTargetHeight;
        id<MTL4RenderCommandEncoder> e = [(id<MTL4CommandBuffer>)buf_ renderCommandEncoderWithDescriptor:d];
        return [[D9RenderEnc alloc] initWithM4:e cmd:self vtab:ctx_.vtab ftab:ctx_.ftab state:st_ attachments:att];
    }
    return nil;
}
- (D9BlitEnc *)blitCommandEncoder {
    if (!buf_)
        return [[D9BlitEnc alloc] initWithClassic:[c_ blitCommandEncoder]];
    if (@available(macOS 26.0, iOS 26.0, *))
        return [[D9BlitEnc alloc] initWithM4:[(id<MTL4CommandBuffer>)buf_ computeCommandEncoder] cmd:self state:st_];
    return nil;
}
- (D9ComputeEnc *)computeCommandEncoder {
    if (!buf_)
        return [[D9ComputeEnc alloc] initWithClassic:[c_ computeCommandEncoder]];
    if (@available(macOS 26.0, iOS 26.0, *))
        return [[D9ComputeEnc alloc] initWithM4:[(id<MTL4CommandBuffer>)buf_ computeCommandEncoder]
                                            cmd:self
                                          table:ctx_.ctab
                                          state:st_];
    return nil;
}
- (void)barrierBeforeForeignWork:(std::initializer_list<id<MTLTexture>>)written {
    if (!buf_)
        return;
    if (@available(macOS 26.0, iOS 26.0, *)) {
        id<MTL4ComputeCommandEncoder> e = [(id<MTL4CommandBuffer>)buf_ computeCommandEncoder];
        [e barrierAfterQueueStages:kD9AllStages beforeStages:kD9AllStages visibilityOptions:MTL4VisibilityOptionDevice];
        [e endEncoding];
        st_->barrier_done();
        for (id<MTLTexture> t : written)   // MetalFX reads and writes these: the next pass that uses one waits
            st_->mark_written(st_->use(t));
    }
}
- (void)addDone:(D9CmdDone)done {
    if (!buf_) {
        [c_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
          done(cb.error, cb.GPUEndTime > cb.GPUStartTime ? cb.GPUEndTime - cb.GPUStartTime : 0.0);
        }];
        return;
    }
    [done_ addObject:done];
}
- (void)commit {
    if (!buf_) {
        [c_ commit];
        return;
    }
    if (@available(macOS 26.0, iOS 26.0, *)) {
        D9M4Timer t_(st_, 4);
        id<MTL4CommandBuffer> cb = buf_;
        [cb endCommandBuffer];
        [ctx_ beforeCommit];   // the queue's residency set (new resources since the last commit)
        // The feedback handler runs once the GPU finished: it reports, then gives the memory back.
        D9M4 *ctx = ctx_;
        NSArray<D9CmdDone> *done = [done_ copy];
        NSArray *chunks = [chunks_ copy];
        id alloc = alloc_;
        MTL4CommitOptions *opts = [MTL4CommitOptions new];
        [opts addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
          const double gpu = fb.GPUEndTime > fb.GPUStartTime ? fb.GPUEndTime - fb.GPUStartTime : 0.0;
          for (D9CmdDone d in done)
              d(fb.error, gpu);
          for (id<MTLBuffer> b in chunks)
              [ctx giveChunk:b];
          [ctx giveAllocator:alloc];
        }];
        id<MTL4CommandBuffer> list[1] = {cb};
        // The value at commit, not at creation: a command buffer made and committed while another is open (the
        // renderer's one-off ones) would otherwise signal out of order, the event would go back, and a wait hang.
        value_ = [ctx_ nextValue];
        st_->now = value_;
        [ctx_.queue commit:list count:1 options:opts];
        [ctx_.queue signalEvent:ctx_.done value:value_];
        [ctx_ afterCommit];
        chunks_ = nil;
        chunk_ = nil;
        alloc_ = nil;
        done_ = nil;
    }
}
- (void)waitUntilCompleted {
    if (!buf_) {
        [c_ waitUntilCompleted];
        return;
    }
    [ctx_.done waitUntilSignaledValue:value_ timeoutMS:20000];
}
@end
