# Coordinateur Thymio + bras ESP32

## Mise en route

1. Compiler et téléverser le firmware depuis le dossier PlatformIO : `pio run -t upload`.
   Les réglages des servos en NVS et la page web existante sont conservés.
2. Connecter le PC au Wi-Fi **Thymio Arm**, mot de passe **micro452**.
   L'interface web reste disponible sur http://192.168.4.1.
3. Allumer le Thymio, brancher USB/dongle et ouvrir Thymio Suite.
   Libérer la connexion des notebooks/autres programmes.
4. Dans ce dossier : `py -m pip install -r requirements.txt`, puis
   `py thymio_operator.py`. Tkinter est fourni avec l'installation standard Python Windows.
5. Cliquer **Connecter les deux**. Attendre le statut du bras prêt et le Thymio vert.
   Nom Thymio vide = premier robot détecté ; préciser son nom si plusieurs robots.

## Modes et commandes

- Démarrage en **Steady**, vert, moteurs arrêtés.
- Flèches physiques gauche/droite (ou interface) : navigation circulaire
  Steady → Calibration → Suivi. Changer de mode arrête le châssis.
- **Calibration**, rouge : les deux capteurs sur blanc ; centre lance la moyenne
  pendant 3 s, puis retour Steady. Sauvegarde `thymio_calibration.json`.
  Sans fichier valide : blanc gauche/droite = 800. La calibration précédente du
  programme source est importée lors de l'installation si disponible.
- **Suivi**, bleu : centre lance la navette. Centre pendant le mouvement arrête
  le châssis et reste en Suivi, en attente d'un nouveau Go.
- **ARRÊT**, Déconnecter et fermeture immobilisent le Thymio. Une séquence Catch/
  Release déjà envoyée finit de s'exécuter sur l'ESP32 : on ne coupe pas les servos
  avec une charge suspendue. Le robot ne repart pas à la réception de sa fin.

## Séquence autonome

Ligne noire située entre les capteurs, fond blanc ; les deux extrémités portent
une bande noire couvrant **les deux capteurs**. Une intersection similaire est
également une fin. Suivi proportionnel normalisé par le blanc de chaque capteur.

En bout : arrêt et confirmation 0,25 s, recul pour dégager la bande, rotation
temporisée de 180°, reprise. Le marqueur n'est réarmé qu'après avoir quitté le
noir et une durée d'inhibition. Le centre arrête chaque phase.

Objet détecté par `prox.horizontal[2]` (IR frontal central) : approche lente
jusqu'au seuil de proximité de prise, recul temporisé, Catch, attente de la fin
de séquence, demi-tour, transport vers l'extrémité située derrière le robot.
En bout, Release puis recul/demi-tour et reprise de la navette. Pendant le
transport, la détection d'objet frontal est inhibée : l'objet tenu peut masquer
l'IR central. Ce capteur n'assure donc pas l'évitement d'obstacles avec une charge.

La dépose se fait à la portée OUT du bras devant le marqueur. Positionner le
marqueur de façon que cette zone soit hors de la trajectoire parcourue ensuite.

## Calibration mécanique indispensable

Tous les paramètres sont dans l'onglet Paramètres et sauvegardés dans
`operator_settings.json`. Ils s'appliquent à la prochaine connexion.

- IR détection/proximité : valeurs brutes de réflexion, **pas des distances**.
  Observer la télémétrie avec l'objet réel. Le seuil de proximité n'est pas un
  contact physique ; approcher jusqu'à être collé ne peut pas être garanti par IR.
  Délai d'approche maximal, puis arrêt si le seuil n'est pas atteint.
- Recul avant Catch : vitesse × durée à calibrer selon la géométrie du bras.
  Pas d'odométrie métrique : la distance n'est pas garantie précise.
- Demi-tour : réglage initial 4,55 s à 100, issu de 2 × 2275 ms du notebook.
  Le poids du bras, la surface et la batterie changent ce réglage. Cette version
  ne recherche pas automatiquement la ligne après une rotation mal calibrée.
- Si les deux capteurs voient du blanc, le robot avance : perte de ligne et
  centrage sur une ligne étroite restent ambigus. Tester d'abord à basse vitesse.
- Catch/Release terminés signifient que les consignes ont été parcourues ; aucun
  capteur ne confirme la présence de l'objet ni la position physique des servos.

## Communication et erreurs

Le PC utilise uniquement GET `/api/robot/state` et POST `/api/robot/action`
avec `action=catch` ou `action=release`. Aucun PWM/angle n'est envoyé par Python.
L'ESP32 conserve toute l'encapsulation et les verrouillages moteur existants.
Le ticket de commande et `canRelease`/`canCatch` distinguent acquittement et fin
de mouvement. Pas de nouvelle tentative automatique après un POST incertain.

En cas de coupure Wi-Fi/erreur du bras, arrêt et reconnexion manuelle après
vérification. Le bras termine sa séquence localement ; ne pas redémarrer
l'ESP32 avec un objet suspendu. Après reconnexion l'état supposé est relu.
Ne pas utiliser les commandes de la page web en parallèle de la navette.
Une commande concurrente détectée met le coordinateur en erreur.

La boucle Thymio et les requêtes HTTP sont dans des threads distincts ; la GUI
reste réactive. Un watchdog Aseba résident arrête les roues après environ 1 s
sans heartbeat du PC ; des mesures IR périmées arrêtent aussi le coordinateur.
Le watchdog remplace le programme précédemment chargé sur le robot.

Wi-Fi retenu : réutilisation du point d'accès et de la page web, pas d'appairage
Bluetooth. Wi-Fi/Bluetooth simultanés sont possibles sur l'ESP32 classique, mais
partagent la radio 2,4 GHz ; ils n'apportent rien ici.

## Vérifications sans matériel

Depuis la racine PlatformIO :

```powershell
py -m unittest discover -s pysrc -p test_operator.py -v
pio run
powershell -ExecutionPolicy Bypass -File test/host/run.ps1
```

Les tests Python simulent modes, calibration, navette, prise/dépose, arrêts et
erreurs, et exercent le client HTTP sur un serveur local. Ils ne valident pas
la mécanique, la portée IR ni les distances et angles physiques.
