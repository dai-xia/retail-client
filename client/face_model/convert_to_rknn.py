#!/usr/bin/env python3
"""
RKNN model conversion script (full AI pipeline).

Models:
  detect    - UltraFace-RFB-320  face detection
  landmark  - PFLD-106           face landmarks (106 points, use first 5 for alignment)
  antispoof - MiniFASNet          anti-spoofing (photo attack defense)
  feature   - MobileFaceNet      face feature extraction (128-dim)
  vad       - Silero-VAD         voice activity detection

Usage:
  python3 convert_to_rknn.py --all
  python3 convert_to_rknn.py --detect --antispoof
  python3 convert_to_rknn.py --all --quant w8a8
"""
import sys
import os
import argparse
import tempfile
import shutil
import numpy as np

try:
    from rknn.api import RKNN
except ImportError:
    print("Error: rknn-toolkit2 not installed")
    print("Install with: pip install rknn-toolkit2")
    sys.exit(1)

# ============================================================
# Model config (ONNX input -> RKNN INT8 output)
# ============================================================
MODELS = {
    'detect': {
        'name': 'UltraFace-RFB-320 (face detection)',
        'onnx': 'version-RFB-320.onnx',
        'rknn': 'ultraface.rknn',
        'input_size': (240, 320),   # H x W
        'mean_values': [[127, 127, 127]],
        'std_values': [[128, 128, 128]],
        'calib_count': 50,
        'description': '320x240 input, scores+boxes output, edge device preferred',
    },
    'landmark': {
        'name': 'PFLD-106 (face landmarks)',
        'onnx': 'pfld_106.onnx',
        'rknn': 'pfld_106.rknn',
        'input_size': (112, 112),   # H x W
        'mean_values': [[127.5, 127.5, 127.5]],
        'std_values': [[127.5, 127.5, 127.5]],  # (pixel-127.5)/127.5 -> [-1,1]
        'calib_count': 50,
        'description': '106-point landmarks, use first 5 (eyes/nose/mouth) for face alignment affine transform',
    },
    'antispoof': {
        'name': 'MiniFASNet (anti-spoofing)',
        'onnx': 'miniFASNet.onnx',
        'rknn': 'miniFASNet.rknn',
        'input_size': (80, 80),     # H x W (MiniFASNet standard input)
        'mean_values': [[127.5, 127.5, 127.5]],
        'std_values': [[127.5, 127.5, 127.5]],  # (pixel-127.5)/127.5 -> [-1,1]
        'calib_count': 50,
        'description': '0=real, 1=attack (photo/video), 80x80 lightweight input',
    },
    'feature': {
        'name': 'MobileFaceNet (feature extraction)',
        'onnx': 'mobilefacenet.onnx',
        'rknn': 'mobilefacenet.rknn',
        'input_size': (112, 112),
        'mean_values': [[127.5, 127.5, 127.5]],
        'std_values': [[127.5, 127.5, 127.5]],  # (pixel-127.5)/127.5 -> [-1,1]
        'calib_count': 100,
        'description': '128-dim face feature, cosine similarity comparison',
    },
    'vad': {
        'name': 'Silero-VAD (voice activity detection)',
        'onnx': 'silero_vad.onnx',
        'rknn': 'silero_vad.rknn',
        'input_size': (1, 512),     # 1x512 (30ms @ 16kHz)
        'mean_values': [[0]],
        'std_values': [[1]],
        'calib_count': 30,
        'description': 'Input PCM 16bit normalized float, output voice probability, replaces fixed-energy VAD',
        'input_fmt': 'NCHW_1D',     # special: 1D input, not image
    },
}


def generate_calibration_data(work_dir, input_size, count, mean, std, input_fmt=None):
    """Generate calibration dataset.

    Note: calibration data must be raw uint8 [0,255], NOT pre-normalized!
    RKNN build(quantization=True) applies config(mean,std) internally;
    pre-normalized data causes double normalization, breaking quantization scale/zero_point.
    """
    h, w = input_size
    dataset_txt = os.path.join(work_dir, 'dataset.txt')
    lines = []
    for i in range(count):
        if input_fmt == 'NCHW_1D':
            # 1D input: (1,1,1,W) voice signal, range [-1,1]
            data = np.random.uniform(-1.0, 1.0, (1, 1, h, w)).astype(np.float32)
        else:
            # Do NOT normalize; RKNN applies config(mean,std) internally.
            data = np.random.randint(0, 256, (1, 3, h, w)).astype(np.float32)

        fname = f'calib_{i:04d}.npy'
        fpath = os.path.join(work_dir, fname)
        np.save(fpath, data)
        lines.append(fpath)

    with open(dataset_txt, 'w') as f:
        f.write('\n'.join(lines))
    return dataset_txt


def convert_model(model_key, quant='w8a8'):
    """Convert a single model: ONNX -> RKNN INT8."""
    cfg = MODELS[model_key]
    onnx_path = cfg['onnx']
    rknn_path = cfg['rknn']

    if not os.path.exists(onnx_path):
        print(f"Error: ONNX file not found: {onnx_path}")
        print(f"  Run first: ./download_models.sh --{model_key}")
        return False

    out_dir = os.path.dirname(rknn_path) or '.'
    os.makedirs(out_dir, exist_ok=True)

    print("=" * 60)
    print(f"Model:  {cfg['name']}")
    print(f"ONNX:   {onnx_path}")
    print(f"RKNN:   {rknn_path}")
    print(f"Quant:  {quant}")
    h, w = cfg['input_size']
    ch = 1 if cfg.get('input_fmt') == 'NCHW_1D' else 3
    print(f"Input:  1x{ch}x{h}x{w}")
    print(f"Platform: rk3568")
    print(f"Desc:   {cfg['description']}")
    print("=" * 60)
    print()

    rknn = RKNN(verbose=False)

    # 1. config
    print(f"[1/4] Config...")
    ret = rknn.config(
        mean_values=cfg['mean_values'],
        std_values=cfg['std_values'],
        quantized_dtype=quant,
        target_platform='rk3568',
        optimization_level=3,
    )
    if ret != 0:
        print(f"  config failed: {ret}")
        rknn.release()
        return False
    print("  OK")

    # 2. load ONNX
    print(f"[2/4] Loading ONNX...")
    ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        print(f"  load_onnx failed: {ret}")
        rknn.release()
        return False
    print("  OK")

    # 3. build
    if quant in ('fp16',):
        print(f"[3/4] Building RKNN (FP16, no quantization)...")
        ret = rknn.build(do_quantization=False)
    else:
        print(f"[3/4] Building RKNN (INT8 quantization, {cfg['calib_count']} calibration samples)...")
        tmp_dir = tempfile.mkdtemp(prefix=f'rknn_calib_{model_key}_')
        try:
            dataset_txt = generate_calibration_data(
                tmp_dir,
                cfg['input_size'],
                cfg['calib_count'],
                cfg['mean_values'][0],
                cfg['std_values'][0],
                cfg.get('input_fmt'),
            )
            ret = rknn.build(
                do_quantization=True,
                dataset=dataset_txt,
            )
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    if ret != 0:
        print(f"  build failed: {ret}")
        rknn.release()
        return False
    print("  OK")

    # 4. export
    print(f"[4/4] Exporting RKNN...")
    ret = rknn.export_rknn(rknn_path)
    if ret != 0:
        print(f"  export failed: {ret}")
        rknn.release()
        return False

    size_kb = os.path.getsize(rknn_path) / 1024
    print(f"  OK ({size_kb:.1f} KB)")

    # 5. accuracy verification (optional)
    try:
        print(f"\n[Verify] model I/O info:")
        rknn2 = RKNN(verbose=False)
        ret = rknn2.load_rknn(rknn_path)
        if ret == 0:
            ret = rknn2.init_runtime()
            if ret == 0:
                h, w = cfg['input_size']
                ch = 1 if cfg.get('input_fmt') == 'NCHW_1D' else 3
                if ch == 1:
                    dummy = np.random.uniform(-1, 1, (h, w)).astype(np.float32)
                else:
                    dummy = np.random.randint(0, 256, (h, w, 3), dtype=np.uint8)
                outputs = rknn2.inference(inputs=[dummy])
                if outputs:
                    for idx, out in enumerate(outputs):
                        print(f"  output[{idx}]: shape={out.shape}, dtype={out.dtype}")
                rknn2.release()
    except Exception as e:
        print(f"  verification skipped: {e}")

    rknn.release()
    return True


def eval_quantization(model_key, quant='w8a8'):
    """
    Quantization accuracy comparison: FP32 (ONNX) vs INT8 (RKNN).

    Method:
      1. Same random input -> run ONNX and RKNN inference separately.
      2. Cosine similarity of outputs (1.0=identical, 0.0=orthogonal).
      3. MSE and max absolute error of outputs.
      4. Per-layer accuracy distribution to find layers with severe loss.

    Notes:
      - Symmetric quantization: zero_point=0, suits weights (symmetric distribution).
      - Asymmetric quantization: zero_point!=0, suits activations (all-positive after ReLU).
      - Common accuracy loss causes:
        a) Non-uniform activation distribution -> quantization bucket overflow.
        b) Error accumulation in deep networks -> per-layer analysis needed.
        c) Small values truncated -> QAT (quantization-aware training) fine-tuning.
    """
    import onnxruntime as ort

    cfg = MODELS[model_key]
    onnx_path = cfg['onnx']
    rknn_path = cfg['rknn']

    if not os.path.exists(onnx_path):
        print(f"[EVAL] ONNX file not found: {onnx_path}")
        return False
    if not os.path.exists(rknn_path):
        print(f"[EVAL] RKNN file not found: {rknn_path}, convert first")
        return False

    print("=" * 60)
    print(f"Quantization accuracy comparison: {cfg['name']}")
    print(f"  ONNX: {onnx_path}")
    print(f"  RKNN: {rknn_path} ({quant})")
    print("=" * 60)

    # 1. generate test input
    h, w = cfg['input_size']
    input_fmt = cfg.get('input_fmt')
    if input_fmt == 'NCHW_1D':
        test_input = np.random.uniform(-1.0, 1.0, (1, 1, h, w)).astype(np.float32)
    else:
        test_input = np.random.randint(0, 256, (h, w, 3), dtype=np.uint8)

    # 2. ONNX FP32 inference
    print("\n[1/2] ONNX FP32 inference...")
    try:
        sess = ort.InferenceSession(onnx_path)
        input_name = sess.get_inputs()[0].name
        onnx_input = test_input.astype(np.float32)
        if input_fmt != 'NCHW_1D':
            # ONNX expects NCHW format
            mean = np.array(cfg['mean_values'][0], dtype=np.float32)
            std = np.array(cfg['std_values'][0], dtype=np.float32)
            onnx_input = (onnx_input - mean) / std
            onnx_input = onnx_input.transpose(2, 0, 1)[np.newaxis, ...]  # HWC -> NCHW

        onnx_outputs = sess.run(None, {input_name: onnx_input})
        print(f"  output count: {len(onnx_outputs)}")
        for i, out in enumerate(onnx_outputs):
            print(f"  output[{i}]: shape={out.shape}, range=[{out.min():.4f}, {out.max():.4f}]")
    except Exception as e:
        print(f"  ONNX inference failed: {e}")
        return False

    # 3. RKNN INT8 inference
    print("\n[2/2] RKNN INT8 inference...")
    try:
        rknn = RKNN(verbose=False)
        ret = rknn.load_rknn(rknn_path)
        if ret != 0:
            print(f"  load RKNN failed")
            rknn.release()
            return False

        ret = rknn.init_runtime()
        if ret != 0:
            print(f"  init runtime failed")
            rknn.release()
            return False

        rknn_outputs = rknn.inference(inputs=[test_input])
        if not rknn_outputs:
            print(f"  RKNN inference no output")
            rknn.release()
            return False

        for i, out in enumerate(rknn_outputs):
            print(f"  output[{i}]: shape={out.shape}, range=[{out.min():.4f}, {out.max():.4f}]")
    except Exception as e:
        print(f"  RKNN inference failed: {e}")
        rknn.release()
        return False

    # 4. accuracy comparison
    print("\n" + "=" * 60)
    print("Accuracy comparison results:")
    print("=" * 60)

    for i in range(min(len(onnx_outputs), len(rknn_outputs))):
        onnx_out = onnx_outputs[i].flatten().astype(np.float64)
        rknn_out = rknn_outputs[i].flatten().astype(np.float64)

        dot = np.dot(onnx_out, rknn_out)
        norm_onnx = np.linalg.norm(onnx_out)
        norm_rknn = np.linalg.norm(rknn_out)
        cosine_sim = dot / (norm_onnx * norm_rknn + 1e-10)

        diff = onnx_out - rknn_out
        mse = np.mean(diff ** 2)
        max_abs_err = np.max(np.abs(diff))
        mean_abs_err = np.mean(np.abs(diff))

        # relative error (non-zero regions only)
        nonzero_mask = np.abs(onnx_out) > 1e-6
        if nonzero_mask.any():
            rel_err = np.mean(np.abs(diff[nonzero_mask] / onnx_out[nonzero_mask]))
        else:
            rel_err = 0.0

        print(f"\n  output[{i}]:")
        print(f"    cosine sim:  {cosine_sim:.6f}  {'OK' if cosine_sim > 0.99 else 'WARN <0.99'}")
        print(f"    MSE:         {mse:.8f}")
        print(f"    max abs err: {max_abs_err:.6f}")
        print(f"    mean abs err: {mean_abs_err:.6f}")
        print(f"    mean rel err: {rel_err*100:.2f}%")

        # accuracy assessment
        if cosine_sim > 0.995:
            print(f"    rating: excellent (minimal quantization loss, ready to deploy)")
        elif cosine_sim > 0.99:
            print(f"    rating: good (acceptable quantization loss)")
        elif cosine_sim > 0.95:
            print(f"    rating: fair (noticeable loss, check calibration data)")
        else:
            print(f"    rating: poor (severe loss, consider FP16 or QAT fine-tuning)")

    rknn.release()
    return True


def main():
    parser = argparse.ArgumentParser(description='RKNN model conversion tool (full AI pipeline)')
    parser.add_argument('--all', action='store_true', help='convert all models')
    parser.add_argument('--detect', action='store_true', help='face detection (UltraFace)')
    parser.add_argument('--landmark', action='store_true', help='face landmarks (PFLD-106)')
    parser.add_argument('--antispoof', action='store_true', help='anti-spoofing (MiniFASNet)')
    parser.add_argument('--feature', action='store_true', help='feature extraction (MobileFaceNet)')
    parser.add_argument('--vad', action='store_true', help='voice activity detection (Silero-VAD)')
    parser.add_argument('--quant', default='w8a8', choices=['w8a8', 'fp16'],
                        help='quantization: w8a8=INT8 (default), fp16=FP16 no quantization')
    parser.add_argument('--eval', action='store_true',
                        help='quantization accuracy comparison: FP32 (ONNX) vs INT8 (RKNN)')
    args = parser.parse_args()

    # --eval mode: accuracy comparison only, no conversion
    if args.eval:
        eval_models = []
        if args.all:
            eval_models = list(MODELS.keys())
        else:
            if args.detect:    eval_models.append('detect')
            if args.landmark:  eval_models.append('landmark')
            if args.antispoof: eval_models.append('antispoof')
            if args.feature:   eval_models.append('feature')
            if args.vad:       eval_models.append('vad')
        if not eval_models:
            eval_models = list(MODELS.keys())  # default: compare all
        print("Quantization accuracy comparison mode")
        for key in eval_models:
            eval_quantization(key, args.quant)
        return

    if not any([args.all, args.detect, args.landmark, args.antispoof, args.feature, args.vad]):
        parser.print_help()
        print("\nSpecify models to convert, e.g. --all or --detect --feature")
        return

    results = {}

    if args.all or args.detect:    results['detect']    = convert_model('detect', args.quant)
    if args.all or args.landmark:  results['landmark']  = convert_model('landmark', args.quant)
    if args.all or args.antispoof: results['antispoof'] = convert_model('antispoof', args.quant)
    if args.all or args.feature:   results['feature']   = convert_model('feature', args.quant)
    if args.all or args.vad:       results['vad']       = convert_model('vad', args.quant)

    print("\n" + "=" * 60)
    print("Conversion summary:")
    print("=" * 60)
    all_ok = True
    for key, ok in results.items():
        status = "OK" if ok else "FAIL"
        name = MODELS[key]['name']
        rknn_path = MODELS[key]['rknn']
        size_str = ""
        if ok and os.path.exists(rknn_path):
            size_str = f" ({os.path.getsize(rknn_path) / 1024:.1f} KB)"
        print(f"  [{status}] {name} -> {rknn_path}{size_str}")
        if not ok:
            all_ok = False

    if all_ok:
        print("\nAll models converted successfully!")
        print("Deploy .rknn files to device face_model/ directory to use NPU acceleration")
    else:
        print("\nSome models failed to convert, check error messages")
        sys.exit(1)


if __name__ == '__main__':
    main()
