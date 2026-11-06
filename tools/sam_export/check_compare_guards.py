#!/usr/bin/env python3
"""Small, model-free checks for the candidate-order acceptance gate."""

import tempfile
from pathlib import Path

import numpy as np

from compare_onnx_pytorch import compare_candidate_lists, load_yolo_defects


def main() -> None:
    masks = np.ones((3, 4, 4), dtype=np.uint8)
    cases = [
        ("same order", [0.9, 0.5, 0.1], [0.9, 0.5, 0.1], True, False, True),
        ("lower tie swap", [0.9, 0.5, 0.499], [0.9, 0.499, 0.5], True, True, True),
        ("top tie swap", [0.9, 0.899, 0.1], [0.899, 0.9, 0.1], True, True, False),
        ("lower tie cannot excuse best swap", [0.9, 0.5, 0.499], [0.499, 0.9, 0.5], False, False, False),
        ("non-tie swap", [0.9, 0.5, 0.1], [0.5, 0.9, 0.1], False, False, False),
    ]
    for name, pt, ox, passed, tied, selected in cases:
        result = compare_candidate_lists(pt, masks, ox, masks)
        assert (result["pass"], result["tieAdjusted"], result["selectedEqual"]) == (passed, tied, selected), name
        print(f"PASS [{name}]")
    assert not compare_candidate_lists([], masks[:0], [], masks[:0])["pass"]
    assert not compare_candidate_lists([0.9, 0.5, 0.1], masks, [0.9, 0.5], masks[:2])["pass"]
    print("PASS [empty and count mismatch]")

    with tempfile.TemporaryDirectory() as temp_dir:
        label = Path(temp_dir) / "sample.txt"
        label.write_text("0 0.1 0.1 0.9 0.1 0.9 0.9 0.1 0.9\n", encoding="utf-8")
        defects = load_yolo_defects(str(label), 100, 100)
        assert len(defects) == 1 and defects[0]["classId"] == 0
        assert 10 <= defects[0]["center"][0] <= 90 and 10 <= defects[0]["center"][1] <= 90
        for bad in ("0 0.1 0.2 0.3\n", "0 0.1 0.1 nan 0.2 0.3 0.3\n", "0 0.1 0.1 1.1 0.2 0.3 0.3\n"):
            label.write_text(bad, encoding="utf-8")
            try:
                load_yolo_defects(str(label), 100, 100)
            except ValueError:
                continue
            raise AssertionError(f"malformed label accepted: {bad!r}")
        label.write_bytes(b"\xff")
        try:
            load_yolo_defects(str(label), 100, 100)
        except ValueError:
            pass
        else:
            raise AssertionError("non-UTF-8 label accepted")
        label.unlink()
        try:
            load_yolo_defects(str(label), 100, 100)
        except ValueError:
            pass
        else:
            raise AssertionError("missing label accepted")
    print("PASS [YOLO-seg prompt and malformed labels]")


if __name__ == "__main__":
    main()
