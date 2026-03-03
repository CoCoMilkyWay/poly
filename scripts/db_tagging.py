#!/usr/bin/env python3
"""
Demo: Use BERTopic to model question topics.
condition_id | question
0x000d2622bf2bc49ffe1b9b609440017d09a75fa97be607e713b4c0045cfd1916 | Ansem FriendTech keys >10 ETH by Friday?
0x0010e9aa3b2a466703a2744a50a5101d11f1eaf1b6a4ebd80a68418ef57f5cf6 | Will Elon Musk visit Gaza in 2023?
0x001b6faa35c7d18d7eabea1a599812f7f0e132ea624e793b3921320e5bea9f5b | Will Texas, Florida, or California have the highest 7-day daily average of COVID-19 cases on April 15, 2021?
0x0026f721f4dd4caef4c1adff3aae56a6938141b7996a5c86e5879e80b9340e7a | Will BTC hit $60,000 in March?
0x002a797edf040e8a053e62b26d85a0292df091c5cacb303ae31407c8a050a32c | Will Matt Gaetz be expelled from Congress before May?
0x002cee569017e515dff96ad105bfe5569a4077d460c8c1fa5eab5be49932bcbb | NBA: Will the Hornets beat the Rockets by more than 6.5 points in their December 27 matchup?
0x0030593a04ac5d23141f7e2d9be9cbcde550d8ddf1068bf23f0210616c729e44 | Who will win Knicks vs. Hawks: Game 2?
0x0031605849bb07564ecadd8eb1f7e9a615b1932ae97f5b0b66c79fd0c4432c69 | Will $ETH be above $1,600 on August 26?
"""

import json
from pathlib import Path

import duckdb
import numpy as np
from sentence_transformers import SentenceTransformer


# macro: whether to include description text in embedding input
INCLUDE_DESCRIPTION = True
TAG_MD_REL_PATH = "core-backend/src/stage0/TAG.md"


def load_stage0_db_path(repo_root: Path) -> Path:
    config_path = repo_root / "config.json"
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    db_rel = config["db_path_stage0"]
    db_path = (repo_root / db_rel).resolve()
    assert db_path.exists(), f"stage0 db not found: {db_path}"
    return db_path


def load_two_level_labels(repo_root: Path) -> list[str]:
    tag_md = (repo_root / TAG_MD_REL_PATH).resolve()
    assert tag_md.exists(), f"tag file not found: {tag_md}"

    labels: list[str] = []
    level1 = ""
    for raw_line in tag_md.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("## "):
            level1 = line[3:].strip()
            continue
        if line.startswith("- "):
            level2 = line[2:].strip()
            assert level1, f"secondary tag without primary section: {level2}"
            assert level2, "empty secondary tag in TAG.md"
            labels.append(f"{level1} - {level2}")

    assert labels, f"no labels parsed from: {tag_md}"
    return labels


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

def cosine_similarity(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Compute cosine similarity between vectors a and matrix b."""
    return np.dot(a, b.T) / (np.linalg.norm(a) * np.linalg.norm(b, axis=1))


# MODEL_NAME = "BAAI/bge-large-en-v1.5"
MODEL_NAME = "sentence-transformers/all-mpnet-base-v2"
QUERY_INSTRUCTION = "Represent this sentence for searching relevant passages: "


def demo_tagging(db_path: str = "", include_description: bool = INCLUDE_DESCRIPTION) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    db = Path(db_path).resolve() if db_path else load_stage0_db_path(repo_root)
    category_labels = load_two_level_labels(repo_root)

    print(f"Loading model: {MODEL_NAME} ...")
    model = SentenceTransformer(MODEL_NAME)

    # encode category labels (no instruction needed for passages/labels)
    print(f"Encoding category labels from TAG.md ... total={len(category_labels)}")
    label_embeddings = model.encode(category_labels, normalize_embeddings=True, convert_to_numpy=True)

    # load questions from database
    con = duckdb.connect(str(db), read_only=True)
    total = con.execute("SELECT COUNT(*) FROM pm_condition_static").fetchone()[0]
    rows = con.execute(
        """
        SELECT
          lower(hex(condition_id)) AS cid,
          market_json::VARCHAR AS market_json
        FROM pm_condition_static
        ORDER BY cid ASC
        """
    ).fetchall()
    assert len(rows) == total, "row count mismatch"

    print(f"db: {db}")
    print(f"total_conditions: {total}")
    print("-" * 100)

    # process each question one by one
    for i, (cid_hex, market_json) in enumerate(rows):
        market = json.loads(market_json)
        question, description = _extract_question_and_desc(market)
        question = question.replace("\n", " ").strip()
        description = description.replace("\n", " ").strip()
        text_for_embedding = question
        if include_description and description:
            text_for_embedding = f"{question} {description}"
        query = QUERY_INSTRUCTION + text_for_embedding
        q_emb = model.encode(query, normalize_embeddings=True, convert_to_numpy=True)
        similarities = cosine_similarity(q_emb, label_embeddings)
        best_idx = np.argmax(similarities)
        best_label = category_labels[best_idx]
        best_score = similarities[best_idx]
        print(f"[{i+1}/{total}] 0x{cid_hex[:16]}... | {best_label} ({best_score:.4f}) | {question[:60]}")


if __name__ == "__main__":
    demo_tagging()
