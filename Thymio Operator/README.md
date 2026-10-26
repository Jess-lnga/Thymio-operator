# Commande du bras

Wi-Fi : **Thymio Arm**, mot de passe **micro452**, interface **http://192.168.4.1**.
Signal du bras sur GPIO 25, pince sur GPIO 26. Servos toujours actifs.

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
- Aucun capteur ne confirme la position ni la presence d'un objet. Les pauses
  permettent aux servos de suivre les consignes, mais doivent etre calibrees sur
  le montage reel.

## Parametres

Positions en microsecondes. Valeurs initiales : HOME 2400, MIDDLE 2030, OUT 1440,
GRIP-OPEN 1530, GRIP-CLOSED 2000. Plage des deux sliders : 500 a 2500 us.
La conversion PWM utilise 50 Hz sur 16 bits (1 tick = 20000/65536 us).

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
- Avant utilisation : verifier sans objet les courses et temps de stabilisation,
  puis ajuster les pas selon la charge. La validation sur ordinateur ne mesure
  pas la vitesse physique des servos.
