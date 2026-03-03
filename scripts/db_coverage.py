#!/usr/bin/env python3
"""
快速用小模型给数据库随机样本打标签, 导出置信度最低的样本帮助迭代TAG定义。

标签设计原则:
1. MECE分类: 每个question必须且只能属于一个多级标签, 无重叠无遗漏
2. 专家认知边界: 标签应对应人类专家的知识领域 (NBA专家不需要懂NFL)
3. 数据驱动: 每个标签需有足够样本量, 避免长尾稀疏标签
4. 高覆盖率: 99%+的样本可被分类, 最小化"Other/Misc"兜底类
5. Embedding友好: 多级别标签名用清晰英文短语+关键词注释, 便于语义匹配

输出文件:
1. tag/coverage_analysis.txt - 总体分析报告
   - 标签分布: count/百分比/平均置信度, 用于发现过热(需拆分)或过冷(需合并)的标签
   - Bottom N标签: 平均置信度最低的标签及其样本, 用于发现边界模糊的标签

2. tag/tags/{label}.txt - 每个标签的详细样本
   - 按置信度升序排列, 低置信度在前
   - 用于分析具体哪些question被错误分类或边界不清

迭代方法:
1. 运行脚本, 先看coverage_analysis.txt:
   - 标签分布不均? -> 拆分过热标签 / 合并过冷标签
   - 某标签平均置信度低? -> 检查该标签定义是否清晰
2. 打开对应的tags/{label}.txt:
   - 低置信度样本是否确实属于该标签? -> 是则优化标签注释关键词
   - 低置信度样本应属于其他标签? -> 调整标签边界或新增标签
3. 修改TAG.md后重新运行验证
"""

from sentence_transformers import SentenceTransformer
import json
import logging
import random
from pathlib import Path

import duckdb
import numpy as np
import transformers

transformers.logging.set_verbosity_error()
logging.getLogger("sentence_transformers").setLevel(logging.ERROR)

# ============ 配置宏 ============
MODEL_NAME = "BAAI/bge-small-en-v1.5"
BATCH_SIZE = 128
TAG_MD_REL_PATH = "core-backend/src/stage0/TAG.md"

SAMPLE_SIZE = 170000         # 随机采样数量
RANDOM_SEED = None           # None则每次不同，设置整数则固定
INCLUDE_DESCRIPTION = False  # 是否包括description做embedding
BOTTOM_TAGS = 10             # 导出平均置信度最低的N个标签
SAMPLES_PER_TAG = 10         # 每个标签导出的低置信度样本数
# ================================


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
            # 用标签名+注释做embedding，增强匹配
            embed_text = f"{level2} {comment}".strip()
            label_texts.append(embed_text)

    assert label_names, f"no labels parsed from: {tag_md}"
    return label_names, label_texts


def _extract_question_and_desc(market: dict) -> tuple[str, str]:
    question = market.get("question", "")
    if not question:
        events = market.get("events", [])
        if events:
            question = events[0].get("title", "")
    question = str(question) if question else "__MISSING__"
    description = str(market.get("description", ""))
    return question, description


def main():
    repo_root = Path(__file__).resolve().parents[1]
    db_path = load_stage0_db_path(repo_root)
    label_names, label_texts = load_labels_with_comments(repo_root)

    print(f"Labels: {len(label_names)}")
    print(f"Loading model: {MODEL_NAME} ...")
    model = SentenceTransformer(MODEL_NAME)

    # encode labels (用标签名+注释)
    print("Encoding labels...")
    label_embeddings = model.encode(
        label_texts, normalize_embeddings=True, convert_to_numpy=True)

    # load from db
    con = duckdb.connect(str(db_path), read_only=True)
    total = con.execute(
        "SELECT COUNT(*) FROM pm_condition_static").fetchone()[0]
    print(f"Total conditions in db: {total}")

    rows = con.execute(
        """
        SELECT
          lower(hex(s.condition_id)) AS cid,
          s.market_json::VARCHAR AS market_json,
          c.first_seen_block
        FROM pm_condition_static s
        LEFT JOIN pm_condition_scan_class c
          ON s.condition_id = c.condition_id
        """
    ).fetchall()

    # random sample
    if RANDOM_SEED is not None:
        random.seed(RANDOM_SEED)
    sample_size = min(SAMPLE_SIZE, len(rows))
    rows = random.sample(rows, sample_size)
    print(f"Sampled: {sample_size}")

    # extract texts
    texts: list[str] = []
    questions: list[str] = []
    descriptions: list[str] = []
    cids: list[str] = []
    start_blocks: list[int] = []

    for cid_hex, market_json, first_seen_block in rows:
        market = json.loads(market_json)
        question, description = _extract_question_and_desc(market)
        question = question.replace("\n", " ").strip()
        description = description.replace("\n", " ").strip()
        # 根据配置决定是否包含description
        if INCLUDE_DESCRIPTION and description:
            text = f"{question} {description}"
        else:
            text = question
        texts.append(text)
        questions.append(question)
        descriptions.append(description[:200])  # 截断
        cids.append(cid_hex)
        start_blocks.append(first_seen_block if first_seen_block else 0)

    # batch encode
    print(f"Encoding {len(texts)} texts...")
    query_embeddings = model.encode(
        texts, normalize_embeddings=True, convert_to_numpy=True,
        batch_size=BATCH_SIZE, show_progress_bar=True
    )

    # compute similarities
    similarities = query_embeddings @ label_embeddings.T
    best_indices = np.argmax(similarities, axis=1)
    best_scores = similarities[np.arange(len(similarities)), best_indices]

    # per-label statistics
    label_counts = np.zeros(len(label_names), dtype=int)
    label_score_sums = np.zeros(len(label_names), dtype=float)
    label_samples: dict[int, list[tuple[float, int]]] = {
        i: [] for i in range(len(label_names))}

    for i, label_idx in enumerate(best_indices):
        label_counts[label_idx] += 1
        label_score_sums[label_idx] += best_scores[i]
        label_samples[label_idx].append((best_scores[i], i))

    # compute mean confidence per label
    label_mean_scores = np.zeros(len(label_names), dtype=float)
    for i in range(len(label_names)):
        if label_counts[i] > 0:
            label_mean_scores[i] = label_score_sums[i] / label_counts[i]
        else:
            label_mean_scores[i] = 1.0  # no samples = high confidence (ignore)

    # sort labels by count (descending) for distribution display
    sorted_by_count = np.argsort(-label_counts)
    # sort labels by mean confidence (ascending) for bottom tags
    sorted_by_confidence = np.argsort(label_mean_scores)

    nonzero_count = np.sum(label_counts > 0)
    zero_count = len(label_names) - nonzero_count

    # write coverage analysis
    script_dir = Path(__file__).resolve().parent
    tag_dir = script_dir / "tag"
    tag_dir.mkdir(exist_ok=True)
    analysis_file = tag_dir / "coverage_analysis.txt"

    # compute max label width for alignment
    max_label_width = max(len(lbl.split(" - ", 1)[1]) for lbl in label_names)

    with analysis_file.open("w", encoding="utf-8") as f:
        # header
        f.write(f"# Coverage Analysis\n")
        f.write(f"# Model: {MODEL_NAME}\n")
        f.write(f"# Sample size: {sample_size}\n")
        f.write(f"# Include description: {INCLUDE_DESCRIPTION}\n")

        # label distribution (sorted by count)
        f.write("\n" + "=" * 100 + "\n")
        f.write(
            f"Label distribution (all {len(label_names)} tags, sorted by count):\n")
        f.write("-" * 100 + "\n")
        for i in sorted_by_count:
            count = label_counts[i]
            pct = 100.0 * count / sample_size
            mean_conf = label_mean_scores[i]
            label2 = label_names[i].split(" - ", 1)[1]
            f.write(
                f"  {count:6d} ({pct:5.2f}%)  avg={mean_conf:.4f}  {label2}\n")
        f.write("-" * 100 + "\n")
        f.write(f"Tags with samples: {nonzero_count}/{len(label_names)}\n")
        f.write(f"Tags with zero samples: {zero_count}\n")

        # bottom N tags by mean confidence, with lowest samples
        f.write("\n" + "=" * 100 + "\n")
        f.write(
            f"Bottom {BOTTOM_TAGS} tags by mean confidence (lowest {SAMPLES_PER_TAG} samples each):\n")
        f.write("=" * 100 + "\n")

        for label_idx in sorted_by_confidence[:BOTTOM_TAGS]:
            if label_counts[label_idx] == 0:
                continue
            label2 = label_names[label_idx].split(" - ", 1)[1]
            mean_conf = label_mean_scores[label_idx]
            count = label_counts[label_idx]

            f.write(f"\n[{label2}] count={count}, avg_conf={mean_conf:.4f}\n")
            f.write("-" * 100 + "\n")

            # sort samples by confidence (ascending)
            samples = sorted(label_samples[label_idx], key=lambda x: x[0])
            for score, idx in samples[:SAMPLES_PER_TAG]:
                question = questions[idx][:90]
                f.write(f"  {score:.4f}  {question}\n")

    # generate per-label files
    tags_dir = tag_dir / "tags"
    tags_dir.mkdir(exist_ok=True)

    for label_idx in range(len(label_names)):
        if label_counts[label_idx] == 0:
            continue
        label2 = label_names[label_idx].split(" - ", 1)[1]
        label_file = tags_dir / f"{label2}.txt"

        # sort samples by confidence (ascending)
        samples = sorted(label_samples[label_idx], key=lambda x: x[0])

        with label_file.open("w", encoding="utf-8") as f:
            f.write(f"# {label_names[label_idx]}\n")
            f.write(f"# count={label_counts[label_idx]}, avg_conf={label_mean_scores[label_idx]:.4f}\n")
            f.write("=" * 100 + "\n")
            for score, idx in samples:
                question = questions[idx][:80]
                block = start_blocks[idx]
                f.write(f"{block:>10}  {score:.4f}  {question}\n")

    print(f"\nResults saved to:")
    print(f"  {analysis_file}")
    print(f"  {tags_dir}/*.txt ({nonzero_count} files)")


if __name__ == "__main__":
    main()
