"""FSM sans accès matériel : transitions testables avec une horloge simulée."""
from dataclasses import dataclass, asdict
from enum import IntEnum
import json
import math
from pathlib import Path


class Mode(IntEnum):
    STEADY = 0
    CALIBRATION = 1
    LINE_FOLLOWING = 2


COLORS = {Mode.STEADY: [0, 32, 0], Mode.CALIBRATION: [32, 0, 0],
          Mode.LINE_FOLLOWING: [0, 0, 32]}


@dataclass
class Settings:
    forward_speed: int = 100
    steering_gain: float = 75.0
    max_speed: int = 200
    end_black_ratio: float = 0.35
    end_confirm_s: float = 0.25
    calibration_s: float = 3.0
    obstacle_threshold: int = 1800
    contact_threshold: int = 3500
    approach_speed: int = 35
    approach_timeout_s: float = 4.0
    reverse_speed: int = 60
    reverse_s: float = 0.8
    marker_reverse_s: float = 0.6
    turn_speed: int = 100
    turn_s: float = 4.55
    marker_ignore_s: float = 0.6
    obstacle_ignore_s: float = 2.0
    arm_timeout_s: float = 90.0
    arm_url: str = "http://192.168.4.1"
    robot_name: str = ""

    def validate(self):
        for name, value in asdict(self).items():
            if isinstance(value, (int, float)) and (not math.isfinite(value) or value <= 0):
                raise ValueError(f"{name} doit être positif et fini")
        for name in ("forward_speed", "max_speed", "approach_speed", "reverse_speed", "turn_speed"):
            value = getattr(self, name)
            if not isinstance(value, int) or value > 500:
                raise ValueError(f"{name} : entier entre 1 et 500")
        if self.forward_speed > self.max_speed or not 0 < self.end_black_ratio < 1:
            raise ValueError("Vitesse ou ratio noir invalide")
        if not self.obstacle_threshold < self.contact_threshold <= 5000:
            raise ValueError("IR : détection < proximité de prise <= 5000")
        if not self.arm_url.startswith("http://"):
            raise ValueError("Adresse ESP32 attendue : http://adresse")


def valid_white(values):
    return (isinstance(values, (tuple, list)) and len(values) == 2
            and all(isinstance(v, (int, float)) and math.isfinite(v)
                    and 50 <= v <= 65535 for v in values))


def load_white(path):
    try:
        values = json.loads(Path(path).read_text(encoding="utf-8"))["white"]
        if valid_white(values):
            return tuple(values)
    except (OSError, ValueError, TypeError, KeyError):
        pass
    return (800.0, 800.0)


class Controller:
    def __init__(self, settings=None, white=(800., 800.)):
        self.cfg = settings or Settings()
        self.cfg.validate()
        self.white = white
        self.mode = Mode.STEADY
        self.phase = "IDLE"
        self.holding = False  # État supposé, confirmé par la séquence ESP32 uniquement.
        self.arm_pending = False
        self.arm_ready = False
        self.fault = ""
        self.message = "Steady : choisir un mode avec gauche/droite."
        self.left = self.right = 0
        self.started = 0.
        self.black_since = None
        self.marker_armed = True
        self.ignore_marker_until = 0.
        self.ignore_object_until = 0.
        self.samples = []
        self.calibration_result = None
        self.arm_order = None
        self.after_arm = None
        self.after_turn = "FOLLOW"

    @property
    def active(self):
        return self.phase != "IDLE"

    def stop(self, message="Arrêt ; centre pour repartir."):
        if self.arm_order is not None:
            # Ordre encore dans la file locale : l'annuler, ne pas le transmettre
            # après un arrêt/changement de mode.
            self.arm_order = None
            self.arm_pending = False
            self.arm_ready = True
        self.left = self.right = 0
        self.phase = "IDLE"
        self.black_since = None
        self.message = message
        # Une séquence déjà transmise au bras se termine sur l'ESP32.
        # Son acquittement est conservé, mais ne redémarre jamais le robot.
        self.after_arm = None

    def fail(self, message):
        self.fault = str(message)
        self.stop("ERREUR : " + self.fault)

    def select(self, mode):
        self.stop()
        self.mode = Mode(mode)
        self.message = {Mode.STEADY: "Steady : moteurs arrêtés.",
                        Mode.CALIBRATION: "Deux capteurs sur blanc ; centre pour mesurer.",
                        Mode.LINE_FOLLOWING: "Ligne entre les capteurs ; centre pour démarrer."}[self.mode]

    def navigate(self, direction):
        self.select((int(self.mode) + direction) % len(Mode))

    def center(self, now):
        if self.active:
            self.stop()
        elif self.mode == Mode.CALIBRATION:
            if self.arm_pending:
                self.message = "Attendre la fin du mouvement du bras."
                return
            self.samples = []
            self._phase("CALIBRATING", now)
            self.message = "Mesure du blanc : garder le robot immobile."
        elif self.mode == Mode.LINE_FOLLOWING:
            if self.fault or not self.arm_ready or self.arm_pending:
                self.message = "Bras non prêt ou erreur : vérifier/reconnecter avant de repartir."
                return
            self.marker_armed = True
            self.black_since = None
            self._phase("FOLLOW", now)
            self.message = "Navette active ; centre pour arrêter."

    def _phase(self, phase, now):
        self.phase = phase
        self.started = now
        self.left = self.right = 0

    def _order(self, action, now):
        if not self.arm_ready or self.arm_pending:
            self.fail("Bras indisponible")
            return
        self.arm_order = action
        self.arm_pending = True
        self.arm_ready = False
        self.after_arm = action
        self._phase("CATCH" if action == "catch" else "RELEASE", now)

    def arm_finished(self, action, now):
        self.arm_pending = False
        self.arm_ready = True
        self.holding = action == "catch"
        continuation = self.after_arm
        self.after_arm = None
        if continuation is None or self.phase not in ("CATCH", "RELEASE"):
            return
        if action == "catch":
            # Repartir vers l'extrémité derrière le robot : évite d'avancer
            # dans l'objet si la prise mécanique a échoué.
            self.after_turn = "FOLLOW"
            self._phase("TURN", now)
        else:
            self.after_turn = "FOLLOW"
            self._phase("MARKER_BACK", now)

    def tick(self, now, ground, front):
        self.left = self.right = 0
        c = self.cfg
        elapsed = now - self.started
        if self.phase == "CALIBRATING":
            self.samples.append(tuple(ground))
            if elapsed >= c.calibration_s:
                white = tuple(sum(s[i] for s in self.samples) / len(self.samples) for i in (0, 1))
                if valid_white(white):
                    self.white = white
                    self.calibration_result = white
                    self.select(Mode.STEADY)
                    self.message = f"Calibration terminée : {white[0]:.0f}, {white[1]:.0f}"
                else:
                    self.select(Mode.STEADY)
                    self.message = "Calibration refusée : réflexion insuffisante."
        elif self.phase == "FOLLOW":
            black = all(g < c.end_black_ratio * w for g, w in zip(ground, self.white))
            if not black and now >= self.ignore_marker_until:
                self.marker_armed = True
            if black and self.marker_armed:
                if self.black_since is None:
                    self.black_since = now
                if now - self.black_since >= c.end_confirm_s:
                    self.marker_armed = False
                    self.black_since = None
                    if self.holding:
                        self._order("release", now)
                    else:
                        self._phase("MARKER_BACK", now)
                return
            self.black_since = None
            # L'objet dans la pince peut rester devant l'IR : ne pas le
            # reclassifier comme un nouvel obstacle pendant son transport.
            if not self.holding and front >= c.obstacle_threshold and now >= self.ignore_object_until:
                self._phase("APPROACH", now)
                return
            dark = [max(0., min(1., 1 - g / w)) for g, w in zip(ground, self.white)]
            correction = c.steering_gain * (dark[0] - dark[1])
            self.left = round(max(0, min(c.max_speed, c.forward_speed - correction)))
            self.right = round(max(0, min(c.max_speed, c.forward_speed + correction)))
        elif self.phase == "APPROACH":
            if front >= c.contact_threshold:
                self._phase("REVERSE", now)
            elif elapsed >= c.approach_timeout_s:
                self.fail("Approche expirée : objet perdu ou seuil IR trop élevé")
            else:
                self.left = self.right = c.approach_speed
        elif self.phase in ("REVERSE", "MARKER_BACK"):
            duration = c.reverse_s if self.phase == "REVERSE" else c.marker_reverse_s
            if elapsed >= duration:
                if self.phase == "REVERSE":
                    self._order("catch", now)
                else:
                    self._phase("TURN", now)
            else:
                self.left = self.right = -c.reverse_speed
        elif self.phase == "TURN":
            if elapsed >= c.turn_s:
                self.ignore_marker_until = now + c.marker_ignore_s
                self.ignore_object_until = now + c.obstacle_ignore_s
                self.marker_armed = False
                self._phase("FOLLOW", now)
            else:
                self.left, self.right = c.turn_speed, -c.turn_speed
        elif self.phase in ("CATCH", "RELEASE") and elapsed > c.arm_timeout_s:
            self.fail("Délai de la séquence du bras dépassé ; vérifier son état")
