// make_pad_buttons.swift - draws the controller button glyphs the menus show for a pad and packs them as a
// Most Wanted texture pack (TPK v5), build/generated/padicons/GLOBAL/RecompPadButtons.tpk (tools/build.py runs
// it; the app bundles build/generated into the core mod's folder).
//
//   swift tools/padicons/make_pad_buttons.swift [output.tpk]
//
// The art is drawn here (CoreGraphics, no game data): PlayStation shapes and Xbox letters on a dark disc,
// shoulders and triggers as labelled caps, sticks and the d-pad. frontend_pad.h swaps the console prompt
// objects' texture hashes to the PS3_* or XBX_* names for the controller in use.
//
// The pack's layout is the one the game's own packs use (GLOBAL/GLOBALB.BUN, GlobalTexturesPC.tpk): a
// 0xB3300000 container with the 0xB3310000 header part (pack info 0x33310001, the sorted hash table
// 0x33310002, 124-byte texture infos 0x33310004, 32-byte format records 0x33310005) and the 0xB3320000 data
// part (0x33320001, then 0x33320002 whose texel data starts 0x80-aligned). Every texture is 128x128 DXT3
// with one level, the format of the game's own menu icons (PC_KEY_ENTER).
import CoreGraphics
import CoreText
import Foundation
import ImageIO

let size = 128

// ---- the game's string hash (bStringHash) ----------------------------------------------------------------
func bHash(_ s: String) -> UInt32 {
    var h: UInt32 = 0xffff_ffff
    for c in s.utf8 { h = h &* 33 &+ UInt32(c) }
    return h
}

// ---- drawing ------------------------------------------------------------------------------------------------
struct RGBA { var r, g, b, a: Double }
func rgb(_ hex: UInt32, _ a: Double = 1) -> RGBA {
    RGBA(r: Double((hex >> 16) & 0xff) / 255, g: Double((hex >> 8) & 0xff) / 255, b: Double(hex & 0xff) / 255, a: a)
}
let discFill = rgb(0x1d1d22)
let discRing = rgb(0xffffff, 0.30)
let white = rgb(0xf4f4f6)

extension CGContext {
    func fill(_ c: RGBA) { setFillColor(red: c.r, green: c.g, blue: c.b, alpha: c.a) }
    func stroke(_ c: RGBA) { setStrokeColor(red: c.r, green: c.g, blue: c.b, alpha: c.a) }
}

let mid = CGFloat(size) / 2

func disc(_ ctx: CGContext) {
    let r = CGRect(x: 4, y: 4, width: size - 8, height: size - 8)
    ctx.fill(discFill)
    ctx.fillEllipse(in: r)
    ctx.stroke(discRing)
    ctx.setLineWidth(4)
    ctx.strokeEllipse(in: r.insetBy(dx: 2, dy: 2))
}

// A cap for a shoulder (rounded below) or a trigger (rounded above), the way the pads draw them.
func cap(_ ctx: CGContext, trigger: Bool) {
    let r = CGRect(x: 6, y: 22, width: size - 12, height: size - 44)
    let big: CGFloat = 34, small: CGFloat = 12
    let p = CGMutablePath()
    // CoreGraphics' origin is bottom-left: "above" is larger y.
    let tl = trigger ? big : small, tr = tl, bl = trigger ? small : big, br = bl
    p.move(to: CGPoint(x: r.minX + bl, y: r.minY))
    p.addLine(to: CGPoint(x: r.maxX - br, y: r.minY))
    p.addArc(tangent1End: CGPoint(x: r.maxX, y: r.minY), tangent2End: CGPoint(x: r.maxX, y: r.maxY), radius: br)
    p.addArc(tangent1End: CGPoint(x: r.maxX, y: r.maxY), tangent2End: CGPoint(x: r.minX, y: r.maxY), radius: tr)
    p.addArc(tangent1End: CGPoint(x: r.minX, y: r.maxY), tangent2End: CGPoint(x: r.minX, y: r.minY), radius: tl)
    p.addArc(tangent1End: CGPoint(x: r.minX, y: r.minY), tangent2End: CGPoint(x: r.maxX, y: r.minY), radius: bl)
    p.closeSubpath()
    ctx.fill(discFill)
    ctx.addPath(p)
    ctx.fillPath()
    ctx.stroke(discRing)
    ctx.setLineWidth(4)
    ctx.addPath(p)
    ctx.strokePath()
}

func text(_ ctx: CGContext, _ s: String, _ color: RGBA, _ points: CGFloat, dy: CGFloat = 0) {
    let font = CTFontCreateUIFontForLanguage(.emphasizedSystem, points, nil)!
    let attrs: [NSAttributedString.Key: Any] = [
        NSAttributedString.Key(kCTFontAttributeName as String): font,
        NSAttributedString.Key(kCTForegroundColorAttributeName as String):
            CGColor(red: color.r, green: color.g, blue: color.b, alpha: color.a),
    ]
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attrs))
    let b = CTLineGetImageBounds(line, ctx)
    ctx.textPosition = CGPoint(x: mid - b.width / 2 - b.minX, y: mid - b.height / 2 - b.minY + dy)
    CTLineDraw(line, ctx)
}

enum PS { case cross, circle, square, triangle }
func psFace(_ ctx: CGContext, _ s: PS) {
    disc(ctx)
    ctx.setLineWidth(10)
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)
    switch s {
    case .cross:
        ctx.stroke(rgb(0x86aef0))
        let d: CGFloat = 25
        ctx.move(to: CGPoint(x: mid - d, y: mid - d)); ctx.addLine(to: CGPoint(x: mid + d, y: mid + d))
        ctx.move(to: CGPoint(x: mid - d, y: mid + d)); ctx.addLine(to: CGPoint(x: mid + d, y: mid - d))
        ctx.strokePath()
    case .circle:
        ctx.stroke(rgb(0xff6670))
        ctx.strokeEllipse(in: CGRect(x: mid - 29, y: mid - 29, width: 58, height: 58))
    case .square:
        ctx.stroke(rgb(0xec92cf))
        ctx.stroke(CGRect(x: mid - 26, y: mid - 26, width: 52, height: 52))
    case .triangle:
        ctx.stroke(rgb(0x46deb0))
        let r: CGFloat = 33, cy = mid - 4
        ctx.move(to: CGPoint(x: mid, y: cy + r))
        ctx.addLine(to: CGPoint(x: mid - r * 0.866, y: cy - r * 0.5))
        ctx.addLine(to: CGPoint(x: mid + r * 0.866, y: cy - r * 0.5))
        ctx.closePath()
        ctx.strokePath()
    }
}

func letterFace(_ ctx: CGContext, _ s: String, _ color: RGBA) {
    disc(ctx)
    text(ctx, s, color, 76)
}

func capLabel(_ ctx: CGContext, _ s: String, trigger: Bool) {
    cap(ctx, trigger: trigger)
    text(ctx, s, white, 50)
}

// A stick seen from above: the disc, its cap's ring, and its letter(s).
func stick(_ ctx: CGContext, _ s: String) {
    disc(ctx)
    ctx.stroke(rgb(0xffffff, 0.55))
    ctx.setLineWidth(6)
    ctx.strokeEllipse(in: CGRect(x: mid - 40, y: mid - 40, width: 80, height: 80))
    text(ctx, s, white, s.count > 1 ? 40 : 50)
}

// Options / Menu: three lines.
func menuButton(_ ctx: CGContext) {
    disc(ctx)
    ctx.stroke(white)
    ctx.setLineWidth(9)
    ctx.setLineCap(.round)
    for dy: CGFloat in [-20, 0, 20] {
        ctx.move(to: CGPoint(x: mid - 26, y: mid + dy))
        ctx.addLine(to: CGPoint(x: mid + 26, y: mid + dy))
    }
    ctx.strokePath()
}

// The d-pad: a plus, its lit arms white, the rest dimmed.
struct Arms: OptionSet { let rawValue: Int
    static let up = Arms(rawValue: 1), down = Arms(rawValue: 2), left = Arms(rawValue: 4), right = Arms(rawValue: 8)
    static let all: Arms = [.up, .down, .left, .right]
}
func dpad(_ ctx: CGContext, _ lit: Arms) {
    disc(ctx)
    let w: CGFloat = 30, reach: CGFloat = 46
    // Opaque colours: translucent arms would darken where they overlap the centre.
    let dim = rgb(0x5c5c63)
    func arm(_ a: Arms, _ r: CGRect) {
        ctx.fill(lit.contains(a) ? white : dim)
        ctx.addPath(CGPath(roundedRect: r, cornerWidth: 7, cornerHeight: 7, transform: nil))
        ctx.fillPath()
    }
    ctx.fill(dim)
    ctx.fill(CGRect(x: mid - w / 2, y: mid - w / 2, width: w, height: w))
    arm(.up, CGRect(x: mid - w / 2, y: mid + 6, width: w, height: reach - 6))
    arm(.down, CGRect(x: mid - w / 2, y: mid - reach, width: w, height: reach - 6))
    arm(.left, CGRect(x: mid - reach, y: mid - w / 2, width: reach - 6, height: w))
    arm(.right, CGRect(x: mid + 6, y: mid - w / 2, width: reach - 6, height: w))
}

let glyphs: [(String, (CGContext) -> Void)] = [
    // PlayStation (the names XtendedInput's pack used, so frontend_pad.h's table carries over)
    ("PS3_CROSS", { psFace($0, .cross) }),
    ("PS3_CIRCLE", { psFace($0, .circle) }),
    ("PS3_SQUARE", { psFace($0, .square) }),
    ("PS3_TRIANGLE", { psFace($0, .triangle) }),
    ("PS3_L1", { capLabel($0, "L1", trigger: false) }),
    ("PS3_R1", { capLabel($0, "R1", trigger: false) }),
    ("PS3_L2", { capLabel($0, "L2", trigger: true) }),
    ("PS3_R2", { capLabel($0, "R2", trigger: true) }),
    ("PS3_L3", { stick($0, "L3") }),
    ("PS3_R3", { stick($0, "R3") }),
    ("PS3_LANALOG", { stick($0, "L") }),
    ("PS3_RANALOG", { stick($0, "R") }),
    ("PS3_START", { menuButton($0) }),
    // The d-pad is the same on both pads.
    ("PS3_DPAD", { dpad($0, .all) }),
    ("PS3_DPAD_Y", { dpad($0, [.up, .down]) }),
    ("PS3_DPAD_X", { dpad($0, [.left, .right]) }),
    ("PS3_DPAD_UP", { dpad($0, .up) }),
    ("PS3_DPAD_DOWN", { dpad($0, .down) }),
    ("PS3_DPAD_LEFT", { dpad($0, .left) }),
    ("PS3_DPAD_RIGHT", { dpad($0, .right) }),
    // Xbox
    ("XBX_A", { letterFace($0, "A", rgb(0x6cc04a)) }),
    ("XBX_B", { letterFace($0, "B", rgb(0xf0484e)) }),
    ("XBX_X", { letterFace($0, "X", rgb(0x3d8ff0)) }),
    ("XBX_Y", { letterFace($0, "Y", rgb(0xf5c518)) }),
    ("XBX_LB", { capLabel($0, "LB", trigger: false) }),
    ("XBX_RB", { capLabel($0, "RB", trigger: false) }),
    ("XBX_LT", { capLabel($0, "LT", trigger: true) }),
    ("XBX_RT", { capLabel($0, "RT", trigger: true) }),
    ("XBX_LS", { stick($0, "LS") }),
    ("XBX_RS", { stick($0, "RS") }),
    ("XBX_LSTICK", { stick($0, "L") }),
    ("XBX_RSTICK", { stick($0, "R") }),
    ("XBX_MENU", { menuButton($0) }),
]

// Straight (not premultiplied) RGBA, top row first. Fully transparent texels take the disc's colour, so
// filtering at the edge blends towards the glyph's own rim rather than black.
func render(_ draw: (CGContext) -> Void) -> [UInt8] {
    var px = [UInt8](repeating: 0, count: size * size * 4)
    let ctx = CGContext(data: &px, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    // A bitmap context's first row in memory is the picture's top (the texture's first row), with y up in
    // the drawing, so the art above draws as it reads.
    ctx.setShouldAntialias(true)
    draw(ctx)
    let bleed = [UInt8(discFill.r * 255), UInt8(discFill.g * 255), UInt8(discFill.b * 255)]
    for i in stride(from: 0, to: px.count, by: 4) {
        let a = Double(px[i + 3])
        if a == 0 {
            px[i] = bleed[0]; px[i + 1] = bleed[1]; px[i + 2] = bleed[2]
        } else if a < 255 {
            for k in 0..<3 { px[i + k] = UInt8(min(255, (Double(px[i + k]) * 255 / a).rounded())) }
        }
    }
    return px
}

// ---- DXT3 -------------------------------------------------------------------------------------------------
func to565(_ r: Int, _ g: Int, _ b: Int) -> UInt16 {
    UInt16(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255))
}
func from565(_ c: UInt16) -> (Int, Int, Int) {
    let r = Int(c >> 11) & 31, g = Int(c >> 5) & 63, b = Int(c) & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
}

// One 4x4 block: explicit 4-bit alpha, then a colour block in four-colour mode (DXT3 always is). The end
// points are the two most different visible texels; each texel takes the nearest of the four colours.
func dxt3Block(_ px: [UInt8], _ bx: Int, _ by: Int) -> [UInt8] {
    var texel = [(Int, Int, Int, Int)]()
    for y in 0..<4 {
        for x in 0..<4 {
            let i = ((by * 4 + y) * size + bx * 4 + x) * 4
            texel.append((Int(px[i]), Int(px[i + 1]), Int(px[i + 2]), Int(px[i + 3])))
        }
    }
    var out = [UInt8]()
    for i in stride(from: 0, to: 16, by: 2) {
        let a0 = (texel[i].3 * 15 + 127) / 255, a1 = (texel[i + 1].3 * 15 + 127) / 255
        out.append(UInt8(a0 | (a1 << 4)))
    }
    let visible = texel.filter { $0.3 > 0 }
    let pool = visible.isEmpty ? texel : visible
    var best = (pool[0], pool[0]), bestD = -1
    for p in pool {
        for q in pool {
            let d = (p.0 - q.0) * (p.0 - q.0) + (p.1 - q.1) * (p.1 - q.1) + (p.2 - q.2) * (p.2 - q.2)
            if d > bestD { bestD = d; best = (p, q) }
        }
    }
    var c0 = to565(best.0.0, best.0.1, best.0.2), c1 = to565(best.1.0, best.1.1, best.1.2)
    if c0 < c1 { swap(&c0, &c1) }
    let e0 = from565(c0), e1 = from565(c1)
    let palette = [e0, e1, ((2 * e0.0 + e1.0) / 3, (2 * e0.1 + e1.1) / 3, (2 * e0.2 + e1.2) / 3),
                   ((e0.0 + 2 * e1.0) / 3, (e0.1 + 2 * e1.1) / 3, (e0.2 + 2 * e1.2) / 3)]
    var bits: UInt32 = 0
    for (i, t) in texel.enumerated() {
        var pick = 0, pickD = Int.max
        for (k, c) in palette.enumerated() {
            let d = (t.0 - c.0) * (t.0 - c.0) + (t.1 - c.1) * (t.1 - c.1) + (t.2 - c.2) * (t.2 - c.2)
            if d < pickD { pickD = d; pick = k }
        }
        bits |= UInt32(pick) << (2 * UInt32(i))
    }
    out += [UInt8(c0 & 0xff), UInt8(c0 >> 8), UInt8(c1 & 0xff), UInt8(c1 >> 8)]
    out += [UInt8(bits & 0xff), UInt8((bits >> 8) & 0xff), UInt8((bits >> 16) & 0xff), UInt8(bits >> 24)]
    return out
}
func dxt3(_ px: [UInt8]) -> [UInt8] {
    var out = [UInt8]()
    for by in 0..<(size / 4) { for bx in 0..<(size / 4) { out += dxt3Block(px, bx, by) } }
    return out
}

// ---- the pack ---------------------------------------------------------------------------------------------
extension Array where Element == UInt8 {
    mutating func u8(_ v: Int) { append(UInt8(v & 0xff)) }
    mutating func u16(_ v: Int) { u8(v); u8(v >> 8) }
    mutating func u32(_ v: UInt32) { for k in 0..<4 { append(UInt8((v >> (8 * UInt32(k))) & 0xff)) } }
    mutating func zeros(_ n: Int) { append(contentsOf: [UInt8](repeating: 0, count: n)) }
    mutating func str(_ s: String, _ n: Int) {
        var b = Array(s.utf8.prefix(n - 1)); b += [UInt8](repeating: 0, count: n - b.count); append(contentsOf: b)
    }
}
func chunk(_ id: UInt32, _ payload: [UInt8]) -> [UInt8] {
    var c = [UInt8](); c.u32(id); c.u32(UInt32(payload.count)); return c + payload
}
// A filler chunk (id 0) that makes the next chunk's header start at a multiple of `align` from the file's
// start, given the absolute offset `at` where the filler goes.
func pad(at: Int, align: Int) -> [UInt8] {
    var n = (align - (at + 8) % align) % align
    if n < 0 { n += align }
    return chunk(0, [UInt8](repeating: 0, count: n))
}

let packFile = "GLOBAL\\RecompPadButtons.tpk"
let packHash = bHash(packFile)
let classHash: UInt32 = 0x001a_93cf // every texture of the game's own packs carries this class

struct Tex { let name: String; let hash: UInt32; let data: [UInt8] }
var textures = glyphs.map { name, draw in Tex(name: name, hash: bHash(name), data: dxt3(render(draw))) }
textures.sort { $0.hash < $1.hash } // the game looks textures up in this table by hash
precondition(Set(textures.map(\.hash)).count == textures.count, "two glyph names share a hash")

var offsets = [Int](), dataBytes = [UInt8]()
for t in textures {
    offsets.append(dataBytes.count)
    dataBytes += t.data
    dataBytes.zeros((0x80 - dataBytes.count % 0x80) % 0x80)
}

var info = [UInt8]()
info.u32(5) // pack version (Most Wanted)
info.str("PADBUTTONS", 28)
info.str(packFile, 64)
info.u32(packHash)
info.zeros(24)

var hashes = [UInt8](), infos = [UInt8](), formats = [UInt8]()
let shift = Int(log2(Double(size)))
for (t, off) in zip(textures, offsets) {
    hashes.u32(t.hash); hashes.u32(0)
    var r = [UInt8]()
    r.zeros(12)
    r.str(t.name, 24)
    r.u32(t.hash)
    r.u32(classHash)
    r.u32(0)
    r.u32(UInt32(off))           // 0x30 texel data, from the data part's start
    r.u32(0)
    r.u32(UInt32(t.data.count))  // 0x38 bytes, all levels
    r.u32(0)
    r.u32(UInt32(t.data.count))  // 0x40 bytes of the top level
    r.u16(size); r.u16(size)     // 0x44
    r.u8(shift); r.u8(shift)     // 0x48 log2 width, height
    r.u8(0x24); r.u8(0)          // 0x4a DXT3
    r.u16(0)                     // 0x4c palette entries
    r.u8(1); r.u8(0)             // 0x4e levels
    // 0x50.. as the game's menu icons (PC_KEY_ENTER): drawn late (5), alpha blended (2, 1), scale 1.0 (8.8).
    r += [0x00, 0x05, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00]
    r.zeros(8)
    r += [0x00, 0x01, 0x00, 0x01]
    r.zeros(124 - r.count)
    precondition(r.count == 124)
    infos += r
    var f = [UInt8]()
    f.zeros(8); f.u32(1); f.u32(5); f.u32(6)
    f += Array("DXT3".utf8)
    f.zeros(8)
    precondition(f.count == 32)
    formats += f
}

let header = chunk(0xB331_0000, chunk(0x3331_0001, info) + chunk(0x3331_0002, hashes) +
                   chunk(0x3331_0004, infos) + chunk(0x3331_0005, formats))

// Assemble with absolute offsets in mind: the container's header (8), a filler so the header part starts at
// 0x40 (as in the game's bundles), the header part, a filler so the data part starts 0x80-aligned, then the
// data part: 0x33320001, a filler to the next 0x80, and 0x33320002 whose payload opens with 0x78 bytes of
// 0x11 so the texels themselves start 0x80-aligned.
var body = [UInt8]()
body += chunk(0, [UInt8](repeating: 0, count: 0x40 - 16)) // 8 + 8 + 48 = 0x40
body += header
body += pad(at: 8 + body.count, align: 0x80)
let dataStart = 8 + body.count
var dataInfo = [UInt8](); dataInfo.zeros(8); dataInfo.u32(1); dataInfo.u32(packHash); dataInfo.zeros(8)
var dataPart = chunk(0x3332_0001, dataInfo)
dataPart += pad(at: dataStart + 8 + dataPart.count, align: 0x80)
let texelStart = dataStart + 8 + dataPart.count + 8 + 0x78
dataPart += chunk(0x3332_0002, [UInt8](repeating: 0x11, count: 0x78) + dataBytes)
body += chunk(0xB332_0000, dataPart)
let file = chunk(0xB330_0000, body)
precondition(dataStart % 0x80 == 0, "data part not 0x80-aligned")
precondition(texelStart % 0x80 == 0, "texels not 0x80-aligned")

// --preview <png>: every glyph decoded back from its DXT3 on a mid grey, to check the art and the encoder.
if let i = CommandLine.arguments.firstIndex(of: "--preview"), i + 1 < CommandLine.arguments.count {
    let cols = 8, rows = (textures.count + cols - 1) / cols
    let W = cols * size, H = rows * size
    var sheet = [UInt8](repeating: 0, count: W * H * 4)
    for (n, t) in textures.enumerated() {
        let ox = (n % cols) * size, oy = (n / cols) * size
        for b in 0..<(size * size / 16) {
            let blk = Array(t.data[(b * 16)..<(b * 16 + 16)])
            let c0 = UInt16(blk[8]) | UInt16(blk[9]) << 8, c1 = UInt16(blk[10]) | UInt16(blk[11]) << 8
            let e0 = from565(c0), e1 = from565(c1)
            let pal = [e0, e1, ((2 * e0.0 + e1.0) / 3, (2 * e0.1 + e1.1) / 3, (2 * e0.2 + e1.2) / 3),
                       ((e0.0 + 2 * e1.0) / 3, (e0.1 + 2 * e1.1) / 3, (e0.2 + 2 * e1.2) / 3)]
            let bits = UInt32(blk[12]) | UInt32(blk[13]) << 8 | UInt32(blk[14]) << 16 | UInt32(blk[15]) << 24
            for k in 0..<16 {
                let a = Int((blk[k / 2] >> (4 * UInt8(k % 2))) & 0xf) * 17
                let c = pal[Int((bits >> (2 * UInt32(k))) & 3)]
                let x = ox + (b % (size / 4)) * 4 + k % 4, y = oy + (b / (size / 4)) * 4 + k / 4
                let p = (y * W + x) * 4
                sheet[p] = UInt8((c.0 * a + 128 * (255 - a)) / 255)
                sheet[p + 1] = UInt8((c.1 * a + 128 * (255 - a)) / 255)
                sheet[p + 2] = UInt8((c.2 * a + 128 * (255 - a)) / 255)
                sheet[p + 3] = 255
            }
        }
    }
    let ctx = CGContext(data: &sheet, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    let url = URL(fileURLWithPath: CommandLine.arguments[i + 1]) as CFURL
    let dest = CGImageDestinationCreateWithURL(url, "public.png" as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
    CGImageDestinationFinalize(dest)
}

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1]
    : "build/generated/padicons/GLOBAL/RecompPadButtons.tpk"
try FileManager.default.createDirectory(atPath: (out as NSString).deletingLastPathComponent,
                                        withIntermediateDirectories: true)
try Data(file).write(to: URL(fileURLWithPath: out))
print("wrote \(out): \(textures.count) textures, \(file.count) bytes")
