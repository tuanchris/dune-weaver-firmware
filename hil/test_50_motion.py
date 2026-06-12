# Tests that move the machine.  Opt in with HIL_MOTION=1 and make sure
# the table is clear; on a ThetaRho table X jogs rotate the arm.
from conftest import motion


@motion
def test_tiny_theta_jog_roundtrip(board):
    import pytest

    state = board.state()
    if state is None or not state.startswith("Idle"):
        pytest.skip(f"machine not idle ({state}); home or unlock first")
    _, status = board.cmd("$J=G91 G21 X0.2 F200")
    assert status == "ok"
    assert board.wait_state("Idle", 30), "jog did not complete"
    _, status = board.cmd("$J=G91 G21 X-0.2 F200")
    assert status == "ok"
    assert board.wait_state("Idle", 30), "return jog did not complete"
