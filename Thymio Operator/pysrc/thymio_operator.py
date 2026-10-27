"""Interface PC Thymio + bras ESP32. Lancer : py thymio_operator.py."""
import json
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict
from pathlib import Path

from tdmclient import ClientAsync
from arm_client import ArmClient, command_complete
from robot_controller import Controller, Settings, Mode, COLORS, load_white

ROOT = Path(__file__).resolve().parent
SETTINGS_FILE = ROOT / "operator_settings.json"
CALIBRATION_FILE = ROOT / "thymio_calibration.json"

# Programme résident minimal : arrêt local si le PC ne rafraîchit plus son
# heartbeat pendant ~1 s. La logique et les modes restent exécutés sur le PC.
WATCHDOG = """
var pc_heartbeat = 0
var previous_heartbeat = -1
var stale = 0
var sensor_tick = 0
motor.left.target = 0
motor.right.target = 0
timer.period[0] = 100
onevent timer0
    if pc_heartbeat == previous_heartbeat then
        stale += 1
    else
        stale = 0
    end
    previous_heartbeat = pc_heartbeat
    if stale >= 10 then
        motor.left.target = 0
        motor.right.target = 0
    end
onevent prox
    sensor_tick += 1
    if sensor_tick >= 30000 then
        sensor_tick = 0
    end
"""


class RobotWorker(threading.Thread):
    def __init__(self, settings, commands, events):
        super().__init__(daemon=True)
        self.settings, self.commands, self.events = settings, commands, events
        self.quit = threading.Event()

    def emit(self, kind, value):
        self.events.put((kind, value))

    def run(self):
        pool = ThreadPoolExecutor(max_workers=1, thread_name_prefix="arm-http")
        arm = ArmClient(self.settings.arm_url)
        try:
            with ClientAsync(zeroconf=True) as client:
                async def program():
                    self.emit("log", "Recherche du Thymio ; garder Thymio Suite ouvert…")
                    node = None
                    deadline = time.monotonic() + 20
                    while node is None:
                        if self.quit.is_set():
                            return
                        if time.monotonic() > deadline:
                            raise TimeoutError("Aucun Thymio trouvé après 20 s")
                        await client.sleep(0.05)
                        node = client.first_node(node_name=self.settings.robot_name or None)
                    if node.status != client.NODE_STATUS_AVAILABLE:
                        raise RuntimeError("Thymio occupé : libérer le notebook/autre programme")
                    locked = await node.lock()
                    assert locked is not None
                    with locked:
                        controller = Controller(self.settings, load_white(CALIBRATION_FILE))
                        async def checked(result):
                            if result is not None:
                                raise RuntimeError(f"Commande TDM refusée : {result}")
                        async def write(values):
                            await checked(await node.set_variables(values))
                        # Horodatages des vrais messages, pas des lectures du cache.
                        # Compteur de l'événement prox : les mesures peuvent
                        # rester constantes et ne générer aucun changement.
                        last_sensor = {"sensor_tick": time.monotonic()}
                        def changed(_node, variables):
                            for name in last_sensor:
                                if name in variables:
                                    last_sensor[name] = time.monotonic()
                        node.add_variables_changed_listener(changed)
                        try:
                            await checked(await node.stop())
                            await checked(await node.compile(WATCHDOG))
                            await checked(await node.run())
                            await node.wait_for_variables({"prox.ground.delta", "prox.horizontal",
                                "button.left", "button.right", "button.center", "pc_heartbeat", "sensor_tick"})
                            self.emit("connected", True)
                            previous = {b: node[f"button.{b}"] == 1 for b in ("left", "right", "center")}
                            last_press = {b: float("-inf") for b in previous}
                            http_future = pool.submit(arm.state)
                            http_kind = "state"
                            next_poll = 0.
                            pending_action = None
                            ticket = None
                            heartbeat = 0
                            last_color = None
                            next_display = 0.
                            last_message = ""
                            while not self.quit.is_set():
                                now = time.monotonic()
                                # Lire les boutons même pendant HTTP et séquences du bras.
                                presses = {}
                                for b in previous:
                                    down = node[f"button.{b}"] == 1
                                    presses[b] = down and not previous[b] and now - last_press[b] >= .2
                                    if presses[b]:
                                        last_press[b] = now
                                    previous[b] = down
                                actions = []
                                if presses["left"]:
                                    actions.append("left")
                                elif presses["right"]:
                                    actions.append("right")
                                elif presses["center"]:
                                    actions.append("center")
                                while True:
                                    try:
                                        actions.append(self.commands.get_nowait())
                                    except queue.Empty:
                                        break
                                for action in actions:
                                    if action == "stop":
                                        controller.stop()
                                    elif action == "left":
                                        controller.navigate(-1)
                                    elif action == "right":
                                        controller.navigate(1)
                                    elif action == "center":
                                        controller.center(now)
                                    elif action.startswith("mode:"):
                                        controller.select(int(action.split(":")[1]))

                                if http_future is not None and http_future.done():
                                    try:
                                        state = http_future.result()
                                        self.emit("arm", state["phase"])
                                        if state.get("loopActive"):
                                            raise RuntimeError("Désactiver LOOP dans l'interface du bras")
                                        if http_kind == "action":
                                            ticket = state["acceptedCommandId"]
                                        if pending_action:
                                            if command_complete(state, ticket, pending_action):
                                                controller.arm_finished(pending_action, now)
                                                pending_action = None
                                                ticket = None
                                        else:
                                            if state.get("busy") or state.get("commandPending"):
                                                controller.arm_ready = False
                                                if controller.active:
                                                    raise RuntimeError("Bras commandé depuis une autre interface")
                                            elif state.get("canCatch") or state.get("canRelease"):
                                                holding = bool(state["canRelease"])
                                                if controller.active and holding != controller.holding:
                                                    raise RuntimeError("État du bras modifié hors de l'automate")
                                                controller.holding = holding
                                                controller.arm_ready = True
                                            else:
                                                controller.arm_ready = False
                                                if controller.active:
                                                    raise RuntimeError("Bras hors position HOME/MIDDLE attendue")
                                    except Exception as error:
                                        # Ne pas répéter un POST qui a pu être exécuté.
                                        controller.fail(f"Bras : {error}. Reconnecter après vérification.")
                                        controller.arm_ready = False
                                        self.emit("arm", "Erreur / état incertain")
                                    http_future = None
                                    next_poll = now + .25

                                # Une coupure des mesures arrête le robot ; le watchdog
                                # prend aussi le relais si la boucle Python se bloque.
                                if max(now - t for t in last_sensor.values()) > 1.0:
                                    controller.fail("Mesures Thymio périmées ; reconnecter")
                                ground = tuple(node["prox.ground.delta"])
                                horizontal = tuple(node["prox.horizontal"])
                                front = horizontal[2]  # IR frontal central.
                                controller.tick(now, ground, front)
                                if controller.arm_order:
                                    # Attendre qu'une lecture HTTP en cours soit terminée.
                                    # Les moteurs restent arrêtés dans CATCH/RELEASE.
                                    if http_future is None and not controller.fault:
                                        pending_action = controller.arm_order
                                        controller.arm_order = None
                                        http_kind = "action"
                                        http_future = pool.submit(arm.action, pending_action)
                                elif http_future is None and now >= next_poll and not controller.fault:
                                    http_kind = "state"
                                    http_future = pool.submit(arm.state)

                                heartbeat = (heartbeat + 1) % 30000
                                values = {"motor.left.target": [controller.left],
                                          "motor.right.target": [controller.right],
                                          "pc_heartbeat": [heartbeat]}
                                if last_color != controller.mode:
                                    values["leds.top"] = COLORS[controller.mode]
                                    last_color = controller.mode
                                await write(values)
                                if controller.calibration_result is not None:
                                    try:
                                        CALIBRATION_FILE.write_text(json.dumps(
                                            {"white": controller.calibration_result}, indent=2), encoding="utf-8")
                                    except OSError as error:
                                        self.emit("log", f"Calibration active, sauvegarde impossible : {error}")
                                    controller.calibration_result = None
                                if last_message != controller.message:
                                    self.emit("log", controller.message)
                                    last_message = controller.message
                                if now >= next_display:
                                    self.emit("telemetry", {
                                        "mode": controller.mode.name, "phase": controller.phase,
                                        "ground": ground, "front": front, "white": controller.white,
                                        "motors": (controller.left, controller.right),
                                        "holding": controller.holding, "fault": controller.fault})
                                    next_display = now + .1
                                await client.sleep(.02)
                        finally:
                            # Le watchdog local reste actif même si cet arrêt échoue.
                            try:
                                await write({"motor.left.target": [0], "motor.right.target": [0],
                                             "leds.top": COLORS[Mode.STEADY]})
                            finally:
                                node.remove_variables_changed_listener(changed)
                client.run_async_program(program)
        except Exception as error:
            self.emit("log", f"ERREUR : {error}")
        finally:
            pool.shutdown(wait=False, cancel_futures=True)
            self.emit("connected", False)


LABELS = {
    "forward_speed": "Vitesse de suivi", "steering_gain": "Gain de correction",
    "max_speed": "Vitesse maximale", "end_black_ratio": "Ratio noir / blanc",
    "end_confirm_s": "Confirmation fin (s)", "calibration_s": "Mesure du blanc (s)",
    "obstacle_threshold": "IR : détection objet", "contact_threshold": "IR : proximité de prise",
    "approach_speed": "Vitesse d'approche", "approach_timeout_s": "Approche maximale (s)",
    "reverse_speed": "Vitesse de recul", "reverse_s": "Recul avant Catch (s)",
    "marker_reverse_s": "Recul avant demi-tour (s)", "turn_speed": "Vitesse demi-tour",
    "turn_s": "Durée demi-tour (s)", "marker_ignore_s": "Inhibition fin après tour (s)",
    "obstacle_ignore_s": "Inhibition prise après tour (s)", "arm_timeout_s": "Attente bras maximale (s)",
}


class Application(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Thymio + bras ESP32")
        self.geometry("930x760")
        self.minsize(800, 650)
        self.worker = None
        self.closing = False
        self.commands, self.events = queue.Queue(), queue.Queue()
        self.settings = Settings()
        try:
            saved = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            self.settings = Settings(**saved)
            self.settings.validate()
        except (OSError, ValueError, TypeError):
            self.settings = Settings()
        notebook = ttk.Notebook(self)
        notebook.pack(fill="both", expand=True, padx=12, pady=12)
        panel, tuning = ttk.Frame(notebook, padding=12), ttk.Frame(notebook, padding=12)
        notebook.add(panel, text="Pilotage")
        notebook.add(tuning, text="Paramètres (appliqués à la connexion)")
        self.fields = {}
        for row, (key, label) in enumerate((("arm_url", "Adresse ESP32"), ("robot_name", "Nom Thymio (vide = premier)"))):
            ttk.Label(panel, text=label).grid(row=row, column=0, sticky="w", pady=5)
            var = tk.StringVar(value=str(getattr(self.settings, key)))
            self.fields[key] = var
            ttk.Entry(panel, textvariable=var, width=42).grid(row=row, column=1, sticky="ew")
        connection = ttk.Frame(panel)
        connection.grid(row=2, column=0, columnspan=2, sticky="w", pady=10)
        self.connect_button = ttk.Button(connection, text="Connecter les deux", command=self.connect)
        self.connect_button.pack(side="left")
        ttk.Button(connection, text="Déconnecter", command=self.disconnect).pack(side="left", padx=5)
        self.status = tk.StringVar(value="Déconnecté")
        ttk.Label(connection, textvariable=self.status).pack(side="left", padx=10)
        self.arm_status = tk.StringVar(value="Bras : non connecté")
        ttk.Label(panel, textvariable=self.arm_status).grid(row=3, column=0, columnspan=2, sticky="w")
        controls = ttk.Frame(panel)
        controls.grid(row=4, column=0, columnspan=2, pady=15, sticky="w")
        self.action_buttons = []
        for text, action in (("◀ Mode", "left"), ("Centre / Go", "center"), ("Mode ▶", "right"), ("ARRÊT", "stop")):
            button = ttk.Button(controls, text=text, command=lambda a=action: self.commands.put(a), state="disabled")
            button.pack(side="left", padx=4)
            self.action_buttons.append(button)
        self.telemetry = tk.StringVar(value="Moteurs arrêtés")
        ttk.Label(panel, textvariable=self.telemetry, justify="left", font=("Consolas", 12)).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=8)
        ttk.Label(panel, text="Fin = bande noire sous les deux capteurs. Demi-tour et recul à calibrer.\n"
                  "L'IR mesure une réflexion, pas un contact. Catch terminé ≠ objet saisi.\n"
                  "ARRÊT immobilise le Thymio ; une séquence du bras déjà lancée se termine.",
                  wraplength=830).grid(row=6, column=0, columnspan=2, sticky="w", pady=10)
        self.log = tk.Text(panel, height=13, state="disabled", wrap="word")
        self.log.grid(row=7, column=0, columnspan=2, sticky="nsew")
        panel.columnconfigure(1, weight=1)
        panel.rowconfigure(7, weight=1)
        for i, (key, label) in enumerate(LABELS.items()):
            col, row = (i // 9) * 2, i % 9
            ttk.Label(tuning, text=label).grid(row=row, column=col, sticky="w", padx=8, pady=10)
            var = tk.StringVar(value=str(getattr(self.settings, key)))
            self.fields[key] = var
            ttk.Entry(tuning, textvariable=var, width=12).grid(row=row, column=col+1, padx=8)
        ttk.Label(tuning, text="Pour modifier : arrêter, déconnecter, régler, puis reconnecter.\n"
                  "Premiers essais : piste dégagée, objet léger, bras et roues visibles.",
                  wraplength=800).grid(row=10, column=0, columnspan=4, pady=24, sticky="w")
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.after(80, self.pump)

    def connect(self):
        if self.worker and self.worker.is_alive():
            return
        try:
            defaults = asdict(Settings())
            values = {key: type(defaults[key])(var.get().strip()) for key, var in self.fields.items()}
            settings = Settings(**values)
            settings.validate()
            SETTINGS_FILE.write_text(json.dumps(asdict(settings), indent=2), encoding="utf-8")
        except (ValueError, OSError) as error:
            messagebox.showerror("Paramètres", str(error))
            return
        self.commands = queue.Queue()
        self.connect_button.configure(state="disabled")
        self.status.set("Connexion…")
        self.worker = RobotWorker(settings, self.commands, self.events)
        self.worker.start()

    def disconnect(self):
        if self.worker:
            self.worker.quit.set()
            self.status.set("Arrêt / déconnexion…")

    def close(self):
        self.closing = True
        self.disconnect()

    def pump(self):
        while True:
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                self.log.configure(state="normal")
                self.log.insert("end", time.strftime("%H:%M:%S ") + str(value) + "\n")
                self.log.see("end")
                self.log.configure(state="disabled")
            elif kind == "connected":
                self.status.set("Thymio connecté" if value else "Déconnecté")
                self.connect_button.configure(state="disabled" if value else "normal")
                for button in self.action_buttons:
                    button.configure(state="normal" if value else "disabled")
            elif kind == "arm":
                self.arm_status.set("Bras : " + value)
            elif kind == "telemetry":
                self.telemetry.set(
                    f"Mode : {value['mode']} | Étape : {value['phase']}\n"
                    f"Sol G/D : {value['ground']} | IR central : {value['front']}\n"
                    f"Blanc G/D : {tuple(round(w) for w in value['white'])}\n"
                    f"Moteurs : {value['motors']} | Objet supposé : {value['holding']}\n"
                    f"{value['fault']}")
        if self.closing and (self.worker is None or not self.worker.is_alive()):
            self.destroy()
            return
        self.after(80, self.pump)


if __name__ == "__main__":
    Application().mainloop()
