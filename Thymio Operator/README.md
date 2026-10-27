# Commande du bras

Wi-Fi : **Thymio Arm**, mot de passe **micro452**, interface **http://192.168.4.1**.
Signal du bras sur GPIO 25, pince sur GPIO 26. Servos toujours actifs.

## Deux taches sur deux coeurs

- `arm-web` sur **core 0**, priorite 1 : initialisation Wi-Fi, serveur HTTP et
  reception des commandes. Aucun appel direct a la FSM ni aux PWM.
- `arm-motors` sur **core 1**, priorite 2 : initialisation des servos, FSM,
  affectation des PWM et sauvegarde Preferences, autorisee seulement au repos.
  La tache s'execute toutes les 2 ms, les trajectoires progressent toutes les
  20 ms. `loop()` ne fait que dormir.

`motor_runtime.cpp/.h` contient les donnees statiques partagees et la passerelle :
`get_ticks(0)` pour le bras, `get_ticks(1)` pour la pince, et `get_state()` pour
une copie coherente de l'ensemble (positions, ticks, disponibilite, phase et
parametres). Les getters ne renvoient aucune reference modifiable.

Le serveur depose une copie de commande avec `submit()`. Une seule commande peut
etre en attente : aucune n'est ecrasee, ni rejouee. La tache moteur la valide
selon son etat reel, l'execute et publie son acquittement. Le serveur attend
jusqu'a 1500 ms sans bloquer le core moteur ; si besoin il retourne un HTTP 202
et l'interface suit le resultat sans retransmettre la commande.

Les copies d'etat et la boite de commande utilisent un meme spinlock ESP32
(`portMUX_TYPE`). Les sections critiques ne contiennent que des copies courtes,
jamais d'appel reseau, PWM, flash ni d'attente. Ajouter seulement `volatile` aux
variables n'assurerait pas cette synchronisation entre coeurs.

Le moniteur serie a 115200 bauds affiche `Tache moteurs : core 1` et
`Tache interface : core 0`. Si la tache moteur ne peut pas etre creee, le serveur
n'est pas lance. La tache web attend que le premier etat moteur soit publie.

Cette separation evite qu'une requete HTTP lente retarde la FSM. La reactivite
visible reste aussi liee au Wi-Fi et au rafraichissement du navigateur ; le
signal PWM lui-meme est toujours genere par le peripherique LEDC.

Au demarrage, les consignes HOME et GRIP-CLOSED sauvegardees sont envoyees
immediatement, puis les commandes sont verrouillees pendant 2 secondes.
Le systeme ne connait pas la position physique au demarrage. Placer le mecanisme
dans une situation permettant ce retour avant de mettre sous tension.

## Sequences

- Catch exige HOME + GRIP-CLOSED : HOME -> MIDDLE, puis approche OUT et ouverture
  simultanees. La pince finit au plus tard a 80 % du temps de trajet du bras.
  Apres stabilisation : fermeture, puis retour MIDDLE avec objet suppose saisi.
- Release exige un Catch termine : MIDDLE -> OUT, ouverture, retour MIDDLE pince
  ouverte, puis retour HOME et **fermeture** simultanes. Cette fermeture interprete
  la derniere etape de la demande, qui repetait une ouverture deja effectuee.
- LOOP exige HOME + GRIP-CLOSED et repete Catch -> pause -> Release -> pause.
  Le bouton devient STOP. STOP termine la paire Catch-Release engagee et s'arrete
  a HOME + GRIP-CLOSED. Pendant la pause a HOME, il arrete immediatement la boucle
  sans lancer un nouveau Catch. Le bouton indique « Arret en cours » jusqu'au
  retour. Les commandes manuelles et Save sont verrouilles pendant toute la boucle,
  y compris les pauses. La boucle continue si le navigateur est deconnecte et
  n'est jamais relancee automatiquement apres un redemarrage.
- Aucun capteur ne confirme la position ni la presence d'un objet. Les pauses
  permettent aux servos de suivre les consignes, mais doivent etre calibrees sur
  le montage reel.

## Parametres

Positions en microsecondes. Valeurs initiales : HOME 2400, MIDDLE 2030, OUT 1440,
GRIP-OPEN 1530, GRIP-CLOSED 2000. Plage des deux sliders : 500 a 2500 us.
La conversion PWM utilise 50 Hz sur 16 bits (1 tick = 20000/65536 us).
Le champ **Pause LOOP (ms)** regle les deux attentes entre Catch et Release,
puis entre Release et le Catch suivant : 2000 ms par defaut, de 0 a 60000 ms,
enregistre uniquement avec Save. Le nouveau format reprend les calibrations
deja sauvegardees par les anciennes versions, sans les remettre a zero.

Chaque sens de trajet du bras et chaque sens de la pince disposent d'un pas
independant, exprime en us par periode de 20 ms (1 a 100). Un pas plus grand
accelere le mouvement. Pendant les phases simultanees, le bras peut etre ralenti
pour que la pince termine avant lui. La pause de stabilisation (100 a 3000 ms,
valeur initiale 300 ms) s'applique a OUT et apres les mouvements de pince seuls.
Il n'y a aucune pause a MIDDLE ni en fin de sequence. La disponibilite des
boutons est relue toutes les 100 ms pendant les mouvements (250 ms au repos),
avec expiration des requetes bloquees et reconnexion automatique.

Les champs du formulaire sont un brouillon : **Save** valide et enregistre
l'ensemble dans la NVS de l'ESP32 avec Preferences. Ils sont alors appliques sans
deplacer les moteurs. **Annuler les modifications** recharge les valeurs actives.
Les sliders commandent directement les moteurs au relachement, sans enregistrer
les positions et sans appliquer le pas des sequences. Ils invalident l'etat
« objet suspendu ». Revenir manuellement aux consignes HOME et GRIP-CLOSED pour
reautoriser Catch. Modifier les positions MIDDLE ou CLOSED pendant qu'un objet
est suspendu rend Release indisponible tant que les consignes actuelles ne
correspondent pas aux nouvelles valeurs ; modifier seulement les pas conserve
la possibilite de lancer Release.

Mouvements manuels et sauvegardes sont refuses pendant les sequences, dans
l'interface et dans l'API. Les sequences continuent si le navigateur se deconnecte.

## Verification

- Firmware : `platformio run` dans le dossier du projet.
- Tests sur ordinateur : `powershell -ExecutionPolicy Bypass -File test/host/run.ps1`
  (Visual Studio C++ requis). Les tests executent le vrai arm_sequence.cpp avec
  horloge, PWM et stockage simules : ordre Catch/Release, mouvements simultanes,
  verrouillages, limites, Save/rechargement, echecs et debordement de millis.
  Un second executable teste le vrai `motor_runtime.cpp` avec plusieurs threads :
  acces reserves au core moteur, copies coherentes, commandes non ecrasees,
  acquittements, poursuite des mouvements sans interface et lectures concurrentes.
- Interface : `node test/host/test_interface.cjs` (Node.js requis). Verification
  de l'actualisation, des reprises reseau et des acquittements differes.
- Avant utilisation : verifier sans objet les courses et temps de stabilisation,
  puis ajuster les pas selon la charge. La validation sur ordinateur ne mesure
  pas la vitesse physique des servos.

## Coordinateur PC Thymio + bras

Une interface Python/Tkinter relie la FSM du Thymio au bras via Wi-Fi/HTTP.
Installation, modes, calibration, navette et limites physiques : [pysrc/README.md](pysrc/README.md).
Lancement : `py pysrc/thymio_operator.py` (ou double-clic sur `pysrc/start_gui.cmd`).
Les commandes du coordinateur vers le bras sont limitees a Catch et Release.
