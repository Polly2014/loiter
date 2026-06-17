"""Avatar compositor — combines 5 sprite layers into a 32×64 PNG for the big screen."""
import json
import hashlib
from pathlib import Path
from io import BytesIO
from PIL import Image

SPRITE_DIR = Path(__file__).parent / "sprite_layers"
MANIFEST = None
_cache: dict[str, bytes] = {}   # avatar_key → PNG bytes


def _load_manifest():
    global MANIFEST
    if MANIFEST is None:
        with open(SPRITE_DIR / "manifest.json") as f:
            MANIFEST = json.load(f)
    return MANIFEST


LAYER_ORDER = ["body", "eyes", "outfit", "hair", "accessory"]


def _variant_filename(layer: str, shape_idx: int, color_idx: int) -> str | None:
    """Get the PNG filename for a given layer/shape/color index."""
    manifest = _load_manifest()
    shapes = manifest.get(layer, [])
    if shape_idx < 0 or shape_idx >= len(shapes):
        return None
    shape_info = shapes[shape_idx]
    name = shape_info["name"]
    n_variants = shape_info["variants"]
    if n_variants == 0:
        return None
    cidx = color_idx % n_variants
    return f"{layer}_{name}_v{cidx}.png"


def compose_avatar(shape: list[int], color: list[int]) -> bytes:
    """Compose 5 layers into a 32×64 RGBA PNG. Returns PNG bytes (cached)."""
    key = f"{shape}:{color}"
    if key in _cache:
        return _cache[key]

    result = Image.new("RGBA", (32, 64), (0, 0, 0, 0))

    for li, layer in enumerate(LAYER_ORDER):
        si = shape[li] if li < len(shape) else 0
        ci = color[li] if li < len(color) else 0

        # hair/accessory shape=0 means "none" → skip
        if layer in ("hair", "accessory") and si == 0:
            continue

        # For hair/accessory, shape index in manifest is shifted by -1 (no "none" entry in PNGs)
        manifest_si = si
        if layer in ("hair", "accessory"):
            manifest_si = si - 1  # shape 0 = none (skipped above), shape 1 = first real

        fname = _variant_filename(layer, manifest_si, ci)
        if fname is None:
            continue
        fpath = SPRITE_DIR / fname
        if not fpath.exists():
            continue

        layer_img = Image.open(fpath).convert("RGBA")
        result = Image.alpha_composite(result, layer_img)

    buf = BytesIO()
    result.save(buf, format="PNG", optimize=True)
    png_bytes = buf.getvalue()
    _cache[key] = png_bytes
    return png_bytes


def avatar_cache_key(shape: list[int], color: list[int]) -> str:
    """Stable ETag for HTTP caching."""
    raw = f"{shape}:{color}".encode()
    return hashlib.md5(raw).hexdigest()[:12]
