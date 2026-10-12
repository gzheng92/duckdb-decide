"""Opt-in paid Decisions probe; mirrors PlanBatch, saves usage without credentials."""
import argparse
import datetime
import json
import os
import statistics
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path


def payload(rubric, version):
    rows, questions = [], []
    for i in range(20):
        name = f"row_{i}"
        condition = "arrived crushed and its contents were broken" if i % 2 == 0 else "arrived intact with no damage"
        rows.append({"id": name, "text": f"Parcel {version}-{i} {condition}."})
        questions.append({"type": "predicate", "name": name, "instructions":
            "The input is a JSON object containing rows. Evaluate only the text of the "
            f"record whose id is {name}. Ignore all other records. Question: " + rubric})
    return {"model": "gpt-6-luna", "input": json.dumps({"rows": rows}, separators=(",", ":")), "questions": questions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    key = os.environ.get("OPENAI_API_KEY")
    if not key:
        raise SystemExit("OPENAI_API_KEY is required")
    tag = uuid.uuid4().hex
    short = f"Evaluation {tag}. Does the parcel description explicitly report physical damage?"
    long = short + "\n" + "\n".join(
        f"Criterion {i}: Interpret the description literally. Crushed packaging, broken contents, tears, dents, and leaks count as physical damage. "
        "Late delivery, tracking delays, price complaints, and missing information do not establish physical damage. "
        "An explicit statement that the parcel arrived intact without damage is negative evidence."
        for i in range(45))
    report = {"date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "model": "gpt-6-luna", "batch_size": 20, "repetitions": 4,
              "transport": "fresh urllib HTTPS connection per request; no cache controls; plugin-equivalent payload",
              "long_rubric_characters": len(long), "samples": []}
    output = Path(args.output)
    for group, rubric in [("long_identical", long), ("long_changing_rows", long), ("short_changing_rows", short)]:
        for rep in range(4):
            body = payload(rubric, 0 if group == "long_identical" else rep + 1)
            req = urllib.request.Request("https://api.openai.com/v1/decisions", data=json.dumps(body).encode(),
                headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
            start = time.perf_counter()
            try:
                with urllib.request.urlopen(req, timeout=60) as response:
                    result = json.load(response)
            except urllib.error.HTTPError as exc:
                raise SystemExit(f"Decisions HTTP {exc.code}; no retry") from None
            except urllib.error.URLError:
                raise SystemExit("Decisions transport failed; no retry") from None
            answers = result.get("answers", [])
            if len(answers) != 20 or any(a.get("name") != f"row_{i}" or a.get("type") != "predicate" for i, a in enumerate(answers)):
                raise SystemExit("Unexpected answer schema")
            sample = {"group": group, "repetition": rep + 1, "seconds": round(time.perf_counter() - start, 3),
                      "usage": result.get("usage"), "correct_at_0_5": sum((a["probability"] >= .5) == (i % 2 == 0) for i, a in enumerate(answers))}
            report["samples"].append(sample)
            output.write_text(json.dumps(report, indent=2) + "\n")
            print(json.dumps(sample), flush=True)
            # Give server-side cache writes time to become visible.
            time.sleep(2)
    report["median_seconds"] = {g: statistics.median(s["seconds"] for s in report["samples"] if s["group"] == g)
                                for g in {s["group"] for s in report["samples"]}}
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["median_seconds"]), flush=True)


if __name__ == "__main__":
    main()
