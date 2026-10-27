"""Transport HTTP du bras. Aucun ordre servo, seulement catch/release.

Les appels tournent dans un thread HTTP, jamais dans la boucle TDM/Tk.
Un POST n'est jamais réessayé : une perte de réponse est un état incertain.
"""
import json
from urllib.request import Request, build_opener, ProxyHandler
from urllib.parse import urlencode


class ArmClient:
    def __init__(self, url, timeout=2):
        self.url = url.rstrip("/")
        self.timeout = timeout
        self.opener = build_opener(ProxyHandler({}))  # Adresse locale : pas de proxy système.

    def request(self, path, data=None):
        body = None if data is None else urlencode(data).encode()
        req = Request(self.url + path, data=body,
                      headers={"Content-Type": "application/x-www-form-urlencoded"})
        with self.opener.open(req, timeout=self.timeout) as response:
            state = json.load(response)
        if state.get("protocolVersion") != 1:
            raise RuntimeError("Firmware du bras à mettre à jour (API robot v1 requise)")
        if not state.get("ready") or state.get("fault"):
            raise RuntimeError("ESP32 non prêt ou défaut moteur")
        return state

    def state(self):
        return self.request("/api/robot/state")

    def action(self, action):
        if action not in ("catch", "release"):
            raise ValueError("Seuls catch et release sont autorisés")
        result = self.request("/api/robot/action", {"action": action})
        if not result.get("acceptedCommandId"):
            raise RuntimeError("Acquittement de commande absent")
        return result


def command_complete(state, ticket, action):
    """Distingue commande acceptée et mouvement physiquement terminé (supposé)."""
    if state.get("commandId") != ticket:
        raise RuntimeError("Commande du bras remplacée (interface web ou autre client)")
    if state.get("commandResult") == "rejected":
        raise RuntimeError("Commande refusée par le bras")
    if state.get("commandPending") or state.get("busy"):
        return False
    expected = "canRelease" if action == "catch" else "canCatch"
    if state.get("commandResult") != "ok" or not state.get(expected):
        raise RuntimeError("Fin de séquence incohérente ; vérifier le bras")
    return True
