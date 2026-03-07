#!/usr/bin/env python3
"""
[  7422/156357] 0x0c3aec5ec.. blk=  27473808 | Sports_Basketball               - Basketball_NBA_Game             (0.4439) | NBA: Who will win 76ers vs. Raptors, scheduled for April 23,
[  7423/156357] 0x0c3b65c7b.. blk=  75363333 | Politics_International_Election - Politics_LatAm_Election         (0.4170) | Will Rodrigo Paz Pereira win by 5~10%?
[  7424/156357] 0x0c3b783dc.. blk=  68605030 | Politics_Geopolitics            - Politics_Ceasefire              (0.2894) | Will Trump say 'retard' or 'retarded' during the 2025 State 
[  7425/156357] 0x0c3b9eb79.. blk=  72718852 | Crypto_Price                    - Crypto_SOL_Price                (0.3139) | Ethereum Up or Down - June 15, 10 AM ET
[  7426/156357] 0x0c3c8a0c0.. blk=  78230802 | Crypto_Market                   - Crypto_Exchange                 (0.3337) | Ceará SC vs. Fortaleza EC: O/U 2.5
"""

from sentence_transformers import SentenceTransformer
import json
import logging
import random
import time
from pathlib import Path

import duckdb
import numpy as np
import torch
import transformers
from tqdm import tqdm
transformers.logging.set_verbosity_error()
logging.getLogger("sentence_transformers").setLevel(logging.ERROR)
print("torch.cuda.is_available():", torch.cuda.is_available())
print("device count:", torch.cuda.device_count())

# macro: whether to include description text in embedding input
INCLUDE_DESCRIPTION = True
TAG_MD_REL_PATH = "core-backend/src/stage0/TAG.md"
SAMPLE = 200
RANDOM_SEED = 42


def load_stage0_db_path(repo_root: Path) -> Path:
    config_path = repo_root / "config.json"
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    db_rel = config["db_path_stage0"]
    db_path = (repo_root / db_rel).resolve()
    assert db_path.exists(), f"stage0 db not found: {db_path}"
    return db_path


def load_labels_with_comments(repo_root: Path) -> tuple[list[str], list[str]]:
    """返回 (标签名列表, 标签名+注释列表用于embedding)"""
    tag_md = (repo_root / TAG_MD_REL_PATH).resolve()
    assert tag_md.exists(), f"tag file not found: {tag_md}"

    label_names: list[str] = []
    label_texts: list[str] = []
    level1 = ""

    for raw_line in tag_md.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("## "):
            level1 = line[3:].strip()
            continue
        if line.startswith("- "):
            parts = line[2:].split("#", 1)
            level2 = parts[0].strip()
            comment = parts[1].strip() if len(parts) > 1 else ""
            assert level1, f"secondary tag without primary section: {level2}"
            assert level2, "empty secondary tag in TAG.md"

            full_name = f"{level1} - {level2}"
            label_names.append(full_name)
            # 用标签名+注释做embedding, 增强匹配
            embed_text = f"{level2} {comment}".strip()
            label_texts.append(embed_text)

    assert label_names, f"no labels parsed from: {tag_md}"
    return label_names, label_texts


def _extract_question_and_desc(market: dict) -> tuple[str, str]:
    """Extract question and description from market JSON."""
    question = market.get("question", "")
    if not question:
        events = market.get("events", [])
        if events:
            question = events[0].get("title", "")
    question = str(question) if question else "__MISSING__"
    description = str(market.get("description", ""))
    return question, description

# MODEL_NAME = "BAAI/bge-large-en-v1.5"
# MODEL_NAME = "BAAI/bge-base-en-v1.5"
MODEL_NAME = "BAAI/bge-small-en-v1.5"

# MODEL_NAME = "Qwen/Qwen3-Embedding-0.6B"
# MODEL_NAME = "sentence-transformers/all-mpnet-base-v2"
# MODEL_NAME = "sentence-transformers/all-MiniLM-L6-v2"
MODEL_BATCH_SIZE = 32
MODEL_SEQ_LEN = 400
LABEL_SEQ_LEN = 64


def demo_tagging(db_path: str = "", include_description: bool = INCLUDE_DESCRIPTION) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    db = Path(db_path).resolve() if db_path else load_stage0_db_path(repo_root)
    label_names, label_texts = load_labels_with_comments(repo_root)

    # compute max widths for alignment
    max_l1 = max(len(lbl.split(" - ", 1)[0]) for lbl in label_names)
    max_l2 = max(len(lbl.split(" - ", 1)[1]) for lbl in label_names)

    print(f"Loading model: {MODEL_NAME} ...")
    model = SentenceTransformer(MODEL_NAME)
    print(f"model.device: {model.device}")

    model.max_seq_length = LABEL_SEQ_LEN
    # encode category labels (用标签名+注释做embedding)
    print(f"Encoding category labels from TAG.md ... total={len(label_names)}")
    label_embeddings = model.encode(
        label_texts, normalize_embeddings=True, convert_to_numpy=True, batch_size=MODEL_BATCH_SIZE)
    torch.cuda.empty_cache()

    # load questions from database
    con = duckdb.connect(str(db), read_only=True)
    total = con.execute(
        "SELECT COUNT(*) FROM pm_condition_static").fetchone()[0]
    rows = con.execute(
        """
        SELECT
          lower(hex(s.condition_id)) AS cid,
          s.market_json::VARCHAR AS market_json,
          c.first_seen_block
        FROM pm_condition_static s
        JOIN pm_condition_scan_class c ON s.condition_id = c.condition_id
        ORDER BY cid ASC
        """
    ).fetchall()
    assert len(rows) == total, "row count mismatch"

    # random sample with fixed seed
    random.seed(RANDOM_SEED)
    if SAMPLE < total:
        rows = random.sample(rows, SAMPLE)
    sample_count = len(rows)

    print(f"db: {db}")
    print(f"total_conditions: {total}, sample: {sample_count}")
    print("-" * 160)

    # prepare output file
    script_dir = Path(__file__).resolve().parent
    tag_dir = script_dir / "tag"
    tag_dir.mkdir(exist_ok=True)
    model_short = MODEL_NAME.replace("/", "_")
    output_file = tag_dir / f"tag_{model_short}.txt"

    # extract texts for batch encoding
    print("Extracting texts...")
    texts: list[str] = []
    questions: list[str] = []
    for cid_hex, market_json, first_seen_block in rows:
        market = json.loads(market_json)
        question, description = _extract_question_and_desc(market)
        question = question.replace("\n", " ").strip()
        description = description.replace("\n", " ").strip()
        text_for_embedding = question
        if include_description and description:
            text_for_embedding = f"{question} {description}"
        texts.append(text_for_embedding)
        questions.append(question)

    model.max_seq_length = MODEL_SEQ_LEN
    # batch encode (no instruction needed for embedding models)
    print(f"Batch encoding {len(texts)} texts (batch_size={MODEL_BATCH_SIZE})...")
    t_start = time.perf_counter()
    query_embeddings = model.encode(texts, normalize_embeddings=True,
                                    convert_to_numpy=True, batch_size=MODEL_BATCH_SIZE, show_progress_bar=True)

    # compute similarities: dot product since normalized (query_embeddings @ label_embeddings.T)
    print("Computing similarities...")
    similarities = query_embeddings @ label_embeddings.T  # (N, num_labels)
    best_indices = np.argmax(similarities, axis=1)
    best_scores = similarities[np.arange(len(similarities)), best_indices]

    t_elapsed = time.perf_counter() - t_start
    items_per_sec = sample_count / t_elapsed

    # write results
    print("Writing results...")
    with output_file.open("w", encoding="utf-8") as f:
        for i, (cid_hex, market_json, first_seen_block) in enumerate(rows):
            best_label = label_names[best_indices[i]]
            level1, level2 = best_label.split(" - ", 1)
            f.write(
                f"0x{cid_hex}\t{level1.ljust(max_l1)}\t{level2.ljust(max_l2)}\t{best_scores[i]:.4f}\t{questions[i]}\n")
        f.write(f"\n# model: {MODEL_NAME}\n")
        f.write(
            f"# speed: {items_per_sec:.2f} items/sec ({sample_count} items in {t_elapsed:.2f}s)\n")

    print(
        f"\nSpeed: {items_per_sec:.2f} items/sec ({sample_count} items in {t_elapsed:.2f}s)")
    print(f"Results saved to: {output_file}")


if __name__ == "__main__":
    demo_tagging()
