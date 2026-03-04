#!/usr/bin/env python3
import os

MODEL_PATH = "bge-small-en-v1.5/model.onnx"
CHUNK_SIZE = 45 * 1024 * 1024  # 50MB

os.chdir(os.path.dirname(os.path.abspath(__file__)))

with open(MODEL_PATH, "rb") as f:
    data = f.read()

for i, start in enumerate(range(0, len(data), CHUNK_SIZE)):
    chunk = data[start:start + CHUNK_SIZE]
    with open(f"{MODEL_PATH}.{i:02d}", "wb") as out:
        out.write(chunk)
    print(f"Written {MODEL_PATH}.{i:02d} ({len(chunk)} bytes)")

os.remove(MODEL_PATH)
print(f"Removed {MODEL_PATH}")
