"""Validate production records using captured KATL positions and a KAUS rollout."""

import json
from pathlib import Path
import sys


def check(path):
    sample, empty, message = [
        json.loads(line) for line in Path(path).read_text().splitlines()
    ]
    assert sample["schema"] == 1 and sample["utc_ms"] > 0
    assert sample["monotonic_s"] == 123.25
    player = sample["player"]
    assert player["heading_deg"] is None and player["on_ground"] is True
    assert player["lat"] == 33.64769955115317
    assert player["com1_frequency_hz_833"] == 118005
    assert "registration" not in player
    traffic = sample["traffic"]
    assert [p["id"] for p in traffic] == [8, 25, 53, 28, 40]
    assert [p["mode_s"] for p in traffic] == [1008, 1025, 1053, 1028, 1040]
    assert traffic[0]["commanded"]["altitude_msl_m"] == 310.89599609375
    assert traffic[0]["rendered_base"]["altitude_msl_m"] == 302.2900695800781
    assert traffic[1]["rendered_base"]["altitude_msl_m"] == 297.6044921875
    assert traffic[2]["commanded"]["altitude_msl_m"] < traffic[2]["ground_msl_m"]
    for p in traffic[:3]:
        assert p["rendered_base"]["altitude_msl_m"] == p["ground_msl_m"]
        assert p["commanded"]["on_ground"] is True
        assert p["feedback_age_s"] == 0.1 and p["command_age_s"] == 0.05
    assert traffic[3]["commanded"]["on_ground"] is False
    assert (
        traffic[3]["rendered_base"]["altitude_msl_m"]
        == traffic[3]["commanded"]["altitude_msl_m"]
    )
    assert traffic[4]["commanded"]["on_ground"] is False
    assert traffic[4]["interpolated"]["on_ground"] is False
    assert traffic[4]["commanded"]["altitude_msl_m"] == 33.43608474731445
    assert traffic[4]["rendered_base"]["altitude_msl_m"] == 152.46011352539062
    assert traffic[4]["rendered_base"]["altitude_msl_m"] == traffic[4]["ground_msl_m"]
    assert traffic[0]["callsign"] == 'TEST"\\\n'
    assert empty["player"] is None and empty["traffic"] == []
    assert empty["status"]["heartbeat_age_s"] is None
    assert message["event"] == "message" and "\n" in message["message"]
    assert message["message"].endswith("\x01\xff")


if __name__ == "__main__":
    check(sys.argv[1])
