"""Tests sans robot : py -m unittest discover -s pysrc -p test_operator.py -v"""
import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

from arm_client import ArmClient, command_complete
from robot_controller import Controller, Mode, Settings


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.robot = Controller()
        self.robot.arm_ready = True

    def start(self):
        self.robot.select(Mode.LINE_FOLLOWING)
        self.robot.center(0)

    def test_modes_are_circular_and_do_not_start_motion(self):
        self.robot.navigate(-1)
        self.assertEqual(self.robot.mode, Mode.LINE_FOLLOWING)
        self.assertFalse(self.robot.active)
        self.robot.navigate(1)
        self.assertEqual(self.robot.mode, Mode.STEADY)

    def test_calibration_waits_for_center_and_averages_each_sensor(self):
        r = self.robot
        r.select(Mode.CALIBRATION)
        r.tick(0, (500, 700), 0)
        self.assertFalse(r.active)
        r.center(0)
        r.tick(1, (600, 800), 0)
        r.tick(3, (800, 1000), 0)
        self.assertEqual(r.white, (700, 900))
        self.assertEqual(r.mode, Mode.STEADY)

    def test_no_start_without_arm(self):
        self.robot.arm_ready = False
        self.start()
        self.assertFalse(self.robot.active)

    def test_correct_steering_and_center_stops_in_line_mode(self):
        self.start()
        r = self.robot
        r.tick(.1, (400, 800), 0)
        self.assertLess(r.left, r.right)
        r.center(.2)
        self.assertEqual(r.mode, Mode.LINE_FOLLOWING)
        r.tick(.3, (400, 800), 0)
        self.assertEqual((r.left, r.right), (0, 0))

    def test_end_marker_turns_then_rearms_only_after_leaving_marker(self):
        self.start()
        r = self.robot
        r.tick(1, (100, 100), 0)
        r.tick(1.3, (100, 100), 0)
        self.assertEqual(r.phase, "MARKER_BACK")
        r.tick(2, (100, 100), 0)
        self.assertEqual(r.phase, "TURN")
        r.tick(7, (100, 100), 0)
        r.tick(8, (100, 100), 0)
        self.assertEqual(r.phase, "FOLLOW")
        r.tick(9, (800, 800), 0)
        self.assertTrue(r.marker_armed)

    def test_complete_pickup_delivery_cycle(self):
        self.start()
        r = self.robot
        r.tick(.1, (800, 800), 2000)
        self.assertEqual(r.phase, "APPROACH")
        r.tick(.2, (800, 800), 3600)
        self.assertEqual(r.phase, "REVERSE")
        r.tick(.3, (800, 800), 1000)
        self.assertLess(r.left, 0)
        r.tick(1.1, (800, 800), 0)
        self.assertEqual(r.arm_order, "catch")
        self.assertEqual((r.left, r.right), (0, 0))
        r.arm_order = None  # Transport a transmis le POST une seule fois.
        r.arm_finished("catch", 2)
        self.assertTrue(r.holding)
        self.assertEqual(r.phase, "TURN")
        r.tick(7, (800, 800), 0)
        r.tick(8, (800, 800), 0)
        r.tick(9, (100, 100), 0)
        r.tick(9.3, (100, 100), 0)
        self.assertEqual(r.arm_order, "release")
        r.arm_order = None
        r.arm_finished("release", 10)
        self.assertFalse(r.holding)
        self.assertEqual(r.phase, "MARKER_BACK")
        r.tick(11, (800, 800), 0)
        r.tick(16, (800, 800), 0)
        self.assertEqual(r.phase, "FOLLOW")

    def test_stop_during_catch_never_restarts_after_completion(self):
        self.start()
        r = self.robot
        r._order("catch", 0)
        r.arm_order = None
        r.center(.1)
        r.arm_finished("catch", 2)
        self.assertTrue(r.holding)
        self.assertEqual(r.phase, "IDLE")
        self.assertEqual(r.mode, Mode.LINE_FOLLOWING)

    def test_held_object_does_not_trigger_a_second_pickup(self):
        self.start()
        self.robot.holding = True
        self.robot.tick(10, (800, 800), 4000)
        self.assertEqual(self.robot.phase, "FOLLOW")
        self.assertIsNone(self.robot.arm_order)

    def test_approach_timeout_stops(self):
        self.start()
        r = self.robot
        r.tick(.1, (800, 800), 2000)
        r.tick(5, (800, 800), 0)
        self.assertTrue(r.fault)
        self.assertEqual((r.left, r.right), (0, 0))

    def test_stop_cancels_an_order_not_yet_sent(self):
        self.start()
        self.robot._order("catch", 0)
        self.robot.stop()
        self.assertIsNone(self.robot.arm_order)
        self.assertFalse(self.robot.arm_pending)

    def test_fault_or_pending_arm_blocks_restart(self):
        self.start()
        self.robot.fail("reseau")
        self.robot.center(10)
        self.assertFalse(self.robot.active)

    def test_invalid_settings(self):
        for kw in ({"reverse_s": -1}, {"steering_gain": float("nan")},
                   {"contact_threshold": 100}, {"turn_speed": 600}):
            with self.assertRaises(ValueError):
                Settings(**kw).validate()


class ArmTests(unittest.TestCase):
    def test_wait_for_motion_not_just_ack(self):
        state = {"commandId": 4, "commandResult": "ok", "busy": True}
        self.assertFalse(command_complete(state, 4, "catch"))
        state.update(busy=False, canRelease=True)
        self.assertTrue(command_complete(state, 4, "catch"))
        with self.assertRaises(RuntimeError):
            command_complete(state, 5, "catch")
        state["commandResult"] = "rejected"
        with self.assertRaises(RuntimeError):
            command_complete(state, 4, "catch")

    def test_real_http_protocol_and_action_restriction(self):
        requests = []
        class Handler(BaseHTTPRequestHandler):
            def answer(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                requests.append((self.path, body))
                data = json.dumps({"protocolVersion": 1, "ready": True,
                                   "fault": False, "acceptedCommandId": 7}).encode()
                self.send_response(200)
                self.end_headers()
                self.wfile.write(data)
            do_GET = do_POST = answer
            def log_message(self, *args):
                pass
        server = HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = ArmClient(f"http://127.0.0.1:{server.server_port}")
            client.state()
            self.assertEqual(client.action("catch")["acceptedCommandId"], 7)
            with self.assertRaises(ValueError):
                client.action("manual")
            self.assertEqual(requests, [("/api/robot/state", b""),
                                        ("/api/robot/action", b"action=catch")])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    unittest.main()
