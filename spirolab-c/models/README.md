# Local inference models

Model weights are not included in this repository and are not downloaded by the app. Place your separately obtained weights in this directory, or pass `--model-dir DIR`:

- `u2netp.onnx`: small U2-Net subject segmentation, Apache-2.0.
- `det_500m.onnx` and `2d106det.onnx`: InsightFace buffalo_s detector and 106-point landmarks. Pretrained weights are restricted to non-commercial research use, as in the existing SpiroLab app.

These weights retain their upstream terms and are not covered by the application's MIT licence. Keep model weights local. A full `u2net.onnx`, if supplied, may be used in preference to the small model.

Sources used by the existing web application are recorded in [`server/Dockerfile`](../../server/Dockerfile):

- [U2-Net small weights](https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx).
- [InsightFace buffalo_s archive](https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_s.zip): extract only `det_500m.onnx` and `2d106det.onnx`, subject to the upstream model terms.

`SHA256SUMS` records the weights used for local validation. Without weights, gear drawing, text/image tracing, 3D and exports remain available; the relevant AI preparation operation reports a missing model.
