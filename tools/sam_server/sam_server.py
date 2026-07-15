"""
SAM FastAPI Server (stub)

第一期：返回固定 polygon/mask，用于 Qt 集成测试。
第二期将接入真实 SAM 模型。
"""
import os
import json
import hashlib
from typing import List, Optional, Dict

from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI(title="DeepLux SAM Server", version="0.1.0")

MODEL_NAME = "sam-stub-v1"

# 嵌入缓存：embedding_id → {path, hash, size}
embeddings: Dict[str, Dict] = {}


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
    try:
        with open(path, "rb") as f:
            return hashlib.md5(f.read()).hexdigest()
    except Exception:
        return "unknown"


def _image_size(path: str):
    try:
        from PIL import Image
        with Image.open(path) as img:
            return img.size  # (w, h)
    except Exception:
        return (0, 0)


@app.get("/health")
def health():
    return {"status": "ok", "model_name": MODEL_NAME}


@app.post("/set_image")
def set_image(req: SetImageRequest):
    path = req.image_path
    if not os.path.exists(path):
        return {"status": "error", "error": "file not found"}
    h = _image_hash(path)
    w, hgt = _image_size(path)
    eid = h[:16]
    embeddings[eid] = {"path": path, "hash": h, "size": [w, hgt]}
    return {
        "embedding_id": eid,
        "image_hash": h,
        "image_size": [w, hgt],
        "model_name": MODEL_NAME,
    }


@app.post("/predict")
def predict(req: PredictRequest):
    eid = req.embedding_id
    if eid not in embeddings:
        return {"status": "invalid_embedding"}

    meta = embeddings[eid]
    w, hgt = meta["size"]

    # 生成占位 polygon：使用 box 或第一个正点的简单矩形
    if req.box is not None and len(req.box) >= 4:
        x, y, bw, bh = req.box[0], req.box[1], req.box[2], req.box[3]
    elif req.points_pos:
        cx, cy = req.points_pos[0]
        x, y, bw, bh = cx - 10, cy - 10, 20, 20
    else:
        x, y, bw, bh = 0, 0, min(50, w), min(50, hgt)

    polygon = [
        [x, y],
        [x + bw, y],
        [x + bw, y + bh],
        [x, y + bh],
    ]
    bbox = [x, y, bw, bh]
    # 占位 RLE mask（空字符串表示无 mask）
    mask_rle = ""

    return {
        "mask_rle": mask_rle,
        "bbox": bbox,
        "polygon": polygon,
        "score": 0.95,
        "model_name": MODEL_NAME,
    }


@app.post("/unload_image")
def unload_image(req: UnloadRequest):
    eid = req.embedding_id
    if eid in embeddings:
        del embeddings[eid]
        return {"ok": True}
    return {"ok": False, "error": "embedding not found"}


if __name__ == "__main__":
    import uvicorn
    port = int(os.environ.get("SAM_SERVER_PORT", "8000"))
    uvicorn.run(app, host="127.0.0.1", port=port)
