# SAM1 ViT-B PyTorch reference masks

These 12 PNGs are independent reference outputs from segment_anything.SamPredictor
using sam_vit_b_01ec64.pth (SHA-256
ec2df62732614e57411cdcf32a23ffdf28910380d03139ee0f4fcbe91eb8c912).
The input is a 640x480 RGB uint8 image filled with [96, 110, 100]; pixels
x=200..279, y=150..209 are [255, 255, 255].

Call predictor.set_image(image), then predictor.predict with multimask_output=True
for each prompt below. Sort the three outputs by descending raw score and save
each boolean mask as a 0/255 grayscale PNG:

| Prefix | Positive point | Negative point | Box (xyxy) |
| --- | --- | --- | --- |
| point | (240, 180) | none | none |
| posneg | (240, 180) | (320, 250) | none |
| box | none | none | (100, 100, 300, 300) |
| mixed | (240, 180) | none | (100, 100, 300, 300) |

test_sam_onnx_inference checks every candidate's score within 0.01 and pixel
IoU against these masks at least 0.99. Running the native test does not
require a Python service.
