// USA SDK movie renderer: shader8200fe00 (<=576 lines),8200ffc0 (>576).
// Game caller8245dc48 ->826d74b8 ->826d7148 ->826db010. Construction826db6f0,
// selection826db2b4, binding826db4f8. See milestone31 reference.json.
// These decimal literals round to the exact original float32 shader constants.
#pragma once
#include "../kit/dx/mf_media.h"

inline bool usa_movie_color(uint32_t width, uint32_t height,
                            mf::VideoColorDescription source,
                            VideoColorConversion *out) {
    using C = mf::VideoColorDescription;
    if (!out || source.matrix != C::Matrix::Unspecified ||
        source.range != C::Range::Unspecified ||
        !((width == 1280 && height == 720) || (width == 1024 && height == 576)))
        return false;
    out->y_offset = 0.0625f;
    out->uv_offset = 0.5f;
    out->y_scale = 1.164382815361023f;
    if (height > 576) {
        out->v_to_r = 1.7926523685455322f;
        out->u_to_g = -0.21323828399181366f;
        out->v_to_g = -0.5330039262771606f;
        out->u_to_b = 2.1124179363250732f;
    } else {
        out->v_to_r = 1.5960273742675781f;
        out->u_to_g = -0.3933164179325104f;
        out->v_to_g = -0.8133906126022339f;
        out->u_to_b = 2.016238212585449f;
    }
    return true;
}
