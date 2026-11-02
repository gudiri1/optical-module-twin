# SPDX-License-Identifier: Apache-2.0
"""Shared assertions for DVT-style results."""


def assert_result(r):
    failed = [f"{label}: {detail}" for label, ok, detail in r.checks if not ok]
    assert r.passed and not failed, f"{r.name} failed:\n  " + "\n  ".join(failed)
