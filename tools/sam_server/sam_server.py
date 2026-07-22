"""
DeepLux SAM FastAPI server.

Default mode requires a real SAM checkpoint through DEEPLUX_SAM_MODEL.
Set DEEPLUX_SAM_STUB=1 only for Qt integration tests without a model.
"""
import asyncio
import base64
import hashlib
import io
import json
import os
from typing import Dict, List, Optional

import numpy as np
from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI(title="DeepLux SAM Server", version="0.2.0")

STUB_MODE = os.environ.get("DEEPLUX_SAM_STUB", "0") == "1"
MODEL_PATH = os.environ.get("DEEPLUX_SAM_MODEL", "")
MODEL_TYPE = os.environ.get("DEEPLUX_SAM_MODEL_TYPE", "vit_b")
MODEL_NAME = "sam-stub-v1" if STUB_MODE else f"sam-{MODEL_TYPE}"

embeddings: Dict[str, Dict] = {}
_predictor = None
_active_embedding_id: Optional[str] = None
_load_error: Optional[str] = None

# Fix 4: 序列化 predict 请求，防止并发访问 SamPredictor
_predict_lock = asyncio.Lock()


class SetImageRequest(BaseModel):
    image_path: str


class PredictRequest(BaseModel):
    embedding_id: str
    points_pos: List[List[float]] = []
    points_neg: List[List[float]] = []
    box: Optional[List[float]] = None


class UnloadRequest(BaseModel):
    embedding_id: str


def _image_hash(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def _load_image(path: str):
    from PIL import Image

    with Image.open(path) as img:
        rgb = img.convert("RGB")
        return np.array(rgb), rgb.size


def _ensure_predictor():
    global _predictor, _load_error
    if STUB_MODE:
        return None
    if _predictor is not None:
        return _predictor
    if not MODEL_PATH or not os.path.exists(MODEL_PATH):
        _load_error = "DEEPLUX_SAM_MODEL is missing or does not exist"
        raise RuntimeError(_load_error)

    try:
        import torch
        from segment_anything import SamPredictor, sam_model_registry

        sam = sam_model_registry[MODEL_TYPE](checkpoint=MODEL_PATH)
        device = os.environ.get("DEEPLUX_SAM_DEVICE") or ("cuda" if torch.cuda.is_available() else "cpu")
        sam.to(device=device)
        _predictor = SamPredictor(sam)
        _load_error = None
        return _predictor
    except Exception as exc:  # dependency/model errors must be visible to Qt
        _load_error = str(exc)
        raise


def _mask_to_bbox(mask: np.ndarray):
    ys, xs = np.where(mask > 0)
    if len(xs) == 0 or len(ys) == 0:
        return [0.0, 0.0, 0.0, 0.0]
    x1, x2 = float(xs.min()), float(xs.max())
    y1, y2 = float(ys.min()), float(ys.max())
    return [x1, y1, x2 - x1 + 1.0, y2 - y1 + 1.0]


def _mask_to_polygon(mask: np.ndarray, bbox):
    try:
        import cv2

        contours, _ = cv2.findContours(mask.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if contours:
            contour = max(contours, key=cv2.contourArea)
            eps = max(1.0, 0.002 * cv2.arcLength(contour, True))
            approx = cv2.approxPolyDP(contour, eps, True).reshape(-1, 2)
            return [[float(x), float(y)] for x, y in approx]
    except Exception:
        pass

    x, y, w, h = bbox
    return [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]


def _encode_rle(mask: np.ndarray) -> str:
    h, w = mask.shape[:2]
    pixels = mask.astype(np.uint8).T.flatten()
    counts = []
    last = 0
    run = 0
    for pixel in pixels:
        value = 1 if pixel else 0
        if value == last:
            run += 1
        else:
            counts.append(run)
            run = 1
            last = value
    counts.append(run)
    return json.dumps({"size": [h, w], "counts": counts}, separators=(",", ":"))


def _encode_mask_png_base64(mask: np.ndarray) -> str:
    """Encode a binary mask as a base64-encoded RGBA PNG for easy QImage loading on the C++ side."""
    try:
        from PIL import Image

        h, w = mask.shape[:2]
        rgba = np.zeros((h, w, 4), dtype=np.uint8)
        # Use a semi-transparent cyan for mask pixels, fully transparent for the rest.
        rgba[mask > 0] = [6, 182, 212, 90]
        rgba[mask == 0] = [0, 0, 0, 0]
        img = Image.fromarray(rgba, mode="RGBA")
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        return base64.b64encode(buf.getvalue()).decode("ascii")
    except Exception:
        return ""


def _stub_prediction(req: PredictRequest, size):
    w, h = size
    if req.box is not None and len(req.box) >= 4:
        x, y, bw, bh = req.box[0], req.box[1], req.box[2], req.box[3]
    elif req.points_pos:
        cx, cy = req.points_pos[0]
        x, y, bw, bh = cx - 10, cy - 10, 20, 20
    else:
        x, y, bw, bh = 0, 0, min(50, w), min(50, h)
    polygon = [[x, y], [x + bw, y], [x + bw, y + bh], [x, y + bh]]
    # Fix P1-5: 返回整图大小的 mask（和真实 SAM 一致），而非 bbox 大小
    mask = np.zeros((int(h), int(w)), dtype=np.uint8)
    x1, y1 = max(0, int(x)), max(0, int(y))
    x2, y2 = min(int(w), int(x + bw)), min(int(h), int(y + bh))
    if x2 > x1 and y2 > y1:
        mask[y1+1:y2-1, x1+1:x2-1] = 1
    return {
        "mask_rle": "",
        "mask_png_base64": _encode_mask_png_base64(mask),
        "bbox": [x, y, bw, bh],
        "polygon": polygon,
        "score": 0.95,
        "model_name": MODEL_NAME,
    }


@app.get("/health")
def health():
    if STUB_MODE:
        return {"status": "ready", "model_name": MODEL_NAME, "mode": "stub"}
    try:
        _ensure_predictor()
        return {"status": "ready", "model_name": MODEL_NAME, "mode": "sam"}
    except Exception:
        return {"status": "error", "model_name": MODEL_NAME, "error": _load_error or "SAM load failed"}


@app.post("/set_image")
async def set_image(req: SetImageRequest):
    global _active_embedding_id
    if not os.path.exists(req.image_path):
        return {"status": "error", "error": "file not found"}

    image, size = _load_image(req.image_path)
    eid = _image_hash(req.image_path)[:16]
    embeddings[eid] = {"path": req.image_path, "size": [size[0], size[1]], "image": image}

    if not STUB_MODE:
        # Fix P0-3: set_image 也使用预测锁，防止和 predict 并发操作 SamPredictor
        async with _predict_lock:
            predictor = _ensure_predictor()
            predictor.set_image(image)
            _active_embedding_id = eid

    return {"embedding_id": eid, "image_size": [size[0], size[1]], "model_name": MODEL_NAME}


@app.post("/predict")
async def predict(req: PredictRequest):
    global _active_embedding_id
    meta = embeddings.get(req.embedding_id)
    if meta is None:
        return {"status": "invalid_embedding"}

    if STUB_MODE:
        return _stub_prediction(req, meta["size"])

    # Fix 4: 序列化推理，防止并发访问 SamPredictor
    async with _predict_lock:
        predictor = _ensure_predictor()
        if _active_embedding_id != req.embedding_id:
            predictor.set_image(meta["image"])
            _active_embedding_id = req.embedding_id

        point_coords = None
        point_labels = None
        points = req.points_pos + req.points_neg
        if points:
            point_coords = np.array(points, dtype=np.float32)
            point_labels = np.array([1] * len(req.points_pos) + [0] * len(req.points_neg), dtype=np.int32)

        box = None
        if req.box is not None and len(req.box) >= 4:
            x, y, w, h = req.box[:4]
            box = np.array([x, y, x + w, y + h], dtype=np.float32)

        masks, scores, _ = predictor.predict(
            point_coords=point_coords,
            point_labels=point_labels,
            box=box,
            multimask_output=True,
        )
        best = int(np.argmax(scores))
        mask = masks[best].astype(np.uint8)
        bbox = _mask_to_bbox(mask)
        polygon = _mask_to_polygon(mask, bbox)
        return {
            "mask_rle": _encode_rle(mask),
            "mask_png_base64": _encode_mask_png_base64(mask),
            "bbox": bbox,
            "polygon": polygon,
            "score": float(scores[best]),
            "model_name": MODEL_NAME,
        }


@app.post("/unload_image")
def unload_image(req: UnloadRequest):
    if req.embedding_id in embeddings:
        del embeddings[req.embedding_id]
        return {"ok": True}
    return {"ok": False, "error": "embedding not found"}


if __name__ == "__main__":
    import argparse
    import socket
    import uvicorn

    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0, help="Port (0=auto)")
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()

    port = args.port or int(os.environ.get("SAM_SERVER_PORT", "0"))

    # Fix P0-1: 在启动 uvicorn 之前用 socket 探测可用端口
    if port == 0:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind((args.host, 0))
        port = sock.getsockname()[1]
        sock.close()
        print(f"SAM_SERVER_PORT={port}", flush=True)

    uvicorn.run(app, host=args.host, port=port, log_level="info")
