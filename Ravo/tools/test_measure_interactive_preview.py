#!/usr/bin/env python3
"""Validate burst-report rejection independently of live-render timing."""

from copy import deepcopy
import unittest

from measure_interactive_preview import validate_burst_row


def report(boundary="latest"):
    def frame(revision, index, sent, elapsed):
        return {"revision": revision, "intent_index": index, "intents_sent": sent,
                "elapsed_us": elapsed, "width": 1600, "height": 1069,
                "gpu_backend": "metal", "output_profile": "srgb"}

    return {
        "case": "burst_" + boundary + "_exposure",
        "samples": [50, 300] if boundary == "first" else [150, 200],
        "workload": {
            "schema": "ravo.perf01.burst-workload/v1", "intent_count": 3,
            "requested_interval_ms": 1, "warmup_kind": "complete_bursts",
            "accepted_intent_order": "strictly_increasing", "latest_target_verified": True,
            "includes_monitor_presentation": True, "includes_native_frame_swap": False,
            "values": [.3, .6], "output_sizes": [[1600, 1069], [1600, 1069]],
            "gpu_backends": ["metal", "metal"],
            "intent_values": [[.1, .2, .3], [.4, .5, .6]],
            "intent_offsets_us": [[100, 200, 300], [50, 100, 150]],
            "accepted_frames": [[frame(11, 0, 1, 150), frame(13, 2, 3, 450)],
                                [frame(23, 2, 3, 350)]],
            "frame_counts": [2, 1], "intents_without_publication": [1, 2],
            "start_revisions": [10, 20],
        },
    }


class BurstReportTest(unittest.TestCase):
    def test_valid_first_and_latest_use_different_clock_origins(self):
        validate_burst_row(report("first"), 2)
        validate_burst_row(report("latest"), 2)

    def test_stale_duplicate_or_unbound_publication_is_rejected(self):
        for key, value in (("intents_sent", 0), ("revision", 10),
                           ("intent_index", 1), ("elapsed_us", 99)):
            with self.subTest(key=key):
                row = report()
                row["workload"]["accepted_frames"][0][0][key] = value
                with self.assertRaises(RuntimeError):
                    validate_burst_row(row, 2)
        row = report()
        row["workload"]["accepted_frames"][0][1]["revision"] = 11
        with self.assertRaises(RuntimeError):
            validate_burst_row(row, 2)

    def test_intermediate_frame_can_follow_queued_intents_without_completing_the_trial(self):
        row = report()
        row["workload"]["accepted_frames"][0][0]["intents_sent"] = 3
        validate_burst_row(row, 2)
        row["workload"]["accepted_frames"][0].pop()
        row["workload"]["frame_counts"][0] = 1
        row["workload"]["intents_without_publication"][0] = 2
        with self.assertRaises(RuntimeError):
            validate_burst_row(row, 2)

    def test_latest_intent_and_frame_accounting_are_required(self):
        row = report()
        row["workload"]["accepted_frames"][0].pop()
        row["workload"]["frame_counts"][0] = 1
        row["workload"]["intents_without_publication"][0] = 2
        with self.assertRaises(RuntimeError):
            validate_burst_row(row, 2)
        row = report()
        row["workload"]["intents_without_publication"][0] = 0
        with self.assertRaises(RuntimeError):
            validate_burst_row(row, 2)

    def test_timing_value_size_and_backend_must_match_the_trace(self):
        mutations = (
            lambda row: row["samples"].__setitem__(0, 149),
            lambda row: row["workload"]["values"].__setitem__(0, .31),
            lambda row: row["workload"]["output_sizes"].__setitem__(0, [1600, 1068]),
            lambda row: row["workload"]["gpu_backends"].__setitem__(0, "cpu"),
        )
        for mutate in mutations:
            row = deepcopy(report())
            mutate(row)
            with self.assertRaises(RuntimeError):
                validate_burst_row(row, 2)

    def test_invalid_or_uncorrelated_intent_stream_is_rejected(self):
        for values in ([.1, float("nan"), .3], [.1, .1, .3], [.1, .2]):
            row = report()
            row["workload"]["intent_values"][0] = values
            with self.assertRaises(RuntimeError):
                validate_burst_row(row, 2)
        row = report()
        row["workload"]["intent_offsets_us"][0] = [200, 100, 300]
        with self.assertRaises(RuntimeError):
            validate_burst_row(row, 2)
        row = report()
        row["workload"]["frame_counts"].pop()
        with self.assertRaises(RuntimeError):
            validate_burst_row(row, 2)


if __name__ == "__main__":
    unittest.main()
