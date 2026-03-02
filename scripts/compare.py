import os
import hashlib
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import logging
from tqdm import tqdm

# ========= 可修改区域 =========
DIR1 = "~/work/poly/data0/stage1"
DIR2 = "~/work/poly/data/stage1"
MAX_WORKERS = 8
LOG_FILE = "compare.log"
# ==============================


def setup_logger():
    logging.basicConfig(
        filename=LOG_FILE,
        filemode="w",
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )


def sha256(path, buf_size=1024 * 1024):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(buf_size)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def collect_files(root):
    root = Path(root).expanduser()
    return {p.relative_to(root) for p in root.rglob("*") if p.is_file()}


def compare_one(rel_path):
    p1 = Path(DIR1).expanduser() / rel_path
    p2 = Path(DIR2).expanduser() / rel_path

    try:
        h1 = sha256(p1)
        h2 = sha256(p2)
        if h1 != h2:
            logging.error(
                "HASH MISMATCH: %s\n  WSL: %s\n  USB: %s",
                rel_path, h1, h2
            )
        return True
    except Exception as e:
        logging.exception("ERROR processing %s: %s", rel_path, e)
        return False


def main():
    setup_logger()
    dir1 = Path(DIR1).expanduser()
    dir2 = Path(DIR2).expanduser()
    assert dir1.is_dir(), f"DIR1 not found: {dir1}"
    assert dir2.is_dir(), f"DIR2 not found: {dir2}"

    print("[INFO] Collecting file list...")
    wsl_files = collect_files(dir1)
    usb_files = collect_files(dir2)
    common = sorted(wsl_files & usb_files)

    total = len(common)
    logging.info("Common files: %d", total)

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as exe:
        futures = [exe.submit(compare_one, f) for f in common]

        for _ in tqdm(
            as_completed(futures),
            total=total,
            desc="Comparing",
            unit="file",
            ncols=80,
        ):
            pass

    print("\n[DONE] Compare finished.")
    print(f"Log saved to: {LOG_FILE}")


if __name__ == "__main__":
    main()