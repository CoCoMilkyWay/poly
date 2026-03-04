from pathlib import Path
import shutil
import tempfile

import onnx
from onnxruntime.quantization import QuantType, quantize_dynamic
from onnxruntime.transformers import optimizer
from onnxruntime.transformers.fusion_options import FusionOptions
from optimum.onnxruntime import ORTModelForFeatureExtraction
from transformers import AutoTokenizer

# ===== Macros =====
MODEL_CHOICES = {
    "bge-base-en-v1.5": "BAAI/bge-base-en-v1.5",
    "bge-large-en-v1.5": "BAAI/bge-large-en-v1.5",
    "bge-small-en-v1.5": "BAAI/bge-small-en-v1.5",
    "qwen3-embedding-0.6b": "Qwen/Qwen3-Embedding-0.6B",
}
GRAPH_OPT_MODEL_TYPE = {
    "bge-base-en-v1.5": "bert",
    "bge-large-en-v1.5": "bert",
    "bge-small-en-v1.5": "bert",
}
MODEL_KEY = "bge-small-en-v1.5"
ENABLE_GRAPH_OPT = True
ENABLE_QUANT_INT8 = False
QUANT_PER_CHANNEL = True
FORCE_EXPORT_FROM_TORCH = False
# ==================

script_dir = Path(__file__).resolve().parent
onnx_dir = script_dir / "onnx"
onnx_dir.mkdir(parents=True, exist_ok=True)

assert MODEL_KEY in MODEL_CHOICES
model_id = MODEL_CHOICES[MODEL_KEY]

final_stem = MODEL_KEY
if ENABLE_QUANT_INT8:
    final_stem = f"{final_stem}__int8"
final_dir = onnx_dir / final_stem
if final_dir.exists():
    shutil.rmtree(final_dir)
final_dir.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="hf2onnx_", dir=str(onnx_dir)) as tmp_root:
    tmp_dir = Path(tmp_root)

    model = ORTModelForFeatureExtraction.from_pretrained(
        model_id,
        export=FORCE_EXPORT_FROM_TORCH,
    )
    tokenizer = AutoTokenizer.from_pretrained(model_id)

    model.save_pretrained(tmp_dir)
    tokenizer.save_pretrained(final_dir)

    onnx_files = sorted(tmp_dir.glob("*.onnx"))
    assert len(onnx_files) == 1
    raw_onnx = onnx_files[0]
    onnx.checker.check_model(str(raw_onnx))

    current_onnx = raw_onnx
    if ENABLE_GRAPH_OPT:
        assert MODEL_KEY in GRAPH_OPT_MODEL_TYPE
        model_type = GRAPH_OPT_MODEL_TYPE[MODEL_KEY]
        opt_options = FusionOptions(model_type)
        opt_options.enable_gelu_approximation = True
        optimized_model = optimizer.optimize_model(
            str(current_onnx),
            model_type=model_type,
            optimization_options=opt_options,
        )
        optimized_onnx = tmp_dir / "model_optimized.onnx"
        optimized_model.save_model_to_file(str(optimized_onnx))
        current_onnx = optimized_onnx

    final_onnx = final_dir / "model.onnx"
    if ENABLE_QUANT_INT8:
        quantize_dynamic(
            str(current_onnx),
            str(final_onnx),
            weight_type=QuantType.QInt8,
            per_channel=QUANT_PER_CHANNEL,
        )
    else:
        shutil.copyfile(current_onnx, final_onnx)

    onnx.checker.check_model(str(final_onnx))
