"""Independent, naive integer/Fraction checks of the compiled numerical helpers.

This does not certify graph scheduling, missing-source handling, cache or host
acceptance. No production arithmetic or documentation research oracle is imported.
"""

from fractions import Fraction
from pathlib import Path
import random
import subprocess
import sys


def invoke(executable, arguments, input_text):
    result = subprocess.run(
        [str(executable), *map(str, arguments)], input=input_text, text=True,
        capture_output=True, check=True, timeout=30,
    )
    if result.stderr:
        raise AssertionError(f"unexpected probe diagnostics: {result.stderr}")
    return result.stdout.splitlines()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_arithmetic.py /absolute/path/to/arithmetic_probe")
    executable = Path(sys.argv[1]).resolve(strict=True)
    seed = 20261015
    random_source = random.Random(seed)
    minimum, maximum = -(1 << 63), (1 << 63) - 1
    round_cases = [(n, 2) for n in (1, 3, -1, -3, 5, -5)]
    round_cases += [(minimum, 1), (minimum, 3), (maximum, 2), (minimum, (1 << 64) - 1)]
    round_cases += [
        (random_source.randint(minimum, maximum), random_source.randint(1, (1 << 64) - 1))
        for _ in range(256)
    ]
    actual = invoke(executable, ["round"], "".join(f"{value} {count}\n" for value, count in round_cases))
    expected = [f"{index} {round(Fraction(value, count))}" for index, (value, count) in enumerate(round_cases)]
    if actual != expected:
        raise AssertionError("independent signed rounding comparison failed")

    datasets = [(3, 8, [2, 2, 2, 8]), (2, maximum // 2, [maximum // 2] * 7)]
    for window in range(1, 17):
        for bound in (0, 20, maximum // window):
            values = [random_source.randint(-bound, bound) for _ in range(80)]
            datasets.append((window, bound, values))
    sma_updates = 0
    for window, bound, values in datasets:
        actual = invoke(executable, ["sma", window, bound], "".join(f"{value}\n" for value in values))
        expected = []
        for index in range(len(values)):
            if index + 1 < window:
                expected.append(f"{index} warming")
            else:
                mean = round(Fraction(sum(values[index + 1 - window:index + 1]), window))
                expected.append(f"{index} published {mean}")
        if actual != expected:
            raise AssertionError(f"independent SMA comparison failed for window={window}, B={bound}")
        sma_updates += len(values)

    lag_updates = 0
    for lag in range(1, 9):
        values = [1, 1, 3, 5] + [random_source.randint(-20, 20) for _ in range(80)]
        actual = invoke(executable, ["lag", lag], "".join(f"{value}\n" for value in values))
        expected = [
            f"{index} warming" if index < lag else f"{index} published {values[index - lag]}"
            for index in range(len(values))
        ]
        if actual != expected:
            raise AssertionError(f"independent lag comparison failed for lag={lag}")
        lag_updates += len(values)
    print(f"arithmetic oracle: seed={seed}, rounding={len(round_cases)}, "
          f"SMA_runs={len(datasets)}, SMA_updates={sma_updates}, lag_runs=8, lag_updates={lag_updates}: passed")


if __name__ == "__main__":
    main()
