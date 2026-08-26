#!/usr/bin/env python3
"""
RKNN quantization strategy comparison script.

Compares three strategies on UltraFace-RFB-320 (detect) and MobileFaceNet (feature):
  - fp16   : no quantization, weights/activations in FP16
  - w8a8   : full INT8 quantization (weights INT8, activations INT8)
  - hybrid : mixed quantization (RKNN auto proposal + output layers kept FP32)

Output: accuracy table (cosine similarity, MSE, max error), model size, build time.
"""
import os
import sys
import argparse
import tempfile
import shutil
import time
import glob
import yaml
import numpy as np

try:
    from rknn.api import RKNN
except ImportError:
    print("Error: rknn-toolkit2 not installed, run: pip install rknn-toolkit2")
    sys.exit(1)

try:
    import onnxruntime as ort
except ImportError:
    print("Warning: onnxruntime not installed, accuracy comparison unavailable")
    ort = None

# ============================================================
# Model config
# ============================================================
MODELS = {
    'detect': {
        'name': 'UltraFace-RFB-320 (face detection)',
        'onnx': 'version-RFB-320.onnx',
        'input_size': (240, 320),   # H x W
        'mean_values': [[127, 127, 127]],
        'std_values': [[128, 128, 128]],
        'calib_count': 50,
        'eval_count': 20,
    },
    'feature': {
        'name': 'MobileFaceNet (face feature extraction)',
        'onnx': 'mobilefacenet.onnx',
        'input_size': (112, 112),
        'mean_values': [[127.5, 127.5, 127.5]],
        'std_values': [[127.5, 127.5, 127.5]],  # (pixel-127.5)/127.5 -> [-1,1]
        'calib_count': 100,
        'eval_count': 20,
    },
}

QUANT_MODES = ['fp16', 'w8a8', 'hybrid']


def _make_preprocessed_input(cfg, batch=1):
    """Generate a single preprocessed NCHW float input."""
    h, w = cfg['input_size']
    ch = 3
    if batch == 1:
        data = np.random.randint(0, 256, (ch, h, w)).astype(np.float32)
    else:
        data = np.random.randint(0, 256, (batch, ch, h, w)).astype(np.float32)
    mean = np.array(cfg['mean_values'][0], dtype=np.float32).reshape(-1, 1, 1)
    std = np.array(cfg['std_values'][0], dtype=np.float32).reshape(-1, 1, 1)
    data = (data - mean) / std
    return data


def _make_hwc_uint8_input(cfg):
    """Generate HWC uint8 input for RKNN inference."""
    h, w = cfg['input_size']
    return np.random.randint(0, 256, (h, w, 3), dtype=np.uint8)


def generate_calibration_data(work_dir, cfg, count):
    """Generate NCHW float32 calibration data, return dataset.txt path.

    Note: calibration data must be raw uint8 [0,255], NOT pre-normalized!
    RKNN build(quantization=True) applies config(mean,std) internally;
    pre-normalized data causes double normalization, breaking quantization scale/zero_point.
    """
    h, w = cfg['input_size']
    dataset_txt = os.path.join(work_dir, 'dataset.txt')
    lines = []
    for i in range(count):
        # Raw uint8 [0,255] as float32, do NOT normalize
        data = np.random.randint(0, 256, (1, 3, h, w)).astype(np.float32)
        fname = f'calib_{i:04d}.npy'
        fpath = os.path.join(work_dir, fname)
        np.save(fpath, data)
        lines.append(fpath)
    with open(dataset_txt, 'w') as f:
        f.write('\n'.join(lines))
    return dataset_txt


def onnx_infer(onnx_path, cfg, inputs_nchw):
    """ONNX FP32 inference, input is NCHW float32."""
    if ort is None:
        return None
    sess = ort.InferenceSession(onnx_path)
    input_name = sess.get_inputs()[0].name
    return sess.run(None, {input_name: inputs_nchw})


def rknn_infer_with_object(rknn, inputs_hwc_list, perf=False):
    """Run PC simulator inference on an already-built + init_runtime'd rknn object."""
    outputs_list = []
    latencies = []
    for inp in inputs_hwc_list:
        t0 = time.time()
        outs = rknn.inference(inputs=[inp])
        t1 = time.time()
        outputs_list.append(outs)
        latencies.append((t1 - t0) * 1000.0)
    avg_latency = np.mean(latencies) if latencies else 0.0
    return outputs_list, avg_latency


def _rknn_build_common(cfg, quant='w8a8'):
    """Create RKNN object and config, return rknn instance."""
    rknn = RKNN(verbose=False)
    ret = rknn.config(
        mean_values=cfg['mean_values'],
        std_values=cfg['std_values'],
        quantized_dtype='w8a8',
        target_platform='rk3568',
        optimization_level=3,
    )
    if ret != 0:
        print(f"  config failed: {ret}")
        rknn.release()
        return None
    ret = rknn.load_onnx(model=cfg['onnx'])
    if ret != 0:
        print(f"  load_onnx failed: {ret}")
        rknn.release()
        return None
    return rknn


def convert_fp16(cfg, out_rknn):
    """FP16 no quantization, return (success, rknn_obj)."""
    print(f"  [fp16] building...")
    rknn = RKNN(verbose=False)
    ret = rknn.config(
        mean_values=cfg['mean_values'],
        std_values=cfg['std_values'],
        target_platform='rk3568',
        optimization_level=3,
    )
    if ret != 0:
        print(f"    config failed: {ret}")
        return False, None
    ret = rknn.load_onnx(model=cfg['onnx'])
    if ret != 0:
        print(f"    load_onnx failed: {ret}")
        return False, None
    ret = rknn.build(do_quantization=False)
    if ret != 0:
        print(f"    build failed: {ret}")
        return False, None
    ret = rknn.export_rknn(out_rknn)
    if ret != 0:
        print(f"    export failed: {ret}")
        return False, None
    return True, rknn


def convert_w8a8(cfg, out_rknn, work_dir):
    """Full INT8 quantization, return (success, rknn_obj)."""
    print(f"  [w8a8] building...")
    rknn = _rknn_build_common(cfg)
    if rknn is None:
        return False, None
    dataset_txt = generate_calibration_data(work_dir, cfg, cfg['calib_count'])
    ret = rknn.build(do_quantization=True, dataset=dataset_txt)
    if ret != 0:
        print(f"    build failed: {ret}")
        return False, None
    ret = rknn.export_rknn(out_rknn)
    if ret != 0:
        print(f"    export failed: {ret}")
        return False, None
    return True, rknn


# Sensitive operator name patterns (matched against ONNX/RKNN op names; matched layers kept as float16)
SENSITIVE_PATTERNS = [
    'softmax', 'Softmax', 'SOFTMAX',
    'sigmoid', 'Sigmoid', 'SIGMOID',
    'tanh', 'Tanh', 'TANH',
    'layernorm', 'LayerNorm', 'layer_norm',
    'pow', 'Pow', 'sqr', 'Sqr', 'sqrt', 'Sqrt', 'log', 'Log', 'exp', 'Exp',
    # MobileFaceNet specific: GDConv (global depthwise separable conv) is sensitive to quantization
    'gdconv', 'GDConv',
    # Conv layers near output head (MobileFaceNet: conv3 is the 1x1 conv before output)
    'conv3', 'conv2/conv2.0', 'conv2/conv2.2',
]


def _is_sensitive_layer(name):
    """Check whether a layer name is a sensitive operator.

    Note: 'output' matches only the exact final output (output0/output),
    not xxx_output_0 (every layer has an output suffix; matching all would protect every layer).
    """
    name_lower = name.lower()
    for pat in SENSITIVE_PATTERNS:
        if pat.lower() in name_lower:
            return True
    # exact match for final output layer (not intermediate _output_0 suffix)
    if name_lower in ('output0', 'output', 'output0_int8'):
        return True
    return False


def _edit_hybrid_cfg(cfg_path, keep_fp32_last_n=3, protect_sensitive=True):
    """Read step1-generated .quantization.cfg, protect sensitive layers + keep last N layers as float16.

    Strategy:
      1. Iterate all dtype==int8 layers in quantize_parameters.
      2. If layer name matches sensitive operator pattern -> add to custom_quantize_layers as float16.
      3. Keep last keep_fp32_last_n layers as float16 (near output head).

    Note: only set via custom_quantize_layers, do NOT modify quantize_parameters directly.
    """
    with open(cfg_path, 'r') as f:
        doc = yaml.safe_load(f)

    params = doc.get('quantize_parameters', {})
    custom = doc.get('custom_quantize_layers', {})
    if custom is None:
        custom = {}

    # Collect quantizable layer names (exclude input and -rs/__float16 internal suffixes)
    # Note: output0/output0_int8 must be kept, as step2 may force outputs to int8
    all_keys = list(params.keys())
    quantizable = [k for k in all_keys if k not in ('input', 'input0', 'input_int8')
                   and not k.endswith('-rs') and not k.endswith('__float16')]

    # detect-class models use numeric IDs or numeric-suffixed internal names (e.g. '465', '479_2mul_i1')
    # step2 only accepts operand names from proposal; manually adding numeric IDs raises "Invalid operands name"
    # Only manually protect models using operator names (e.g. feature/mobilefacenet)
    has_numeric_keys = any(isinstance(k, int) or (isinstance(k, str) and k.isdigit())
                           for k in quantizable)
    if has_numeric_keys:
        print(f"  [hybrid-cfg] numeric ID naming detected (detect-class), using proposal only, no manual protection")
        return True

    protected_count = 0

    # Strategy 1: protect sensitive operator layers
    if protect_sensitive:
        for k in quantizable:
            if k in custom:
                continue  # already protected by proposal
            if _is_sensitive_layer(str(k)):
                custom[str(k)] = 'float16'
                protected_count += 1

    # Strategy 2: protect last N layers (near output head)
    last_n = quantizable[-keep_fp32_last_n:] if keep_fp32_last_n > 0 else []
    for k in last_n:
        if k not in custom:
            custom[str(k)] = 'float16'
            protected_count += 1

    doc['custom_quantize_layers'] = custom
    with open(cfg_path, 'w') as f:
        yaml.safe_dump(doc, f, sort_keys=False)
    print(f"  [hybrid-cfg] protected {protected_count} sensitive/output layers (of {len(quantizable)} quantizable layers)")
    return True


def convert_hybrid(cfg, out_rknn, work_dir, keep_fp32_last_n=3):
    """Hybrid quantization: step1 auto proposal + sensitive layer protection + output layer protection, return (success, rknn_obj)."""
    print(f"  [hybrid] step1 generating proposal...")
    rknn = _rknn_build_common(cfg)
    if rknn is None:
        return False, None

    dataset_txt = generate_calibration_data(work_dir, cfg, cfg['calib_count'])
    ret = rknn.hybrid_quantization_step1(
        dataset=dataset_txt,
        proposal=True,
        proposal_dataset_size=min(cfg['calib_count'], 20),
    )
    if ret != 0:
        print(f"    step1 failed: {ret}")
        return False, None
    rknn.release()

    base = os.path.splitext(cfg['onnx'])[0]
    model_path = base + '.model'
    data_path = base + '.data'
    cfg_path = base + '.quantization.cfg'
    if not all(os.path.exists(p) for p in [model_path, data_path, cfg_path]):
        print(f"    step1 output files not found: {model_path}, {data_path}, {cfg_path}")
        return False, None

    # protect sensitive operators + layers near output head
    print(f"  [hybrid] protecting sensitive layers (Softmax/Sigmoid/LayerNorm/GDConv/output head)...")
    _edit_hybrid_cfg(cfg_path, keep_fp32_last_n, protect_sensitive=True)

    print(f"  [hybrid] step2 building hybrid quantized model...")
    rknn2 = RKNN(verbose=False)
    ret = rknn2.hybrid_quantization_step2(
        model_input=model_path,
        data_input=data_path,
        model_quantization_cfg=cfg_path,
    )
    if ret != 0:
        print(f"    step2 failed: {ret}")
        return False, None
    ret = rknn2.export_rknn(out_rknn)
    if ret != 0:
        print(f"    export failed: {ret}")
        return False, None
    return True, rknn2


def evaluate_model(onnx_path, rknn, cfg):
    """Compare ONNX FP32 output accuracy with a built RKNN object."""
    if rknn is None:
        return None

    ret = rknn.init_runtime()
    if ret != 0:
        print(f"    init_runtime (simulator) failed: {ret}")
        return None

    results = []
    for _ in range(cfg['eval_count']):
        # Generate same uint8 HWC data so ONNX and RKNN process the same image
        hwc = _make_hwc_uint8_input(cfg)
        # ONNX needs NCHW float + normalization (matching RKNN internal mean/std)
        nchw = hwc.transpose(2, 0, 1)[np.newaxis, ...].astype(np.float32)
        mean = np.array(cfg['mean_values'][0], dtype=np.float32).reshape(-1, 1, 1)
        std = np.array(cfg['std_values'][0], dtype=np.float32).reshape(-1, 1, 1)
        nchw = (nchw - mean) / std

        onnx_outs = onnx_infer(onnx_path, cfg, nchw)
        if onnx_outs is None:
            rknn.release()
            return None
        rknn_outs, _ = rknn_infer_with_object(rknn, [hwc])
        rknn_outs = rknn_outs[0]

        pair_results = []
        for o, r in zip(onnx_outs, rknn_outs):
            o_f = o.flatten().astype(np.float64)
            r_f = r.flatten().astype(np.float64)
            dot = np.dot(o_f, r_f)
            norm_o = np.linalg.norm(o_f)
            norm_r = np.linalg.norm(r_f)
            cos = dot / (norm_o * norm_r + 1e-10)
            diff = o_f - r_f
            mse = np.mean(diff ** 2)
            max_err = np.max(np.abs(diff))
            mean_err = np.mean(np.abs(diff))
            pair_results.append((cos, mse, max_err, mean_err))
        results.append(pair_results)

    rknn.release()

    # average
    avg = []
    n_pairs = len(results[0])
    for p in range(n_pairs):
        cos_avg = np.mean([r[p][0] for r in results])
        mse_avg = np.mean([r[p][1] for r in results])
        max_avg = np.mean([r[p][2] for r in results])
        mean_avg = np.mean([r[p][3] for r in results])
        avg.append({'cosine': cos_avg, 'mse': mse_avg, 'max_err': max_avg, 'mean_err': mean_avg})
    return avg


def latency_bench(rknn, cfg, runs=50):
    """Simple latency benchmark."""
    if rknn is None:
        return None
    ret = rknn.init_runtime()
    if ret != 0:
        return None
    _, avg = rknn_infer_with_object(rknn, [_make_hwc_uint8_input(cfg) for _ in range(runs)], perf=True)
    rknn.release()
    return avg


def run_compare(model_keys, quant_modes, keep_fp32_last_n=3):
    all_results = []
    work_root = tempfile.mkdtemp(prefix='quant_compare_')
    print(f"Work dir: {work_root}\n")

    try:
        for key in model_keys:
            cfg = MODELS[key]
            onnx_path = cfg['onnx']
            if not os.path.exists(onnx_path):
                print(f"Skip {cfg['name']}: ONNX not found {onnx_path}")
                continue

            print("=" * 70)
            print(f"Model: {cfg['name']}")
            print(f"ONNX: {onnx_path}")
            print("=" * 70)

            for mode in quant_modes:
                work_dir = os.path.join(work_root, f'{key}_{mode}')
                os.makedirs(work_dir, exist_ok=True)
                out_rknn = f'{key}_{mode}.rknn'

                t0 = time.time()
                if mode == 'fp16':
                    ok, rknn_obj = convert_fp16(cfg, out_rknn)
                elif mode == 'w8a8':
                    ok, rknn_obj = convert_w8a8(cfg, out_rknn, work_dir)
                elif mode == 'hybrid':
                    ok, rknn_obj = convert_hybrid(cfg, out_rknn, work_dir, keep_fp32_last_n)
                else:
                    ok, rknn_obj = False, None
                build_time = time.time() - t0

                size_kb = os.path.getsize(out_rknn) / 1024.0 if ok and os.path.exists(out_rknn) else 0.0

                print(f"\n  [{mode}] build result: {'OK' if ok else 'FAIL'}")
                print(f"  model file: {out_rknn}")
                print(f"  model size: {size_kb:.1f} KB")
                print(f"  build time: {build_time:.1f} s")

                eval_res = None
                latency = None
                if ok and rknn_obj is not None:
                    eval_res = evaluate_model(onnx_path, rknn_obj, cfg)
                    # evaluate_model already releases internally; rebuilding for latency via file path is impractical,
                    # so latency is set to None to avoid inaccurate double init_runtime overhead on the simulator.
                    if eval_res:
                        for idx, m in enumerate(eval_res):
                            print(f"  output[{idx}] cosine: {m['cosine']:.6f}, MSE: {m['mse']:.8f}, max err: {m['max_err']:.6f}")
                    print("  avg inference latency (50 runs): - (not meaningful on PC simulator)")

                all_results.append({
                    'model': cfg['name'],
                    'mode': mode,
                    'ok': ok,
                    'size_kb': size_kb,
                    'build_time': build_time,
                    'latency_ms': latency,
                    'eval': eval_res,
                })

                # Clean up step1 intermediate files
                base = os.path.splitext(onnx_path)[0]
                for ext in ['.model', '.data', '.quantization.cfg']:
                    tmp = base + ext
                    if os.path.exists(tmp):
                        os.remove(tmp)
                print()
    finally:
        shutil.rmtree(work_root, ignore_errors=True)

    return all_results


def print_summary(results):
    print("\n" + "=" * 100)
    print("Quantization strategy comparison summary")
    print("=" * 100)
    print(f"{'Model':<40} {'Quant':<8} {'Size(KB)':<12} {'Build(s)':<10} {'Lat(ms)':<10} {'cosine':<10} {'MSE':<12}")
    print("-" * 100)
    for r in results:
        name = r['model'].split('(')[0].strip()
        mode = r['mode']
        if not r['ok']:
            print(f"{name:<40} {mode:<8} {'FAIL':<12} {'-':<10} {'-':<10} {'-':<10} {'-':<12}")
            continue
        size = f"{r['size_kb']:.1f}"
        bt = f"{r['build_time']:.1f}"
        lat = f"{r['latency_ms']:.2f}" if r['latency_ms'] is not None else '-'
        if r['eval']:
            cos = f"{r['eval'][0]['cosine']:.4f}"
            mse = f"{r['eval'][0]['mse']:.6f}"
        else:
            cos = '-'
            mse = '-'
        print(f"{name:<40} {mode:<8} {size:<12} {bt:<10} {lat:<10} {cos:<10} {mse:<12}")
    print("=" * 100)


def main():
    parser = argparse.ArgumentParser(description='RKNN quantization strategy comparison (detect/feature)')
    parser.add_argument('--models', nargs='+', choices=list(MODELS.keys()) + ['all'],
                        default=['all'], help='models to compare')
    parser.add_argument('--modes', nargs='+', choices=QUANT_MODES + ['all'],
                        default=['all'], help='quantization strategies')
    parser.add_argument('--keep-fp32-last-n', type=int, default=3,
                        help='number of FP32 layers kept near output in hybrid mode (default 5)')
    args = parser.parse_args()

    model_keys = list(MODELS.keys()) if 'all' in args.models else args.models
    quant_modes = QUANT_MODES if 'all' in args.modes else args.modes

    print(f"Models to compare: {model_keys}")
    print(f"Quantization strategies to compare: {quant_modes}")

    results = run_compare(model_keys, quant_modes, args.keep_fp32_last_n)
    print_summary(results)


if __name__ == '__main__':
    main()
