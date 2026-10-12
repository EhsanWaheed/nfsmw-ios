// remaster_tex.h - NFSMW Remaster A1, step 1: the texture keys the Metal D3D9 renderer sees.
//
// A mod keys a texture (PopModApi::d3d9_texture_tag); the key reaches the renderer in the texture's
// HostD9TextureDesc, or later through D3D9Backend::tag. Nothing here changes what is drawn. Off unless asked:
//   RECOMP_REMASTER_TEXDUMP=<dir>  write each keyed texture's top level once, as <key>.dds (or .bin for formats
//                                  DDS does not describe here), plus keys.csv with per-key draw counts;
//   RECOMP_REMASTER_TEXSTATS=1     counters only;
//   RECOMP_REMASTER_PACK=<dir>     replacement pack (step 2): <dir>/index.tsv lines "<key hex16>\t<slot>\t<file>"
//                                  (slot "albedo"; other material slots reserved) naming DDS files with full mip
//                                  chains (see read_dds for formats). RECOMP_REMASTER_TEXSWAP=0 loads but binds the
//                                  original (the Original switch).
// A summary line "[remaster] tex: ..." is logged every 600 presented frames while anything changed.
// Render thread only (every caller is a D3D9Backend method).
#pragma once

#include "../../../dx/host_d9.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remaster {

// Remaster run 12: the synthetic key of a texture key's normal slot (key ^ kSlotNormal; real keys carry the variant,
// 0 today, in the low 32 bits).
constexpr uint64_t kSlotNormal = 0x4E524D31ull;

class TexKeys {
public:
    TexKeys() {
        const char *d = std::getenv("RECOMP_REMASTER_TEXDUMP");
        if (d && *d) {
            dir_ = d;
            ::mkdir(d, 0755);
            on_ = true;
        }
        const char *s = std::getenv("RECOMP_REMASTER_TEXSTATS");
        if (s && *s == '1')
            on_ = true;
        const char *p = std::getenv("RECOMP_REMASTER_PACK");
        if (p && *p) {
            on_ = true;
            pack_dir_ = p;
            read_index();
        }
        const char *w = std::getenv("RECOMP_REMASTER_TEXSWAP");
        swap_ = !(w && *w == '0');
    }
    // The pack file for a key's albedo slot, or "" (and the lookup is counted either way).
    std::string pack_file(uint64_t key) {
        if (!key || pack_.empty())
            return {};
        auto it = pack_.find(key);
        if (it == pack_.end()) {
            ++look_none_;
            return {};
        }
        return it->second[0] == '/' ? it->second : pack_dir_ + "/" + it->second;   // run 12: slot rows are absolute
    }
    // Remaster run 12 PBR step 2 pilot: the "normal" rows of <dir>/index.tsv, under key ^ kSlotNormal (absolute paths).
    // A real key (low 32 bits = the variant) that equals a synthetic one keeps its row; the slot row is dropped.
    size_t add_slots(const std::string &dir) {
        std::FILE *f = std::fopen((dir + "/index.tsv").c_str(), "r");
        if (!f) {
            std::fprintf(stderr, "[remaster] pack: no slot index.tsv in %s\n", dir.c_str());
            return 0;
        }
        char line[512];
        size_t n = 0, clash = 0;
        while (std::fgets(line, sizeof line, f)) {
            char key[32], slot[32], file[400];
            if (line[0] == '#' || std::sscanf(line, "%31s %31s %399s", key, slot, file) != 3 || std::strcmp(slot, "normal"))
                continue;
            const uint64_t k = std::strtoull(key, nullptr, 16) ^ kSlotNormal;
            if (pack_.count(k)) {
                ++clash;
                continue;
            }
            pack_[k] = dir + "/" + file;
            ++n;
        }
        std::fclose(f);
        std::fprintf(stderr, "[remaster] pack: %zu normal slot entries from %s (%zu dropped: key clash)\n", n, dir.c_str(),
                     clash);
        return n;
    }
    bool swap() const { return swap_; }
    bool has_pack() const { return !pack_dir_.empty(); }
    // Run 15: every pack key with its file (albedo rows and slot rows), for the cache's startup warm.
    std::vector<std::pair<uint64_t, std::string>> pack_files() const {
        std::vector<std::pair<uint64_t, std::string>> v;
        v.reserve(pack_.size());
        for (const auto &kv : pack_)
            v.emplace_back(kv.first, kv.second[0] == '/' ? kv.second : pack_dir_ + "/" + kv.second);
        return v;
    }
    void redefined() { ++redefined_; changed_ = true; }
    void load_failed() { ++look_fail_; changed_ = true; }
    void load_rejected() { ++look_reject_; changed_ = true; }
    void loaded() { ++look_loaded_; changed_ = true; }
    void replaced_bind() { ++replaced_binds_; }
    void dropped_rewrite() { ++dropped_rewrite_; changed_ = true; }

    // A DDS file's mip chain. Formats (all UNORM; sRGB variants are rejected: the game samples linear):
    // legacy A8R8G8B8 (BGRA8), DXT1 (BC1), DXT5 (BC3); DX10 header BGRA8 87, RGBA8 28, BC1 71, BC3 77, BC7 98.
    enum Fmt { F_NONE, F_BGRA8, F_RGBA8, F_BC1, F_BC3, F_BC7 };
    struct Image {
        uint32_t w = 0, h = 0;
        Fmt fmt = F_NONE;
        std::vector<std::vector<uint8_t>> levels;
        std::vector<uint32_t> row_bytes; // per level: bytes per row of pixels, or per row of 4x4 blocks
        size_t bytes() const {
            size_t n = 0;
            for (const auto &l : levels)
                n += l.size();
            return n;
        }
    };
    static bool block_fmt(Fmt f) { return f == F_BC1 || f == F_BC3 || f == F_BC7; }
    static Image read_dds(const std::string &path) {
        Image im;
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            return im;
        uint32_t h[31] = {}, dx10[5] = {};
        char magic[4];
        bool ok = std::fread(magic, 1, 4, f) == 4 && !std::memcmp(magic, "DDS ", 4) && std::fread(h, 4, 31, f) == 31 &&
                  h[0] == 124 && h[18] == 32;
        Fmt fmt = F_NONE;
        if (ok && (h[19] & 0x4)) { // FOURCC
            const uint32_t cc = h[20];
            if (cc == fourcc('D', 'X', 'T', '1')) fmt = F_BC1;
            else if (cc == fourcc('D', 'X', 'T', '5')) fmt = F_BC3;
            else if (cc == fourcc('D', 'X', '1', '0')) {
                ok = std::fread(dx10, 4, 5, f) == 5 && dx10[1] == 3 /* TEXTURE2D */ && dx10[3] <= 1 /* one image */;
                switch (dx10[0]) {
                case 87: fmt = F_BGRA8; break;
                case 28: fmt = F_RGBA8; break;
                case 71: fmt = F_BC1; break;
                case 77: fmt = F_BC3; break;
                case 98: fmt = F_BC7; break;
                default: break; // includes the _SRGB variants (72, 78, 99, 91, 29): rejected
                }
            }
        } else if (ok && (h[19] & 0x40) && h[21] == 32 && h[22] == 0x00ff0000 && h[23] == 0x0000ff00 &&
                   h[24] == 0x000000ff) {
            fmt = F_BGRA8;
        }
        const uint32_t w = h[3], ht = h[2], mips = h[6] ? h[6] : 1;
        if (ok && fmt != F_NONE && w && ht && w <= 16384 && ht <= 16384 && mips <= 15) {
            const bool blk = block_fmt(fmt);
            const uint32_t unit = fmt == F_BC1 ? 8 : 16; // bytes per 4x4 block
            for (uint32_t l = 0; l < mips && ok; ++l) {
                const uint32_t lw = std::max<uint32_t>(w >> l, 1), lh = std::max<uint32_t>(ht >> l, 1);
                const uint32_t rows = blk ? (lh + 3) / 4 : lh;
                const uint32_t rb = blk ? (lw + 3) / 4 * unit : lw * 4;
                std::vector<uint8_t> px((size_t)rows * rb);
                ok = std::fread(px.data(), 1, px.size(), f) == px.size();
                if (ok) {
                    im.levels.push_back(std::move(px));
                    im.row_bytes.push_back(rb);
                }
            }
            if (ok) {
                im.w = w;
                im.h = ht;
                im.fmt = fmt;
            } else {
                im.levels.clear();
                im.row_bytes.clear();
            }
        }
        std::fclose(f);
        return im;
    }
    bool on() const { return on_; }

    void defined(const HostD9TextureDesc &d) {
        if (!d.key)
            return;
        Key &k = keys_[d.key];
        k.w = d.width;
        k.h = d.height;
        k.format = d.format;
        k.levels = d.levels;
        ++k.defines;
        ++defined_;
        changed_ = true;
    }
    void tagged_late(uint64_t key, const HostD9TextureDesc &d) {
        ++late_;
        HostD9TextureDesc c = d;
        c.key = key;
        defined(c);
    }
    void dropped(uint64_t key) {
        if (key)
            ++dropped_;
    }
    void bind(uint64_t key) {
        if (key) {
            ++keys_[key].binds;
            ++keyed_binds_;
        } else {
            ++unkeyed_binds_;
        }
    }
    // Top level, first face: dumped once per key.
    void upload(const HostD9TextureDesc &d, uint32_t face, uint32_t level, const uint8_t *bytes,
                uint32_t pitch) {
        if (!d.key)
            return;
        Key &k = keys_[d.key];
        ++k.uploads;
        if (dir_.empty() || k.dumped || face || level || !bytes)
            return;
        k.dumped = write_level(d, bytes, pitch);
        if (k.dumped)
            ++dumped_;
        changed_ = true;
    }
    void frame() {
        if (++frames_ % 600 || !changed_)
            return;
        changed_ = false;
        std::fprintf(stderr,
                     "[remaster] tex: frame %llu keys %zu defined %llu late-tagged %llu dropped %llu dumped %llu "
                     "draw-binds keyed %llu unkeyed %llu\n",
                     (unsigned long long)frames_, keys_.size(), (unsigned long long)defined_,
                     (unsigned long long)late_, (unsigned long long)dropped_, (unsigned long long)dumped_,
                     (unsigned long long)keyed_binds_, (unsigned long long)unkeyed_binds_);
        if (!pack_dir_.empty())
            std::fprintf(stderr,
                         "[remaster] pack: entries %zu swap %d lookups no-entry %llu load-failed %llu rejected %llu "
                         "loaded %llu; draw-binds replaced %llu; dropped (rewritten) %llu; redefined %llu\n",
                         pack_.size(), swap_ ? 1 : 0, (unsigned long long)look_none_,
                         (unsigned long long)look_fail_, (unsigned long long)look_reject_,
                         (unsigned long long)look_loaded_, (unsigned long long)replaced_binds_,
                         (unsigned long long)dropped_rewrite_, (unsigned long long)redefined_);
        if (!dir_.empty())
            write_csv();
    }

private:
    struct Key {
        uint32_t w = 0, h = 0, format = 0, levels = 0;
        uint64_t defines = 0, uploads = 0, binds = 0;
        bool dumped = false;
    };

    static uint32_t fourcc(char a, char b, char c, char d) {
        return (uint32_t)(uint8_t)a | (uint32_t)(uint8_t)b << 8 | (uint32_t)(uint8_t)c << 16 |
               (uint32_t)(uint8_t)d << 24;
    }

    bool write_level(const HostD9TextureDesc &d, const uint8_t *bytes, uint32_t pitch) {
        const uint32_t dxt1 = fourcc('D', 'X', 'T', '1'), dxt3 = fourcc('D', 'X', 'T', '3'),
                       dxt5 = fourcc('D', 'X', 'T', '5');
        const bool block = d.format == dxt1 || d.format == dxt3 || d.format == dxt5;
        const bool argb = d.format == 21 || d.format == 22; // A8R8G8B8, X8R8G8B8
        uint32_t rows, row_bytes;
        if (block) {
            rows = (d.height + 3) / 4 ? (d.height + 3) / 4 : 1;
            row_bytes = ((d.width + 3) / 4 ? (d.width + 3) / 4 : 1) * (d.format == dxt1 ? 8u : 16u);
        } else if (argb) {
            rows = d.height;
            row_bytes = d.width * 4;
        } else {
            rows = d.height;
            row_bytes = pitch; // undescribed format: raw rows as the game wrote them
        }
        if (pitch < row_bytes)
            return false;
        char name[64];
        std::snprintf(name, sizeof name, "%016llx.%s", (unsigned long long)d.key, block || argb ? "dds" : "bin");
        std::FILE *f = std::fopen((dir_ + "/" + name).c_str(), "wb");
        if (!f)
            return false;
        if (block || argb) {
            uint32_t h[32] = {};
            h[0] = 124;                                   // dwSize
            h[1] = 0x1007 | (block ? 0x80000u : 0x8u);    // CAPS|HEIGHT|WIDTH|PIXELFORMAT + LINEARSIZE or PITCH
            h[2] = d.height;
            h[3] = d.width;
            h[4] = block ? rows * row_bytes : row_bytes;  // linear size or pitch
            h[6] = 1;                                     // mip count: the top level only
            h[18] = 32;                                   // ddspf.dwSize
            if (block) {
                h[19] = 0x4;                              // DDPF_FOURCC
                h[20] = d.format;
            } else {
                h[19] = d.format == 21 ? 0x41u : 0x40u;   // RGB (+ALPHAPIXELS)
                h[21] = 32;
                h[22] = 0x00ff0000;
                h[23] = 0x0000ff00;
                h[24] = 0x000000ff;
                h[25] = d.format == 21 ? 0xff000000u : 0;
            }
            h[26] = 0x1000;                               // DDSCAPS_TEXTURE
            std::fwrite("DDS ", 1, 4, f);
            std::fwrite(h, 4, 31, f);
        }
        for (uint32_t r = 0; r < rows; ++r)
            std::fwrite(bytes + (size_t)r * pitch, 1, row_bytes, f);
        std::fclose(f);
        return true;
    }

    void write_csv() {
        std::FILE *f = std::fopen((dir_ + "/keys.csv").c_str(), "w");
        if (!f)
            return;
        std::fprintf(f, "key,name_hash,variant,width,height,format,levels,defines,uploads,draw_binds,dumped\n");
        for (const auto &kv : keys_) {
            const Key &k = kv.second;
            std::fprintf(f, "%016llx,%08x,%u,%u,%u,%u,%u,%llu,%llu,%llu,%d\n", (unsigned long long)kv.first,
                         (uint32_t)(kv.first >> 32), (uint32_t)kv.first, k.w, k.h, k.format, k.levels,
                         (unsigned long long)k.defines, (unsigned long long)k.uploads,
                         (unsigned long long)k.binds, k.dumped ? 1 : 0);
        }
        std::fclose(f);
    }

    void read_index() {
        std::FILE *f = std::fopen((pack_dir_ + "/index.tsv").c_str(), "r");
        if (!f) {
            std::fprintf(stderr, "[remaster] pack: no index.tsv in %s\n", pack_dir_.c_str());
            return;
        }
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            char key[32], slot[32], file[400];
            if (line[0] == '#' || std::sscanf(line, "%31s %31s %399s", key, slot, file) != 3)
                continue;
            if (!std::strcmp(slot, "albedo"))
                pack_[std::strtoull(key, nullptr, 16)] = file;
        }
        std::fclose(f);
        std::fprintf(stderr, "[remaster] pack: %zu albedo entries from %s\n", pack_.size(), pack_dir_.c_str());
    }

    bool on_ = false, changed_ = false, swap_ = true;
    std::string pack_dir_;
    std::unordered_map<uint64_t, std::string> pack_;
    uint64_t redefined_ = 0;
    uint64_t look_none_ = 0, look_fail_ = 0, look_reject_ = 0, look_loaded_ = 0, replaced_binds_ = 0,
             dropped_rewrite_ = 0;
    std::string dir_;
    std::unordered_map<uint64_t, Key> keys_;
    uint64_t frames_ = 0, defined_ = 0, late_ = 0, dropped_ = 0, dumped_ = 0, keyed_binds_ = 0,
             unkeyed_binds_ = 0;
};

} // namespace remaster
