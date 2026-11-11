# Thermostat programmable – Arduino Mega 2560

Thermostat à hystérésis avec minuterie : LCD 1602A, capteur SHT31 (température + humidité), 3 boutons, relais et buzzer.
Sketch : [`Thermostat_v2/Thermostat_v2.ino`](Thermostat_v2/Thermostat_v2.ino) (v2.0).

> ⚠️ **Sécurité** – Si le relais commande un chauffage sur le secteur, ajoutez **toujours** une protection matérielle indépendante (fusible thermique ou thermostat de sécurité mécanique) en série avec la charge. Le logiciel ne peut pas protéger contre un relais soudé ou un capteur mal placé. Ne travaillez sur le 230 V que si vous êtes qualifié.

## Matériel

| Élément | Rôle |
|---|---|
| Arduino Mega 2560 | Microcontrôleur |
| LCD 1602A (HD44780) | Affichage 16×2 |
| SHT31 (Adafruit ou compatible) | Température / humidité (I2C) |
| 3 boutons poussoirs | `+`, `-`, `OK` |
| Module relais 5 V | Commande du chauffage |
| Buzzer passif | Bips de fin et d'alarme |
| Potentiomètre 10 kΩ | Contraste du LCD |
| Résistance 10 kΩ | Pull-down du relais (voir ci-dessous) |

## Câblage

### LCD 1602A (mode 4 bits)

| LCD | Mega |
|---|---|
| VSS | GND |
| VDD | 5 V |
| V0 | Curseur du potentiomètre 10 kΩ (extrémités sur 5 V et GND) |
| RS | D2 |
| RW | GND |
| E | D3 |
| D4 / D5 / D6 / D7 | D4 / D5 / D6 / D7 |
| D0 à D3 | non connectées |
| A (LED+) | 5 V via 220 Ω |
| K (LED−) | GND |

### SHT31 (I2C)

| SHT31 | Mega |
|---|---|
| VIN | 5 V (module avec régulateur, type Adafruit) ou 3,3 V |
| GND | GND |
| SDA | D20 (SDA) |
| SCL | D21 (SCL) |
| ADR | libre/GND → adresse `0x44` ; au +VCC → `0x45` (les deux sont essayées) |

### Boutons, buzzer, relais

| Élément | Broche Mega | Remarque |
|---|---|---|
| Bouton `+` | D22 → GND | `INPUT_PULLUP`, pas de résistance externe |
| Bouton `-` | D23 → GND | idem |
| Bouton `OK` | D24 → GND | idem |
| Buzzer (+) | D8 | autre borne sur GND, buzzer **passif** |
| Relais (IN) | D53 | VCC → 5 V, GND → GND |

**Pull-down du relais :** ajoutez une résistance de 10 kΩ entre D53 et GND. Pendant un reset (ou si le watchdog redémarre la carte), la broche flotte : sans pull-down, certains modules peuvent se déclencher.

**Module relais actif à l'état bas :** inversez `RELAIS_ON` et `RELAIS_OFF` en haut du sketch, et remplacez le pull-down par un pull-up.

**Charge :** le contact COM/NO du relais se place en série sur le conducteur de phase du chauffage, derrière un fusible adapté, dans un boîtier fermé et relié à la terre.

## Installation

1. Arduino IDE ou Arduino CLI, carte **Arduino Mega or Mega 2560**.
2. Bibliothèques : **Adafruit SHT31 Library** (installe aussi *Adafruit BusIO*). `Wire`, `LiquidCrystal` et `EEPROM` sont fournies avec l'IDE.
3. Ouvrir `Thermostat_v2/Thermostat_v2.ino` (le dossier doit porter le même nom que le `.ino`) et téléverser.

> Le watchdog matériel est activé (4 s). Certains Mega à ancien bootloader peuvent mal le gérer après un reset watchdog ; le sketch le désactive au démarrage pour éviter une boucle de reset. En cas de doute, mettre un test sur table avant l'installation définitive.

## Navigation

| Écran | `+` / `-` | `OK` | Appui long `-` (1,5 s) |
|---|---|---|---|
| **Accueil** (T, H, consigne, tolérance) | – | Ouvre le menu | – |
| **Menu** (Consigne, Durée, Tolérance, Lancer, Retour) | Option précédente / suivante | Valide l'option | – |
| **Édition d'un réglage** | Modifie la valeur (maintenir = répétition accélérée) | Enregistre et revient au menu | – |
| **Confirmation** (`Go?`) | Bascule OUI / NON | Valide (lance le cycle si OUI) | – |
| **Cycle** | – | Pause | Abandonne le cycle |
| **Pause** | – | Reprend | Abandonne le cycle |
| **Fin de cycle** | – | Retour à l'accueil | – |
| **Alarme** | – (coupe le bip) | Acquitte | – |

- Retour automatique à l'accueil après **60 s** sans appui dans les menus.
- Une touche coupe le bip en cours.
- Pendant un cycle, l'écran affiche en permanence : température / consigne, humidité, temps restant, état du chauffage (`ON` avec flamme / `OFF`). En pause, le temps clignote.

## Réglages

| Réglage | Plage | Pas | Défaut |
|---|---|---|---|
| Consigne | 5,0 – 80,0 °C | 0,5 °C | 40,0 °C |
| Durée | 00h30 – 24h00 | 30 min | 00h30 |
| Tolérance (hystérésis) | ±0,5 – ±5,0 °C | 0,5 °C | ±1,0 °C |

Les réglages sont **sauvegardés en EEPROM** (avec CRC) à la sortie de chaque édition. Si les données sont invalides, les valeurs par défaut sont utilisées.

**Régulation :** le relais s'enclenche à `consigne − tolérance` et se coupe à `consigne + tolérance`. La minuterie décompte dès le lancement du cycle, indépendamment de la température.

### Constantes à adapter (haut du sketch)

| Constante | Défaut | Rôle |
|---|---|---|
| `TEMP_SECURITE_ABS` | 85 °C | Alarme de surchauffe dans tous les états |
| `MARGE_SURCHAUFFE` | 5 °C | Alarme si T ≥ consigne + tolérance + marge (en cycle) |
| `TIMEOUT_CAPTEUR` | 30 s | Alarme si le capteur est en défaut pendant un cycle |
| `DUREE_SANS_HAUSSE` / `HAUSSE_MIN` | 20 min / 1 °C | Alarme « pas de chauffe » (0 = désactivée) |
| `DELAI_MIN_COMMUTATION` | 5 s | Délai minimal entre deux commutations du relais |
| `TIMEOUT_PAUSE` / `TIMEOUT_PAUSE_ABANDON` | 5 min / 30 min | Réinitialisation du temps / arrêt du cycle en pause |
| `OFFSET_TEMP` | 0,0 °C | Correction d'étalonnage de la température |
| `BIP_TOUCHES` | `true` | Petit bip à chaque appui |

## Sécurités

| Situation | Comportement |
|---|---|
| Capteur absent ou lecture invalide | Relais coupé immédiatement ; cycle refusé au lancement ; alarme après 30 s en cycle |
| T ≥ 85 °C (quel que soit l'état) | Relais coupé, alarme verrouillée |
| Dépassement de consigne + marge | Relais coupé, alarme verrouillée |
| Relais ON sans hausse de température (20 min) | Alarme « PAS DE CHAUFFE » |
| Pause trop longue (30 min) | Cycle abandonné, relais coupé |
| Blocage du programme | Reset par watchdog (4 s), relais coupé, message `RESET WATCHDOG!` |
| Coupure de courant | Aucun redémarrage automatique du cycle |
| Parasites sur le LCD | Ré-initialisation toutes les 30 s |

Les alarmes restent affichées jusqu'à acquittement par `OK` (le bip s'arrête seul après 2 min).

## Licence

À définir.
