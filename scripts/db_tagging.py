#!/usr/bin/env python3
"""
Demo: Use BAAI/bge-large-en-v1.5 to tag questions.
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
from tqdm import tqdm


# predefined category labels
CATEGORY_LABELS = [
    "Sports and Athletics",
    "Politics and Elections",
    "Cryptocurrency and Blockchain",
    "Finance and Stock Market",
    "Entertainment and Movies",
    "Science and Technology",
    "Weather and Climate",
    "Gaming and Esports",
    "World Events and News",
    "Legal and Court Cases",
]


def load_stage0_db_path(repo_root: Path) -> Path:
    config_path = repo_root / "config.json"
    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)
    db_rel = config["db_path_stage0"]
    db_path = (repo_root / db_rel).resolve()
    assert db_path.exists(), f"stage0 db not found: {db_path}"
    return db_path


def _extract_question(market: dict) -> str:
    question = market.get("question")
    if question:
        return str(question)
    events = market.get("events", [])
    if events:
        title = events[0].get("title")
        if title:
            return str(title)
    return "__MISSING__"


def cosine_similarity(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Compute cosine similarity between vectors a and matrix b."""
    return np.dot(a, b.T) / (np.linalg.norm(a) * np.linalg.norm(b, axis=1))


def demo_tagging(db_path: str = "", limit: int = 20) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    db = Path(db_path).resolve() if db_path else load_stage0_db_path(repo_root)

    print("Loading model: sentence-transformers/all-mpnet-base-v2 ...")
    model = SentenceTransformer("sentence-transformers/all-mpnet-base-v2")

    # encode category labels
    print("Encoding category labels ...")
    label_embeddings = model.encode(CATEGORY_LABELS, convert_to_numpy=True)

    conn = duckdb.connect(str(db), read_only=True)
    rows = conn.execute(
        f"""
        SELECT
          lower(hex(condition_id)) AS cid,
          market_json::VARCHAR AS market_json
        FROM pm_condition_static
        ORDER BY cid ASC
        LIMIT {limit}
        """
    ).fetchall()

    print(f"\ndb: {db}")
    print(f"tagging {len(rows)} questions\n")
    print("-" * 100)

    questions = []
    cids = []
    for cid_hex, market_json in rows:
        market = json.loads(market_json)
        question = _extract_question(market).replace("\n", " ").strip()
        questions.append(question)
        cids.append(cid_hex)

    # batch encode questions
    print("Encoding questions ...")
    question_embeddings = model.encode(questions, show_progress_bar=True, convert_to_numpy=True)

    # compute similarities and assign tags
    print("\nResults:\n")
    for i, (cid, question, q_emb) in enumerate(zip(cids, questions, question_embeddings)):
        similarities = cosine_similarity(q_emb, label_embeddings)
        best_idx = np.argmax(similarities)
        best_label = CATEGORY_LABELS[best_idx]
        best_score = similarities[best_idx]

        print(f"[{i+1}] 0x{cid[:16]}...")
        print(f"    Q: {question[:80]}{'...' if len(question) > 80 else ''}")
        print(f"    Tag: {best_label} (score: {best_score:.4f})")
        print()


if __name__ == "__main__":
    demo_tagging(limit=20)
