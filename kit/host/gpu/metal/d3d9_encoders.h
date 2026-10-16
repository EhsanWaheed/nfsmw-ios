// d3d9_encoders.h - the command buffer and encoders the Direct3D 9 renderer (d3d9_metal.mm) encodes into: one set
// of objects that answer the same messages as Metal's own (setVertexBytes:length:atIndex:, drawPrimitives:..., and
// so on), so the renderer's call sites read as before, while the objects decide which Metal does the work
// (docs/METAL4_PLAN.md). Stage 1a: classic Metal only, a forwarding layer with no change in behaviour.
//
// D9Cmd wraps one command buffer: encoders come from it, it commits, it reports completion (error and GPU time) to
// D9CmdDone handlers, and `raw` gives the classic buffer to what needs one (MetalFX).
#pragma once
#import <Metal/Metal.h>

// Called once when the command buffer finished on the GPU: `error` nil on success, `gpu_s` the GPU time (0 unknown).
typedef void (^D9CmdDone)(NSError *error, double gpu_s);

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
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue;
- (D9RenderEnc *)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)rp;
- (D9BlitEnc *)blitCommandEncoder;
- (D9ComputeEnc *)computeCommandEncoder;
- (void)addDone:(D9CmdDone)done;   // before commit
- (void)commit;
- (void)waitUntilCompleted;
@end

// ---- classic Metal ------------------------------------------------------------------------------------------------

@implementation D9RenderEnc {
    id<MTLRenderCommandEncoder> c_;
}
- (instancetype)initWithClassic:(id<MTLRenderCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (id<MTLRenderCommandEncoder>)raw {
    return c_;
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p {
    [c_ setRenderPipelineState:p];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s {
    [c_ setDepthStencilState:s];
}
- (void)setStencilReferenceValue:(uint32_t)v {
    [c_ setStencilReferenceValue:v];
}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
    [c_ setDepthBias:bias slopeScale:slope clamp:clamp];
}
- (void)setCullMode:(MTLCullMode)m {
    [c_ setCullMode:m];
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)m {
    [c_ setTriangleFillMode:m];
}
- (void)setFrontFacingWinding:(MTLWinding)w {
    [c_ setFrontFacingWinding:w];
}
- (void)setViewport:(MTLViewport)v {
    [c_ setViewport:v];
}
- (void)setScissorRect:(MTLScissorRect)r {
    [c_ setScissorRect:r];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
    [c_ setBlendColorRed:r green:g blue:b alpha:a];
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
    [c_ setVisibilityResultMode:mode offset:offset];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    [c_ setVertexBytes:bytes length:length atIndex:index];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    [c_ setFragmentBytes:bytes length:length atIndex:index];
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [c_ setVertexBuffer:b offset:offset atIndex:index];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [c_ setFragmentBuffer:b offset:offset atIndex:index];
}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    [c_ setVertexTexture:t atIndex:index];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    [c_ setFragmentTexture:t atIndex:index];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index {
    [c_ setVertexSamplerState:s atIndex:index];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index {
    [c_ setFragmentSamplerState:s atIndex:index];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {
    [c_ drawPrimitives:type vertexStart:start vertexCount:count];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances {
    [c_ drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)itype
                  indexBuffer:(id<MTLBuffer>)ib
            indexBufferOffset:(NSUInteger)offset {
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
    [c_ endEncoding];
}
@end

@implementation D9BlitEnc {
    id<MTLBlitCommandEncoder> c_;
}
- (instancetype)initWithClassic:(id<MTLBlitCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
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
    [c_ endEncoding];
}
@end

@implementation D9ComputeEnc {
    id<MTLComputeCommandEncoder> c_;
}
- (instancetype)initWithClassic:(id<MTLComputeCommandEncoder>)enc {
    if (!enc)
        return nil;
    if ((self = [super init]))
        c_ = enc;
    return self;
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)p {
    [c_ setComputePipelineState:p];
}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    [c_ setTexture:t atIndex:index];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [c_ setBuffer:b offset:offset atIndex:index];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    [c_ setBytes:bytes length:length atIndex:index];
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads {
    [c_ dispatchThreadgroups:groups threadsPerThreadgroup:threads];
}
- (void)endEncoding {
    [c_ endEncoding];
}
@end

@implementation D9Cmd {
    id<MTLCommandBuffer> c_;
}
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (!cb)
        return nil;
    if ((self = [super init]))
        c_ = cb;
    return self;
}
- (id<MTLCommandBuffer>)raw {
    return c_;
}
- (D9RenderEnc *)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)rp {
    return [[D9RenderEnc alloc] initWithClassic:[c_ renderCommandEncoderWithDescriptor:rp]];
}
- (D9BlitEnc *)blitCommandEncoder {
    return [[D9BlitEnc alloc] initWithClassic:[c_ blitCommandEncoder]];
}
- (D9ComputeEnc *)computeCommandEncoder {
    return [[D9ComputeEnc alloc] initWithClassic:[c_ computeCommandEncoder]];
}
- (void)addDone:(D9CmdDone)done {
    [c_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      done(cb.error, cb.GPUEndTime > cb.GPUStartTime ? cb.GPUEndTime - cb.GPUStartTime : 0.0);
    }];
}
- (void)commit {
    [c_ commit];
}
- (void)waitUntilCompleted {
    [c_ waitUntilCompleted];
}
@end
