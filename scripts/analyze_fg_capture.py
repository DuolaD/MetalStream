#!/usr/bin/env python3
"""Analyze a MetalStream frame-generation capture (metalstream.ini: FGCapture=1, press F10 in game).

For each captured frame k the capture holds R(k) = real frame, G(k) = generated frame between
R(k-1) and R(k), plus the HUD-less color, depth and motion vectors MetalStream fed to D3DMetal.

The script
  * writes PNGs (real, generated, depth, motion vectors) next to the raw files,
  * measures the global image shift R(k-1) -> R(k) and R(k-1) -> G(k) by block matching
    (a correct generated frame sits halfway: ratio ~0.5),
  * compares that with the shift implied by the motion vectors as MetalStream scaled them.

Usage: scripts/analyze_fg_capture.py <capture dir> [burst number]
Needs Pillow (no numpy).
"""
import os
import re
import struct
import sys

from PIL import Image, ImageChops, ImageStat

DXGI_R8G8B8A8 = {27, 28, 29}          # TYPELESS / UNORM / UNORM_SRGB
DXGI_R32 = {39, 40, 41}               # TYPELESS / D32_FLOAT / R32_FLOAT
DXGI_R16G16 = {33, 34}                # TYPELESS / FLOAT
DXGI_R32G8X24 = {19, 20}              # TYPELESS / D32_FLOAT_S8X24_UINT


def read_meta(path):
    items, extra = {}, {}
    for line in open(path):
        line = line.strip()
        m = re.match(r"(\w+) width=(\d+) height=(\d+) format=(\d+) rowpitch=(\d+)$", line)
        if m:
            items[m.group(1)] = dict(w=int(m.group(2)), h=int(m.group(3)), fmt=int(m.group(4)), pitch=int(m.group(5)))
            continue
        for kv in re.split(r" (?=\w+=)", line):
            if "=" in kv:
                k, v = kv.split("=", 1)
                extra[k] = v
    return items, extra


def load_rgba(path, info):
    raw = open(path, "rb").read()
    w, h, pitch = info["w"], info["h"], info["pitch"]
    rows = b"".join(raw[y * pitch:y * pitch + w * 4] for y in range(h))
    return Image.frombytes("RGBA", (w, h), rows).convert("RGB")


def load_floats(path, info, comps, kind):
    raw = open(path, "rb").read()
    w, h, pitch = info["w"], info["h"], info["pitch"]
    size = {"f": 4, "e": 2}[kind] * comps
    fmt = "<" + kind * (w * comps)
    out = []
    for y in range(h):
        out.append(struct.unpack_from(fmt, raw, y * pitch))
    return w, h, out


def load_depth(path, info):
    if info["fmt"] in DXGI_R32G8X24:
        # 8 bytes per texel: float depth + stencil
        raw = open(path, "rb").read()
        w, h, pitch = info["w"], info["h"], info["pitch"]
        rows = [struct.unpack_from("<" + "fI" * w, raw, y * pitch)[0::2] for y in range(h)]
        return w, h, rows
    return load_floats(path, info, 1, "f")


def to_gray_small(img, scale):
    g = img.convert("L")
    return g.resize((g.width // scale, g.height // scale), Image.BILINEAR)


def global_shift(a, b, max_shift):
    """Shift (dx, dy) such that b(x, y) ~ a(x - dx, y - dy), by exhaustive SAD over a central crop."""
    w, h = a.size
    m = max_shift
    box = (int(w * 0.2) + m, int(h * 0.2) + m, int(w * 0.8) - m, int(h * 0.8) - m)
    target = b.crop(box)
    best, best_err, errs = (0, 0), None, {}
    for dy in range(-m, m + 1):
        for dx in range(-m, m + 1):
            src = a.crop((box[0] - dx, box[1] - dy, box[2] - dx, box[3] - dy))
            err = ImageStat.Stat(ImageChops.difference(src, target)).mean[0]
            errs[(dx, dy)] = err
            better = best_err is None or err < best_err - 1e-6 or \
                (abs(err - best_err) <= 1e-6 and abs(dx) + abs(dy) < abs(best[0]) + abs(best[1]))
            if better:
                best, best_err = (dx, dy), err

    def refine(axis):
        d = best[axis]
        lo = list(best); hi = list(best)
        lo[axis] -= 1; hi[axis] += 1
        e0, e1, e2 = errs.get(tuple(lo)), errs[best], errs.get(tuple(hi))
        if e0 is None or e2 is None:
            return float(d)
        denom = e0 - 2 * e1 + e2
        return d + (0.5 * (e0 - e2) / denom if denom > 1e-9 else 0.0)

    return refine(0), refine(1), best_err


def mvec_stats(mv, ngx_scale, out_w, mv_w):
    w, h, rows = mv
    xs, ys = [], []
    for y in range(int(h * 0.2), int(h * 0.8), 4):
        row = rows[y]
        for x in range(int(w * 0.2), int(w * 0.8), 4):
            xs.append(row[2 * x]); ys.append(row[2 * x + 1])
    xs.sort(); ys.sort()
    med = lambda v: v[len(v) // 2]
    to_out = out_w / mv_w
    return dict(raw_median=(med(xs), med(ys)), raw_min=(xs[0], ys[0]), raw_max=(xs[-1], ys[-1]),
                # NGX convention (verified by test_gptk_fg_probe): mv * MvecScale = pixels in mvec space,
                # pointing from the current pixel to its previous position
                prev_minus_curr_px_out=(med(xs) * ngx_scale[0] * to_out, med(ys) * ngx_scale[1] * to_out))


def save_mvec_png(mv, path, scale_px):
    w, h, rows = mv
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = rows[y]
        for x in range(w):
            vx, vy = row[2 * x] * scale_px[0], row[2 * x + 1] * scale_px[1]
            px[x, y] = (max(0, min(255, int(128 + vx * 4))), max(0, min(255, int(128 + vy * 4))), 128)
    img.save(path)


def save_depth_png(depth, path):
    w, h, rows = depth
    flat = [v for r in rows[::8] for v in r[::8] if v == v]
    lo, hi = (min(flat), max(flat)) if flat else (0.0, 1.0)
    span = (hi - lo) or 1.0
    img = Image.new("L", (w, h))
    px = img.load()
    for y in range(h):
        for x, v in enumerate(rows[y]):
            px[x, y] = int(255 * (v - lo) / span) if v == v else 0
    img.save(path)
    return lo, hi


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cap = sys.argv[1]
    bursts = sorted({int(m.group(1)) for f in os.listdir(cap) for m in [re.match(r"burst(\d+)_frame\d_meta\.txt", f)] if m})
    if len(sys.argv) > 2:
        bursts = [int(sys.argv[2])]
    for burst in bursts:
        print(f"=== burst {burst}")
        frames = []
        for k in range(3):
            prefix = os.path.join(cap, f"burst{burst:02d}_frame{k}_")
            if not os.path.exists(prefix + "meta.txt"):
                break
            items, extra = read_meta(prefix + "meta.txt")
            fr = dict(items=items, extra=extra, prefix=prefix)
            fr["real"] = load_rgba(prefix + "real.raw", items["real"])
            fr["generated"] = load_rgba(prefix + "generated.raw", items["generated"])
            fr["real"].save(prefix + "real.png")
            fr["generated"].save(prefix + "generated.png")
            if "mvecs" in items and items["mvecs"]["fmt"] in DXGI_R16G16:
                fr["mvecs"] = load_floats(prefix + "mvecs.raw", items["mvecs"], 2, "e")
            if "depth" in items and (items["depth"]["fmt"] in DXGI_R32 or items["depth"]["fmt"] in DXGI_R32G8X24):
                fr["depth"] = load_depth(prefix + "depth.raw", items["depth"])
            frames.append(fr)
            print(f"frame {k}: " + " ".join(f"{n}={i['w']}x{i['h']}/fmt{i['fmt']}" for n, i in items.items()))
            print(f"         {extra.get('ngxMvecScale')=} {extra.get('slMvecScale')=} {extra.get('jitter')=} "
                  f"{extra.get('depthInverted')=} {extra.get('mvecJittered')=} {extra.get('reset')=}")

        for k, fr in enumerate(frames):
            ngx = tuple(float(v) for v in fr["extra"]["ngxMvecScale"].split(","))
            if "mvecs" in fr:
                st = mvec_stats(fr["mvecs"], ngx, fr["items"]["real"]["w"], fr["items"]["mvecs"]["w"])
                print(f"frame {k} mvecs: raw median={st['raw_median']} min={st['raw_min']} max={st['raw_max']}")
                print(f"         -> implied (prev - curr) in output pixels: ({st['prev_minus_curr_px_out'][0]:+.2f}, {st['prev_minus_curr_px_out'][1]:+.2f})")
                fr["mv_implied"] = st["prev_minus_curr_px_out"]
                save_mvec_png(fr["mvecs"], fr["prefix"] + "mvecs.png", ngx)
            if "depth" in fr:
                lo, hi = save_depth_png(fr["depth"], fr["prefix"] + "depth.png")
                print(f"frame {k} depth range: {lo:.6f} .. {hi:.6f}")

        scale, max_shift = 2, 24
        for k in range(1, len(frames)):
            prev_r = to_gray_small(frames[k - 1]["real"], scale)
            cur_r = to_gray_small(frames[k]["real"], scale)
            gen = to_gray_small(frames[k]["generated"], scale)
            rx, ry, re_ = global_shift(prev_r, cur_r, max_shift)
            gx, gy, ge = global_shift(prev_r, gen, max_shift)
            hx, hy, he = global_shift(gen, cur_r, max_shift)
            rx, ry, gx, gy, hx, hy = (v * scale for v in (rx, ry, gx, gy, hx, hy))
            print(f"frame {k}: shift R{k-1}->R{k} = ({rx:+.1f}, {ry:+.1f}) px   [err {re_:.2f}]")
            print(f"         shift R{k-1}->G{k} = ({gx:+.1f}, {gy:+.1f}) px   [err {ge:.2f}]")
            print(f"         shift G{k}->R{k}   = ({hx:+.1f}, {hy:+.1f}) px   [err {he:.2f}]")
            if abs(rx) > 1.0:
                print(f"         generated frame temporal position (x): {gx / rx:.2f}  (0.5 = correct midpoint, 0 = previous frame, 1 = current frame)")
            if abs(ry) > 1.0:
                print(f"         generated frame temporal position (y): {gy / ry:.2f}")
            if "mv_implied" in frames[k]:
                mx, my = frames[k]["mv_implied"]
                print(f"         motion vectors say prev - curr = ({mx:+.1f}, {my:+.1f}) px; image says ({-rx:+.1f}, {-ry:+.1f}) px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
