// d3d9_encoders.h - the command buffer and encoders the Direct3D 9 renderer (d3d9_metal.mm) encodes into: one set
// of objects that answer the same messages as Metal's own (setVertexBytes:length:atIndex:, drawPrimitives:..., and
// so on), so the renderer's call sites read as before, while the objects decide which Metal does the work
// (docs/METAL4_PLAN.md).
//
// Classic Metal: each object forwards to the MTLCommandBuffer / encoder it wraps.
// Metal 4 (D9M4, RECOMP_METAL4=1 on a device of the Metal 4 family): command buffers come from the device and go to
// an MTL4CommandQueue; encoders bind through MTL4ArgumentTables (one per stage); setVertexBytes and the like copy into
// the command buffer's upload chunks and bind their GPU address; every resource an encoder touches is held by the
// command buffer until the GPU is done with it (Metal 4 command buffers retain nothing) and put in its residency set;
// each pass starts with a consumer barrier on all earlier work (Metal 4 tracks no hazards; the classic ordering).
// Completion: the queue signals the shared event with the command buffer's value; its feedback handler reports.
#pragma once
#import <Metal/Metal.h>
#include <mutex>
#include <vector>

// Called once when the command buffer finished on the GPU: `error` nil on success, `gpu_s` the GPU time (0 unknown).
typedef void (^D9CmdDone)(NSError *error, double gpu_s);

@class D9Cmd;

// ---- the Metal 4 context: one per renderer --------------------------------------------------------------------------
API_AVAILABLE(macos(26.0), ios(26.0))
@interface D9M4 : NSObject
@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) id<MTL4CommandQueue> queue;
@property(nonatomic, readonly) id<MTL4Compiler> compiler;          // MetalFX 4 scalers
@property(nonatomic, readonly) id<MTLSharedEvent> done;            // signalled with each command buffer's value
@property(nonatomic, readonly) id<MTLSharedEvent> presented;       // the presenter's copies (classic queue)
@property(nonatomic, readonly) id<MTL4ArgumentTable> vtab, ftab, ctab;
// Nil (with the reason in *why) when the device or the OS has no Metal 4.
+ (instancetype)contextWithDevice:(id<MTLDevice>)device why:(NSString **)why;
- (uint64_t)nextValue;
// Upload chunks and allocators, recycled once the GPU passed the value they were last used with.
- (id<MTLBuffer>)takeChunk:(NSUInteger)atLeast;
- (void)giveChunk:(id<MTLBuffer>)chunk;
- (id<MTL4CommandAllocator>)takeAllocator;
- (void)giveAllocator:(id<MTL4CommandAllocator>)a;
- (id<MTLResidencySet>)takeResidency;
- (void)giveResidency:(id<MTLResidencySet>)r;
// The presenter's classic command buffer copied the frame: the next Metal 4 work waits for it.
- (uint64_t)nextPresentedValue;
- (void)waitPresented:(uint64_t)value;
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
// Metal 4: keep `r` alive and resident until this command buffer completes (no-op on classic Metal).
- (void)touch:(id<MTLResource>)r;
// Metal 4: `bytes` copied into this command buffer's upload memory; its GPU address (0 on classic Metal).
- (uint64_t)upload:(const void *)bytes length:(NSUInteger)length;
// Metal 4: a consumer barrier on all earlier passes before an encoder the renderer makes itself (MetalFX).
- (void)barrierBeforeForeignWork;
@end

// ---- classic Metal + Metal 4 ----------------------------------------------------------------------------------------

static const MTLStages kD9AllStages = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit;

@implementation D9M4 {
    std::mutex mutex_;
    NSMutableArray *chunks_, *allocators_, *residency_;
    uint64_t value_, presented_value_;
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
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
        ad.maxBufferBindCount = 31;
        ad.maxTextureBindCount = 31;
        ad.maxSamplerStateBindCount = 16;
        ad.initializeBindings = YES;
        m->_vtab = [device newArgumentTableWithDescriptor:ad error:&error];
        m->_ftab = [device newArgumentTableWithDescriptor:ad error:&error];
        m->_ctab = [device newArgumentTableWithDescriptor:ad error:&error];
        m->chunks_ = [NSMutableArray array];
        m->allocators_ = [NSMutableArray array];
        m->residency_ = [NSMutableArray array];
        if (!m->_queue || !m->_done || !m->_presented || !m->_vtab || !m->_ftab || !m->_ctab) {
            *why = [NSString stringWithFormat:@"setup failed (%@)", error.localizedDescription ?: @"no queue"];
            return nil;
        }
        return m;
    }
    *why = @"this OS has no Metal 4";
    return nil;
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
- (id<MTLResidencySet>)takeResidency {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (residency_.count) {
            id<MTLResidencySet> r = residency_.lastObject;
            [residency_ removeLastObject];
            return r;
        }
    }
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
    rd.initialCapacity = 512;
    return [_device newResidencySetWithDescriptor:rd error:nil];
}
- (void)giveResidency:(id<MTLResidencySet>)r {
    [r removeAllAllocations];
    [r commit];
    std::lock_guard<std::mutex> lock(mutex_);
    if (residency_.count < 8)
        [residency_ addObject:r];
}
- (uint64_t)nextPresentedValue {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++presented_value_;
}
- (void)waitPresented:(uint64_t)value {
    [_queue waitForEvent:_presented value:value];
}
@end

@implementation D9RenderEnc {
    id<MTLRenderCommandEncoder> c_;
    id m_;            // id<MTL4RenderCommandEncoder>
    D9Cmd *cmd_;      // Metal 4: uploads and touches
    id<MTL4ArgumentTable> vt_, ft_;
}
- (instancetype)initWithClassic:(id<MTLRenderCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (instancetype)initWithM4:(id)enc cmd:(D9Cmd *)cmd vtab:(id<MTL4ArgumentTable>)vt ftab:(id<MTL4ArgumentTable>)ft
    API_AVAILABLE(macos(26.0), ios(26.0)) {
    if (!enc)
        return nil;
    if ((self = [super init])) {
        m_ = enc;
        cmd_ = cmd;
        vt_ = vt;
        ft_ = ft;
        id<MTL4RenderCommandEncoder> e = enc;
        [e barrierAfterQueueStages:kD9AllStages
                      beforeStages:MTLStageVertex | MTLStageFragment
                 visibilityOptions:MTL4VisibilityOptionDevice];
        [e setArgumentTable:vt atStages:MTLRenderStageVertex];
        [e setArgumentTable:ft atStages:MTLRenderStageFragment];
    }
    return self;
}
- (id<MTLRenderCommandEncoder>)raw {
    return c_;
}
#define D9_M4(stmt)                                                                                                    \
    if (m_) {                                                                                                          \
        if (@available(macOS 26.0, iOS 26.0, *)) {                                                                     \
            id<MTL4RenderCommandEncoder> e = m_;                                                                       \
            stmt;                                                                                                      \
        }                                                                                                              \
        return;                                                                                                        \
    }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p {
    D9_M4([e setRenderPipelineState:p]);
    [c_ setRenderPipelineState:p];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s {
    D9_M4([e setDepthStencilState:s]);
    [c_ setDepthStencilState:s];
}
- (void)setStencilReferenceValue:(uint32_t)v {
    D9_M4([e setStencilReferenceValue:v]);
    [c_ setStencilReferenceValue:v];
}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
    D9_M4([e setDepthBias:bias slopeScale:slope clamp:clamp]);
    [c_ setDepthBias:bias slopeScale:slope clamp:clamp];
}
- (void)setCullMode:(MTLCullMode)m {
    D9_M4([e setCullMode:m]);
    [c_ setCullMode:m];
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)m {
    D9_M4([e setTriangleFillMode:m]);
    [c_ setTriangleFillMode:m];
}
- (void)setFrontFacingWinding:(MTLWinding)w {
    D9_M4([e setFrontFacingWinding:w]);
    [c_ setFrontFacingWinding:w];
}
- (void)setViewport:(MTLViewport)v {
    D9_M4([e setViewport:v]);
    [c_ setViewport:v];
}
- (void)setScissorRect:(MTLScissorRect)r {
    D9_M4([e setScissorRect:r]);
    [c_ setScissorRect:r];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
    D9_M4([e setBlendColorRed:r green:g blue:b alpha:a]);
    [c_ setBlendColorRed:r green:g blue:b alpha:a];
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
    D9_M4([e setVisibilityResultMode:mode offset:offset]);
    [c_ setVisibilityResultMode:mode offset:offset];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    D9_M4((void)e; [vt_ setAddress:[cmd_ upload:bytes length:length] atIndex:index]);
    [c_ setVertexBytes:bytes length:length atIndex:index];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    D9_M4((void)e; [ft_ setAddress:[cmd_ upload:bytes length:length] atIndex:index]);
    [c_ setFragmentBytes:bytes length:length atIndex:index];
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    D9_M4((void)e; [cmd_ touch:b]; [vt_ setAddress:b ? b.gpuAddress + offset : 0 atIndex:index]);
    [c_ setVertexBuffer:b offset:offset atIndex:index];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    D9_M4((void)e; [cmd_ touch:b]; [ft_ setAddress:b ? b.gpuAddress + offset : 0 atIndex:index]);
    [c_ setFragmentBuffer:b offset:offset atIndex:index];
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    D9_M4((void)e; [cmd_ touch:t]; [vt_ setTexture:t ? t.gpuResourceID : MTLResourceID{0} atIndex:index]);
    [c_ setVertexTexture:t atIndex:index];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    D9_M4((void)e; [cmd_ touch:t]; [ft_ setTexture:t ? t.gpuResourceID : MTLResourceID{0} atIndex:index]);
    [c_ setFragmentTexture:t atIndex:index];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index {
    D9_M4((void)e; [vt_ setSamplerState:s ? s.gpuResourceID : MTLResourceID{0} atIndex:index]);
    [c_ setVertexSamplerState:s atIndex:index];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index {
    D9_M4((void)e; [ft_ setSamplerState:s ? s.gpuResourceID : MTLResourceID{0} atIndex:index]);
    [c_ setFragmentSamplerState:s atIndex:index];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {
    D9_M4([e drawPrimitives:type vertexStart:start vertexCount:count]);
    [c_ drawPrimitives:type vertexStart:start vertexCount:count];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances {
    D9_M4([e drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances]);
    [c_ drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)itype
                  indexBuffer:(id<MTLBuffer>)ib
            indexBufferOffset:(NSUInteger)offset {
    D9_M4([cmd_ touch:ib]; [e drawIndexedPrimitives:type
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
    D9_M4([cmd_ touch:ib]; [e drawIndexedPrimitives:type
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
    D9_M4([e endEncoding]);
    [c_ endEncoding];
}
#undef D9_M4
@end

// A blit on Metal 4 is a compute encoder (MTL4ComputeCommandEncoder does both).
@implementation D9BlitEnc {
    id<MTLBlitCommandEncoder> c_;
    id m_;   // id<MTL4ComputeCommandEncoder>
    D9Cmd *cmd_;
}
- (instancetype)initWithClassic:(id<MTLBlitCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (instancetype)initWithM4:(id)enc cmd:(D9Cmd *)cmd API_AVAILABLE(macos(26.0), ios(26.0)) {
    if (!enc)
        return nil;
    if ((self = [super init])) {
        m_ = enc;
        cmd_ = cmd;
        [(id<MTL4ComputeCommandEncoder>)enc barrierAfterQueueStages:kD9AllStages
                                                        beforeStages:MTLStageBlit
                                                   visibilityOptions:MTL4VisibilityOptionDevice];
    }
    return self;
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
    D9_M4([cmd_ touch:src]; [cmd_ touch:dst]; [e copyFromTexture:src
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
    D9_M4([cmd_ touch:src]; [cmd_ touch:dst]; [e copyFromTexture:src
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
    D9_M4([cmd_ touch:src]; [cmd_ touch:dst]; [e copyFromBuffer:src
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
}
- (instancetype)initWithClassic:(id<MTLComputeCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (instancetype)initWithM4:(id)enc cmd:(D9Cmd *)cmd table:(id<MTL4ArgumentTable>)t
    API_AVAILABLE(macos(26.0), ios(26.0)) {
    if (!enc)
        return nil;
    if ((self = [super init])) {
        m_ = enc;
        cmd_ = cmd;
        t_ = t;
        id<MTL4ComputeCommandEncoder> e = enc;
        [e barrierAfterQueueStages:kD9AllStages beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        [e setArgumentTable:t];
    }
    return self;
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
    D9_M4((void)e; [cmd_ touch:t]; [t_ setTexture:t ? t.gpuResourceID : MTLResourceID{0} atIndex:index]);
    [c_ setTexture:t atIndex:index];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    D9_M4((void)e; [cmd_ touch:b]; [t_ setAddress:b ? b.gpuAddress + offset : 0 atIndex:index]);
    [c_ setBuffer:b offset:offset atIndex:index];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    D9_M4((void)e; [t_ setAddress:[cmd_ upload:bytes length:length] atIndex:index]);
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
    id buf_;                     // id<MTL4CommandBuffer>
    id alloc_;                   // id<MTL4CommandAllocator>
    id<MTLResidencySet> res_;
    NSMutableSet *held_;         // every resource the GPU work touches, until it completes
    NSMutableArray *chunks_;     // upload memory, until it completes
    id<MTLBuffer> chunk_;
    NSUInteger used_;
    NSMutableArray<D9CmdDone> *done_;
    uint64_t value_;
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
    alloc_ = [m4 takeAllocator];
    id<MTL4CommandBuffer> cb = [m4.device newCommandBuffer];
    if (!cb || !alloc_)
        return nil;
    [cb beginCommandBufferWithAllocator:alloc_];
    buf_ = cb;
    res_ = [m4 takeResidency];
    held_ = [NSMutableSet setWithCapacity:256];
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
    if (buf_ && r)
        [held_ addObject:r];
}
- (uint64_t)upload:(const void *)bytes length:(NSUInteger)length {
    if (!buf_)
        return 0;
    const NSUInteger n = (length + 255) & ~(NSUInteger)255;   // constant buffer offsets: 256-byte aligned (safe)
    if (!chunk_ || used_ + n > chunk_.length) {
        chunk_ = [ctx_ takeChunk:n];
        used_ = 0;
        [chunks_ addObject:chunk_];
        [held_ addObject:chunk_];
    }
    memcpy((uint8_t *)chunk_.contents + used_, bytes, length);
    const uint64_t a = chunk_.gpuAddress + used_;
    used_ += n;
    return a;
}
- (D9RenderEnc *)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)rp {
    if (!buf_)
        return [[D9RenderEnc alloc] initWithClassic:[c_ renderCommandEncoderWithDescriptor:rp]];
    if (@available(macOS 26.0, iOS 26.0, *)) {
        MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
        for (NSUInteger i = 0; i < 8; ++i) {
            MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
            if (!a.texture)
                continue;
            d.colorAttachments[i] = [a copy];
            [self touch:a.texture];
            [self touch:a.resolveTexture];
        }
        if (rp.depthAttachment.texture) {
            d.depthAttachment = [rp.depthAttachment copy];
            [self touch:rp.depthAttachment.texture];
        }
        if (rp.stencilAttachment.texture) {
            d.stencilAttachment = [rp.stencilAttachment copy];
            [self touch:rp.stencilAttachment.texture];
        }
        if (rp.visibilityResultBuffer) {
            d.visibilityResultBuffer = rp.visibilityResultBuffer;
            [self touch:rp.visibilityResultBuffer];
        }
        if (rp.renderTargetWidth)
            d.renderTargetWidth = rp.renderTargetWidth;
        if (rp.renderTargetHeight)
            d.renderTargetHeight = rp.renderTargetHeight;
        id<MTL4RenderCommandEncoder> e = [(id<MTL4CommandBuffer>)buf_ renderCommandEncoderWithDescriptor:d];
        return [[D9RenderEnc alloc] initWithM4:e cmd:self vtab:ctx_.vtab ftab:ctx_.ftab];
    }
    return nil;
}
- (D9BlitEnc *)blitCommandEncoder {
    if (!buf_)
        return [[D9BlitEnc alloc] initWithClassic:[c_ blitCommandEncoder]];
    if (@available(macOS 26.0, iOS 26.0, *))
        return [[D9BlitEnc alloc] initWithM4:[(id<MTL4CommandBuffer>)buf_ computeCommandEncoder] cmd:self];
    return nil;
}
- (D9ComputeEnc *)computeCommandEncoder {
    if (!buf_)
        return [[D9ComputeEnc alloc] initWithClassic:[c_ computeCommandEncoder]];
    if (@available(macOS 26.0, iOS 26.0, *))
        return [[D9ComputeEnc alloc] initWithM4:[(id<MTL4CommandBuffer>)buf_ computeCommandEncoder]
                                            cmd:self
                                          table:ctx_.ctab];
    return nil;
}
- (void)barrierBeforeForeignWork {
    if (!buf_)
        return;
    if (@available(macOS 26.0, iOS 26.0, *)) {
        id<MTL4ComputeCommandEncoder> e = [(id<MTL4CommandBuffer>)buf_ computeCommandEncoder];
        [e barrierAfterQueueStages:kD9AllStages beforeStages:kD9AllStages visibilityOptions:MTL4VisibilityOptionDevice];
        [e endEncoding];
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
        id<MTL4CommandBuffer> cb = buf_;
        for (id<MTLAllocation> r in held_)
            [res_ addAllocation:r];
        [res_ commit];
        [cb useResidencySet:res_];   // between begin and end (validation)
        [cb endCommandBuffer];
        // The feedback handler runs once the GPU finished: it reports, then gives the memory back.
        D9M4 *ctx = ctx_;
        NSArray<D9CmdDone> *done = [done_ copy];
        NSMutableSet *held = held_;
        NSArray *chunks = [chunks_ copy];
        id alloc = alloc_;
        id<MTLResidencySet> res = res_;
        MTL4CommitOptions *opts = [MTL4CommitOptions new];
        [opts addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
          const double gpu = fb.GPUEndTime > fb.GPUStartTime ? fb.GPUEndTime - fb.GPUStartTime : 0.0;
          for (D9CmdDone d in done)
              d(fb.error, gpu);
          for (id<MTLBuffer> b in chunks)
              [ctx giveChunk:b];
          [ctx giveAllocator:alloc];
          [ctx giveResidency:res];
          [held removeAllObjects];
        }];
        id<MTL4CommandBuffer> list[1] = {cb};
        // The value at commit, not at creation: a command buffer made and committed while another is open (the
        // renderer's one-off ones) would otherwise signal out of order, the event would go back, and a wait hang.
        value_ = [ctx_ nextValue];
        [ctx_.queue commit:list count:1 options:opts];
        [ctx_.queue signalEvent:ctx_.done value:value_];
        held_ = nil;
        chunks_ = nil;
        chunk_ = nil;
        alloc_ = nil;
        res_ = nil;
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
