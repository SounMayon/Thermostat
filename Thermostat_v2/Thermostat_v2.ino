/*
  =====================================================================
  THERMOSTAT PROGRAMMABLE - ARDUINO MEGA 2560                  v2.0
  LCD 1602A + SHT31 + 3 boutons + relais + buzzer
  =====================================================================
  Evolutions par rapport a la v1 :

  SECURITE / FIABILITE
   - Watchdog materiel (4 s) : le relais retombe si le programme se bloque
   - Alarmes verrouillees : surchauffe, capteur absent, chauffe inefficace
   - Pause limitee : arret automatique du cycle apres 30 min de pause
   - Delai mini entre deux commutations du relais (usure, rebonds thermiques)
   - Reglages sauvegardes en EEPROM (CRC + validation des plages)
   - Aucun redemarrage automatique de cycle apres coupure de courant
   - Timeout I2C (si le coeur AVR le supporte) contre les blocages du bus
   - Re-initialisation periodique du LCD (parasites du relais)
   - Refus de demarrer un cycle si le capteur est absent

  INTERFACE
   - Auto-repetition acceleree sur + / - (appui maintenu)
   - Ecran de cycle fixe (plus d'alternance) : temperature, consigne,
     humidite, temps restant, etat du chauffage
   - Icones LCD, messages de confirmation, option "Retour" dans le menu
   - Retour automatique a l'accueil apres 60 s d'inactivite
   - Bips de fin de cycle et d'alarme (arret par une touche)
*/

#include <Wire.h>
#include <LiquidCrystal.h>
#include <Adafruit_SHT31.h>
#include <EEPROM.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION_LOGICIEL "2.0"

// Caracteres speciaux du LCD. Chaque macro est un litteral separe :
// toujours les concatener avec un autre litteral ("\x01" " texte").
#define STR_CHAUFFE "\x01"   // flamme (caractere personnalise)
#define STR_HORLOGE "\x02"   // horloge (caractere personnalise)
#define STR_CLOCHE  "\x03"   // cloche (caractere personnalise)
#define STR_COCHE   "\x04"   // coche (caractere personnalise)
#define STR_DEGRE   "\xDF"   // symbole degre (ROM A00)
#define STR_FLECHE  "\x7E"   // fleche droite (ROM A00)

// =====================================================
// TYPES (definis avant toute fonction)
// =====================================================

struct Bouton {
  byte pin;
  bool etatBrut;
  bool etatStable;
  unsigned long dernierChangement;
  unsigned long debutAppui;
  unsigned long dernierRepete;
  byte nbRepetes;
  bool longSignale;
};

enum Ecran {
  ACCUEIL,
  MENU,
  EDIT_TEMP,
  EDIT_DUREE,
  EDIT_TOLERANCE,
  CONFIRMATION,
  CYCLE,
  PAUSE,
  TERMINE,
  ALARME
};

enum Defaut {
  DEFAUT_AUCUN,
  DEFAUT_SURCHAUFFE,
  DEFAUT_CAPTEUR,
  DEFAUT_CHAUFFE
};

enum ModeBip {
  BIP_AUCUN,
  BIP_FIN,
  BIP_ALARME
};

struct Reglages {
  uint16_t magique;
  uint8_t version;
  float consigne;
  float tolerance;
  uint16_t duree;
  uint16_t crc;   // doit rester le DERNIER champ
};

// =====================================================
// CAUSE DU DERNIER RESET + DESACTIVATION DU WATCHDOG AU BOOT
// Execute tres tot (section .init3), avant setup().
// Evite une boucle de reset apres un reset par watchdog.
// =====================================================

#if defined(__AVR__)
uint8_t causeReset __attribute__((section(".noinit")));

void lireCauseReset(void) __attribute__((naked)) __attribute__((used))
    __attribute__((section(".init3")));

void lireCauseReset(void) {
  causeReset = MCUSR;
  MCUSR = 0;
  wdt_disable();
}
#else
uint8_t causeReset = 0;
#endif

// =====================================================
// MATERIEL
// =====================================================

// LCD : RS, E, D4, D5, D6, D7
LiquidCrystal lcd(2, 3, 4, 5, 6, 7);
Adafruit_SHT31 sht31 = Adafruit_SHT31();

// Broches Arduino Mega 2560
const byte PIN_PLUS   = 22;
const byte PIN_MOINS  = 23;
const byte PIN_OK     = 24;
const byte PIN_BUZZER = 8;
const byte PIN_RELAIS = 53;

// Logique du module relais (HIGH = actif a l'etat haut).
// Module "actif bas" : inverser les deux valeurs.
const byte RELAIS_ON  = HIGH;
const byte RELAIS_OFF = LOW;

// Capteur SHT31 : 0x44 (ADR libre/GND) ou 0x45 (ADR au +VCC)
const byte ADRESSE_SHT31 = 0x44;
const byte ADRESSE_SHT31_ALT = 0x45;

// =====================================================
// PARAMETRES REGLABLES PAR L'UTILISATEUR (valeurs par defaut)
// =====================================================

float consigne = 40.0;
float tolerance = 1.0;
unsigned int dureeMinutes = 30;

const float TEMP_MIN = 5.0;
const float TEMP_MAX = 80.0;
const float PAS_TEMP = 0.5;

const float TOL_MIN = 0.5;
const float TOL_MAX = 5.0;
const float PAS_TOL = 0.5;

const unsigned int DUREE_MIN = 30;     // minutes
const unsigned int DUREE_MAX = 1440;   // minutes (24 h)
const unsigned int PAS_DUREE = 30;     // minutes

// =====================================================
// PARAMETRES DE SECURITE (a adapter a votre installation)
// =====================================================

// Limite absolue : au-dela, alarme dans TOUS les etats.
const float TEMP_SECURITE_ABS = 85.0;

// Pendant un cycle : alarme si T >= consigne + tolerance + marge.
// Augmenter si votre systeme a une forte inertie thermique.
const float MARGE_SURCHAUFFE = 5.0;

// Pendant un cycle : alarme si le capteur est en defaut depuis ce delai.
const unsigned long TIMEOUT_CAPTEUR = 30000UL;

// Chauffe inefficace : relais ON en continu sans gagner HAUSSE_MIN degres
// pendant ce delai => alarme (resistance HS, fusible, relais...).
// Mettre 0 pour desactiver.
const unsigned long DUREE_SANS_HAUSSE = 1200000UL;   // 20 min
const float HAUSSE_MIN = 1.0;

// Delai minimal entre deux commutations de la regulation.
const unsigned long DELAI_MIN_COMMUTATION = 5000UL;

// Pause : le temps restant est reinitialise apres 5 min,
// le cycle est abandonne apres 30 min.
const unsigned long TIMEOUT_PAUSE = 300000UL;
const unsigned long TIMEOUT_PAUSE_ABANDON = 1800000UL;

// Correction d'etalonnage de la temperature (en degres C)
const float OFFSET_TEMP = 0.0;

// =====================================================
// TEMPORISATIONS ET INTERFACE
// =====================================================

const unsigned long PERIODE_MESURE = 1000UL;
const unsigned long PERIODE_AFFICHAGE = 250UL;
const unsigned long PERIODE_REINIT_LCD = 30000UL;
const unsigned long TIMEOUT_MENU = 60000UL;

const unsigned long ANTI_REBOND = 35UL;
const unsigned long APPUI_LONG = 1500UL;

// Auto-repetition des touches + et - en edition
const unsigned long REPETE_DELAI = 450UL;
const unsigned long REPETE_PERIODE = 150UL;
const unsigned long REPETE_PERIODE_RAPIDE = 60UL;
const byte REPETE_RAPIDE_APRES = 12;

// Buzzer
const byte NB_BIPS_FIN = 6;
const unsigned long DUREE_MAX_ALARME_SONORE = 120000UL;
const bool BIP_TOUCHES = true;

// =====================================================
// EEPROM
// =====================================================

const uint16_t REGLAGES_MAGIQUE = 0x7E57;
const uint8_t REGLAGES_VERSION = 2;
const int ADRESSE_REGLAGES = 0;

// =====================================================
// BOUTONS
// =====================================================

Bouton boutonPlus  = {PIN_PLUS,  HIGH, HIGH, 0, 0, 0, 0, false};
Bouton boutonMoins = {PIN_MOINS, HIGH, HIGH, 0, 0, 0, 0, false};
Bouton boutonOK    = {PIN_OK,    HIGH, HIGH, 0, 0, 0, 0, false};

// Evenements :
//  EV_APPUI  : appui court valide au relachement
//  EV_LONG   : appui long (une seule fois)
//  EV_REPETE : a l'enfoncement, puis en auto-repetition
const byte EV_APPUI  = 1;
const byte EV_LONG   = 2;
const byte EV_REPETE = 4;

byte lireBouton(Bouton &b, unsigned long maintenant) {
  byte evenement = 0;
  bool lecture = digitalRead(b.pin);

  if (lecture != b.etatBrut) {
    b.etatBrut = lecture;
    b.dernierChangement = maintenant;
  }

  if (maintenant - b.dernierChangement >= ANTI_REBOND &&
      b.etatStable != b.etatBrut) {
    b.etatStable = b.etatBrut;

    if (b.etatStable == LOW) {
      b.debutAppui = maintenant;
      b.dernierRepete = maintenant;
      b.nbRepetes = 0;
      b.longSignale = false;
      evenement |= EV_REPETE;
    } else if (!b.longSignale) {
      evenement |= EV_APPUI;
    }
  }

  if (b.etatStable == LOW) {
    if (!b.longSignale && maintenant - b.debutAppui >= APPUI_LONG) {
      b.longSignale = true;
      evenement |= EV_LONG;
    }

    unsigned long delai;
    if (b.nbRepetes == 0) {
      delai = REPETE_DELAI;
    } else if (b.nbRepetes < REPETE_RAPIDE_APRES) {
      delai = REPETE_PERIODE;
    } else {
      delai = REPETE_PERIODE_RAPIDE;
    }

    if (maintenant - b.dernierRepete >= delai) {
      b.dernierRepete = maintenant;
      if (b.nbRepetes < 255) b.nbRepetes++;
      evenement |= EV_REPETE;
    }
  }

  return evenement;
}

// =====================================================
// ETAT DU PROGRAMME
// =====================================================

Ecran ecran = ACCUEIL;
Defaut defaut = DEFAUT_AUCUN;

const byte NB_OPTIONS = 5;
byte optionMenu = 0;
bool lancerOui = true;

// Mesures
float temperature = NAN;
float humidite = NAN;
bool capteurOK = false;
unsigned long derniereMesureValide = 0;

// Relais
bool chauffageActif = false;
unsigned long dernierCommutation = 0;
unsigned long refHausseTemps = 0;
float refHausseTemp = 0.0;

// Minuterie
unsigned long tempsRestant = 0;
unsigned long dernierTick = 0;
unsigned long debutPause = 0;
bool pauseDejaReinitialisee = false;

// Boucle
unsigned long derniereMesure = 0;
unsigned long dernierAffichage = 0;
unsigned long derniereActivite = 0;
unsigned long dernierReinitLCD = 0;
bool besoinAffichage = true;

// Buzzer
ModeBip modeBip = BIP_AUCUN;
bool bipSon = false;
unsigned long debutPhaseBip = 0;
unsigned long debutAlarme = 0;
byte compteurBip = 0;

// Message temporaire superpose a l'ecran
bool msgActif = false;
char msgLigne1[17];
char msgLigne2[17];
unsigned long msgDebut = 0;
unsigned long msgDuree = 0;

// Caracteres personnalises (codes 1 a 4 : le code 0 est evite
// car il termine les chaines C)
byte glypheChauffe[8] = {0b00100, 0b00100, 0b01010, 0b01010,
                         0b10001, 0b10101, 0b01110, 0b00000};
byte glypheHorloge[8] = {0b00000, 0b01110, 0b10101, 0b10111,
                         0b10001, 0b01110, 0b00000, 0b00000};
byte glypheCloche[8]  = {0b00100, 0b01110, 0b01110, 0b01110,
                         0b11111, 0b00000, 0b00100, 0b00000};
byte glypheCoche[8]   = {0b00000, 0b00001, 0b00011, 0b10110,
                         0b11100, 0b01000, 0b00000, 0b00000};

// =====================================================
// OUTILS GENERAUX
// =====================================================

bool cycleEnCours() {
  return (ecran == CYCLE || ecran == PAUSE);
}

bool enEdition() {
  return (ecran == EDIT_TEMP ||
          ecran == EDIT_DUREE ||
          ecran == EDIT_TOLERANCE);
}

unsigned long dureeEnMillis() {
  return (unsigned long)dureeMinutes * 60000UL;
}

// Ecrit exactement 16 caracteres (tronque ou complete par des espaces).
void ligneLCD(byte ligne, const char *texte) {
  lcd.setCursor(0, ligne);

  byte n = 0;
  while (texte[n] != '\0' && n < 16) {
    lcd.write((uint8_t)texte[n]);
    n++;
  }
  while (n < 16) {
    lcd.write((uint8_t)' ');
    n++;
  }
}

void initialiserLCD() {
  lcd.begin(16, 2);
  lcd.createChar(1, glypheChauffe);
  lcd.createChar(2, glypheHorloge);
  lcd.createChar(3, glypheCloche);
  lcd.createChar(4, glypheCoche);
  lcd.clear();
  dernierReinitLCD = millis();
}

void afficherMessage(const char *l1, const char *l2,
                     unsigned long duree) {
  strncpy(msgLigne1, l1, 16);
  msgLigne1[16] = '\0';
  strncpy(msgLigne2, l2, 16);
  msgLigne2[16] = '\0';

  msgDebut = millis();
  msgDuree = duree;
  msgActif = true;
  besoinAffichage = true;
}

// Duree configuree : HHhMM (ex. 01h30, 24h00)
void formaterDureeHHMM(unsigned int totalMinutes,
                       char *destination,
                       size_t taille) {
  snprintf(destination, taille, "%02uh%02u",
           totalMinutes / 60, totalMinutes % 60);
}

// Temps restant : MM:SS (< 1 h) ou HH:MM:SS
void formaterTemps(unsigned long millisecondes,
                   char *destination,
                   size_t taille) {
  unsigned long secondes = (millisecondes + 999UL) / 1000UL;

  unsigned long heures = secondes / 3600UL;
  unsigned long minutes = (secondes % 3600UL) / 60UL;
  unsigned long sec = secondes % 60UL;

  if (heures > 0) {
    snprintf(destination, taille, "%02lu:%02lu:%02lu",
             heures, minutes, sec);
  } else {
    snprintf(destination, taille, "%02lu:%02lu", minutes, sec);
  }
}

int humiditeAffichee() {
  int h = (int)(humidite + 0.5);
  if (h < 0) h = 0;
  if (h > 99) h = 99;
  return h;
}

// =====================================================
// EEPROM : SAUVEGARDE / CHARGEMENT AVEC CRC
// =====================================================

uint16_t calculerCRC(const uint8_t *donnees, size_t taille) {
  uint16_t crc = 0xFFFF;

  for (size_t i = 0; i < taille; i++) {
    crc ^= donnees[i];
    for (byte b = 0; b < 8; b++) {
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001)
                      : (uint16_t)(crc >> 1);
    }
  }

  return crc;
}

void sauvegarderReglages() {
  Reglages r;
  memset(&r, 0, sizeof(r));

  r.magique = REGLAGES_MAGIQUE;
  r.version = REGLAGES_VERSION;
  r.consigne = consigne;
  r.tolerance = tolerance;
  r.duree = dureeMinutes;
  r.crc = calculerCRC((const uint8_t *)&r,
                      sizeof(r) - sizeof(r.crc));

  // put() n'ecrit que les octets modifies (limite l'usure).
  EEPROM.put(ADRESSE_REGLAGES, r);
}

bool chargerReglages() {
  Reglages r;
  EEPROM.get(ADRESSE_REGLAGES, r);

  if (r.magique != REGLAGES_MAGIQUE ||
      r.version != REGLAGES_VERSION) {
    return false;
  }

  uint16_t crc = calculerCRC((const uint8_t *)&r,
                             sizeof(r) - sizeof(r.crc));
  if (crc != r.crc) {
    return false;
  }

  if (isnan(r.consigne) || isnan(r.tolerance) ||
      r.consigne < TEMP_MIN || r.consigne > TEMP_MAX ||
      r.tolerance < TOL_MIN || r.tolerance > TOL_MAX ||
      r.duree < DUREE_MIN || r.duree > DUREE_MAX ||
      (r.duree % PAS_DUREE) != 0) {
    return false;
  }

  consigne = r.consigne;
  tolerance = r.tolerance;
  dureeMinutes = r.duree;
  return true;
}

// =====================================================
// BUZZER (non bloquant)
// =====================================================

void arreterBip() {
  noTone(PIN_BUZZER);
  modeBip = BIP_AUCUN;
  bipSon = false;
}

void demarrerBip(ModeBip mode) {
  arreterBip();

  modeBip = mode;
  compteurBip = 0;
  bipSon = true;
  debutPhaseBip = millis();

  tone(PIN_BUZZER, mode == BIP_ALARME ? 3000 : 2200);
}

void gererBip(unsigned long maintenant) {
  if (modeBip == BIP_AUCUN) {
    return;
  }

  unsigned long duree = (modeBip == BIP_ALARME) ? 200UL : 300UL;

  if (maintenant - debutPhaseBip < duree) {
    return;
  }

  debutPhaseBip = maintenant;

  if (bipSon) {
    noTone(PIN_BUZZER);
    bipSon = false;

    if (modeBip == BIP_FIN) {
      compteurBip++;
      if (compteurBip >= NB_BIPS_FIN) {
        modeBip = BIP_AUCUN;
      }
    }
  } else {
    if (modeBip == BIP_ALARME &&
        maintenant - debutAlarme >= DUREE_MAX_ALARME_SONORE) {
      modeBip = BIP_AUCUN;
      return;
    }

    tone(PIN_BUZZER, modeBip == BIP_ALARME ? 3000 : 2200);
    bipSon = true;
  }
}

// =====================================================
// RELAIS ET REGULATION
// =====================================================

// Ecrit TOUJOURS la broche (reaffirme l'etat voulu).
// Une coupure (marche = false) est immediate, sans delai mini.
void commanderRelais(bool marche) {
  if (marche != chauffageActif) {
    chauffageActif = marche;
    dernierCommutation = millis();

    if (marche) {
      refHausseTemps = dernierCommutation;
      refHausseTemp = temperature;
    }
  }

  digitalWrite(PIN_RELAIS, chauffageActif ? RELAIS_ON : RELAIS_OFF);
}

void actualiserChauffage(unsigned long maintenant) {
  if (!cycleEnCours() || !capteurOK || isnan(temperature)) {
    commanderRelais(false);
    return;
  }

  bool delaiOk =
      (maintenant - dernierCommutation >= DELAI_MIN_COMMUTATION);

  if (!chauffageActif &&
      temperature <= consigne - tolerance &&
      delaiOk) {
    commanderRelais(true);
  } else if (chauffageActif &&
             temperature >= consigne + tolerance &&
             delaiOk) {
    commanderRelais(false);
  }
}

// =====================================================
// DEFAUTS ET SECURITES
// =====================================================

void declencherDefaut(Defaut d) {
  commanderRelais(false);

  defaut = d;
  ecran = ALARME;
  msgActif = false;
  debutAlarme = millis();
  tempsRestant = dureeEnMillis();

  demarrerBip(BIP_ALARME);
  besoinAffichage = true;
}

void verifierSecurites(unsigned long maintenant) {
  if (ecran == ALARME) {
    commanderRelais(false);
    return;
  }

  if (capteurOK && !isnan(temperature)) {
    // 1) Limite absolue, quel que soit l'etat.
    if (temperature >= TEMP_SECURITE_ABS) {
      declencherDefaut(DEFAUT_SURCHAUFFE);
      return;
    }

    if (cycleEnCours()) {
      // 2) Depassement important de la consigne.
      if (temperature >= consigne + tolerance + MARGE_SURCHAUFFE) {
        declencherDefaut(DEFAUT_SURCHAUFFE);
        return;
      }

      // 3) Chauffe sans effet sur la temperature.
      if (DUREE_SANS_HAUSSE > 0 && chauffageActif) {
        if (temperature >= refHausseTemp + HAUSSE_MIN) {
          refHausseTemp = temperature;
          refHausseTemps = maintenant;
        } else if (maintenant - refHausseTemps >= DUREE_SANS_HAUSSE) {
          declencherDefaut(DEFAUT_CHAUFFE);
          return;
        }
      }
    }
  }

  // 4) Capteur en defaut trop longtemps pendant un cycle.
  if (cycleEnCours() && !capteurOK &&
      maintenant - derniereMesureValide >= TIMEOUT_CAPTEUR) {
    declencherDefaut(DEFAUT_CAPTEUR);
  }
}

// =====================================================
// CAPTEUR SHT31
// =====================================================

bool detecterCapteur() {
  if (sht31.begin(ADRESSE_SHT31)) {
    return true;
  }
  return sht31.begin(ADRESSE_SHT31_ALT);
}

void mesurerCapteur(unsigned long maintenant) {
  float t = sht31.readTemperature();
  float h = sht31.readHumidity();

  if (isnan(t) || isnan(h) ||
      t < -40.0 || t > 125.0 ||
      h < 0.0 || h > 100.0) {
    capteurOK = false;
    temperature = NAN;
    humidite = NAN;

    commanderRelais(false);
    return;
  }

  temperature = t + OFFSET_TEMP;
  humidite = h;
  capteurOK = true;
  derniereMesureValide = maintenant;

  actualiserChauffage(maintenant);
}

// =====================================================
// MINUTERIE ET CYCLE
// =====================================================

void demarrerCycle() {
  unsigned long maintenant = millis();

  arreterBip();

  tempsRestant = dureeEnMillis();
  dernierTick = maintenant;
  debutPause = 0;
  pauseDejaReinitialisee = false;
  derniereMesureValide = maintenant;

  ecran = CYCLE;

  commanderRelais(false);
  actualiserChauffage(maintenant);
  besoinAffichage = true;
}

void mettreEnPause() {
  ecran = PAUSE;

  debutPause = millis();
  dernierTick = debutPause;
  pauseDejaReinitialisee = false;

  // Minuteur suspendu, regulation thermique maintenue.
  besoinAffichage = true;
}

void reprendreCycle() {
  ecran = CYCLE;

  dernierTick = millis();
  debutPause = 0;

  actualiserChauffage(dernierTick);
  besoinAffichage = true;
}

void abandonnerCycle() {
  commanderRelais(false);
  arreterBip();

  tempsRestant = dureeEnMillis();

  ecran = ACCUEIL;
  debutPause = 0;
  pauseDejaReinitialisee = false;
  besoinAffichage = true;
}

void gererMinuterie(unsigned long maintenant) {
  if (ecran == PAUSE) {
    if (!pauseDejaReinitialisee &&
        maintenant - debutPause >= TIMEOUT_PAUSE) {
      tempsRestant = dureeEnMillis();
      pauseDejaReinitialisee = true;
    }

    if (maintenant - debutPause >= TIMEOUT_PAUSE_ABANDON) {
      abandonnerCycle();
      afficherMessage("Pause trop longue", "Cycle arrete", 4000);
      demarrerBip(BIP_FIN);
    }

    return;
  }

  if (ecran != CYCLE) {
    return;
  }

  unsigned long ecoule = maintenant - dernierTick;
  dernierTick = maintenant;

  if (ecoule >= tempsRestant) {
    tempsRestant = 0;

    commanderRelais(false);
    ecran = TERMINE;

    demarrerBip(BIP_FIN);
    besoinAffichage = true;
  } else {
    tempsRestant -= ecoule;
  }
}

// =====================================================
// AFFICHAGE
// =====================================================

void afficherAccueil() {
  char l1[24];
  char l2[24];
  char t[10];
  char c[10];
  char d[10];

  if (!capteurOK) {
    ligneLCD(0, "ERREUR CAPTEUR");
    ligneLCD(1, "Verifier cablage");
    return;
  }

  dtostrf(temperature, 1, 1, t);
  dtostrf(consigne, 1, 1, c);
  dtostrf(tolerance, 1, 1, d);

  snprintf(l1, sizeof(l1), "T:%s" STR_DEGRE "C H:%d%%",
           t, humiditeAffichee());
  snprintf(l2, sizeof(l2), "C:%s +-%s OK" STR_FLECHE, c, d);

  ligneLCD(0, l1);
  ligneLCD(1, l2);
}

void afficherCycle(unsigned long maintenant) {
  char l1[24];
  char l2[24];
  char t[10];
  char c[10];
  char temps[12];
  char gauche[16];

  formaterTemps(tempsRestant, temps, sizeof(temps));

  if (!capteurOK) {
    snprintf(l1, sizeof(l1), "ERREUR CAPTEUR");
  } else {
    dtostrf(temperature, 1, 1, t);
    dtostrf(consigne, 1, 1, c);
    snprintf(l1, sizeof(l1), "%s/%s" STR_DEGRE "C H%d%%",
             t, c, humiditeAffichee());
  }

  if (ecran == PAUSE) {
    // Le temps clignote pendant la pause.
    bool masque = ((maintenant / 500UL) % 2UL) == 1UL;

    if (strlen(temps) <= 5) {
      snprintf(gauche, sizeof(gauche), "PAUSE %s",
               masque ? "     " : temps);
    } else {
      snprintf(gauche, sizeof(gauche), "|| %s",
               masque ? "        " : temps);
    }
  } else {
    snprintf(gauche, sizeof(gauche), "%s %s", STR_HORLOGE, temps);
  }

  const char *droite = chauffageActif ? (STR_CHAUFFE " ON") : " OFF";

  snprintf(l2, sizeof(l2), "%-12s%s", gauche, droite);

  ligneLCD(0, l1);
  ligneLCD(1, l2);
}

void afficherTermine() {
  char l1[24];
  char t[10];

  if (capteurOK) {
    dtostrf(temperature, 1, 1, t);
  } else {
    strcpy(t, "--");
  }

  snprintf(l1, sizeof(l1), "%s FIN T:%s" STR_DEGRE "C", STR_CLOCHE, t);

  ligneLCD(0, l1);
  ligneLCD(1, "OK=accueil");
}

const char *texteDefaut() {
  switch (defaut) {
    case DEFAUT_SURCHAUFFE: return "SURCHAUFFE";
    case DEFAUT_CAPTEUR:    return "CAPTEUR ABSENT";
    case DEFAUT_CHAUFFE:    return "PAS DE CHAUFFE";
    default:                return "DEFAUT";
  }
}

void afficherAlarme(unsigned long maintenant) {
  char l1[24];
  bool visible = ((maintenant / 500UL) % 2UL) == 0UL;

  snprintf(l1, sizeof(l1), "%s %s",
           visible ? STR_CLOCHE : " ", texteDefaut());

  ligneLCD(0, l1);
  ligneLCD(1, "OK=acquitter");
}

void afficherMenu() {
  const char *noms[NB_OPTIONS] = {
    "Consigne", "Duree", "Tolerance", "Lancer", "Retour"
  };

  char l1[24];
  char l2[24];
  char valeur[10];

  snprintf(l1, sizeof(l1), ">%-11s %d/%d",
           noms[optionMenu], optionMenu + 1, NB_OPTIONS);

  switch (optionMenu) {
    case 0:
      dtostrf(consigne, 1, 1, valeur);
      snprintf(l2, sizeof(l2), "%s" STR_DEGRE "C OK=modif", valeur);
      break;

    case 1:
      formaterDureeHHMM(dureeMinutes, valeur, sizeof(valeur));
      snprintf(l2, sizeof(l2), "%s OK=modif", valeur);
      break;

    case 2:
      dtostrf(tolerance, 1, 1, valeur);
      snprintf(l2, sizeof(l2), "+-%s" STR_DEGRE "C OK=modif", valeur);
      break;

    case 3:
      snprintf(l2, sizeof(l2), "OK=lancer");
      break;

    default:
      snprintf(l2, sizeof(l2), "OK=accueil");
      break;
  }

  ligneLCD(0, l1);
  ligneLCD(1, l2);
}

// "-" a gauche, "+" a droite, valeur centree.
void construireLigneEdition(char *destination, const char *valeur) {
  byte longueur = strlen(valeur);
  if (longueur > 14) longueur = 14;

  memset(destination, ' ', 16);
  destination[0] = '-';
  destination[15] = '+';
  memcpy(destination + (16 - longueur) / 2, valeur, longueur);
  destination[16] = '\0';
}

void afficherEdition() {
  char l1[24];
  char l2[24];
  char valeur[16];
  const char *titre;

  switch (ecran) {
    case EDIT_TEMP:
      titre = "Consigne";
      dtostrf(consigne, 1, 1, valeur);
      strcat(valeur, STR_DEGRE "C");
      break;

    case EDIT_DUREE:
      titre = "Duree";
      formaterDureeHHMM(dureeMinutes, valeur, sizeof(valeur));
      break;

    case EDIT_TOLERANCE:
      titre = "Tolerance";
      strcpy(valeur, "+-");
      dtostrf(tolerance, 1, 1, valeur + 2);
      strcat(valeur, STR_DEGRE "C");
      break;

    default:
      return;
  }

  snprintf(l1, sizeof(l1), "%-12s[OK]", titre);
  construireLigneEdition(l2, valeur);

  ligneLCD(0, l1);
  ligneLCD(1, l2);
}

void afficherConfirmation() {
  char l1[24];
  char l2[24];
  char c[10];
  char d[10];
  char duree[10];

  dtostrf(consigne, 1, 1, c);
  dtostrf(tolerance, 1, 1, d);
  formaterDureeHHMM(dureeMinutes, duree, sizeof(duree));

  snprintf(l1, sizeof(l1), "%s" STR_DEGRE "C +-%s Go?", c, d);
  snprintf(l2, sizeof(l2), "%s %s", duree,
           lancerOui ? "[OUI] NON" : " OUI [NON]");

  ligneLCD(0, l1);
  ligneLCD(1, l2);
}

void afficherEcran(unsigned long maintenant) {
  if (msgActif) {
    if (maintenant - msgDebut < msgDuree) {
      ligneLCD(0, msgLigne1);
      ligneLCD(1, msgLigne2);
      return;
    }
    msgActif = false;
  }

  switch (ecran) {
    case ACCUEIL:
      afficherAccueil();
      break;

    case MENU:
      afficherMenu();
      break;

    case EDIT_TEMP:
    case EDIT_DUREE:
    case EDIT_TOLERANCE:
      afficherEdition();
      break;

    case CONFIRMATION:
      afficherConfirmation();
      break;

    case CYCLE:
    case PAUSE:
      afficherCycle(maintenant);
      break;

    case TERMINE:
      afficherTermine();
      break;

    case ALARME:
      afficherAlarme(maintenant);
      break;
  }
}

// =====================================================
// ACTIONS DES BOUTONS
// =====================================================

void actionPlus() {
  switch (ecran) {
    case MENU:
      optionMenu = (optionMenu + 1) % NB_OPTIONS;
      break;

    case EDIT_TEMP:
      consigne += PAS_TEMP;
      if (consigne > TEMP_MAX) consigne = TEMP_MAX;
      break;

    case EDIT_DUREE:
      if (dureeMinutes + PAS_DUREE <= DUREE_MAX) {
        dureeMinutes += PAS_DUREE;
      }
      break;

    case EDIT_TOLERANCE:
      tolerance += PAS_TOL;
      if (tolerance > TOL_MAX) tolerance = TOL_MAX;
      break;

    case CONFIRMATION:
      lancerOui = !lancerOui;
      break;

    default:
      break;
  }
}

void actionMoins() {
  switch (ecran) {
    case MENU:
      optionMenu = (optionMenu + NB_OPTIONS - 1) % NB_OPTIONS;
      break;

    case EDIT_TEMP:
      consigne -= PAS_TEMP;
      if (consigne < TEMP_MIN) consigne = TEMP_MIN;
      break;

    case EDIT_DUREE:
      if (dureeMinutes >= DUREE_MIN + PAS_DUREE) {
        dureeMinutes -= PAS_DUREE;
      }
      break;

    case EDIT_TOLERANCE:
      tolerance -= PAS_TOL;
      if (tolerance < TOL_MIN) tolerance = TOL_MIN;
      break;

    case CONFIRMATION:
      lancerOui = !lancerOui;
      break;

    default:
      break;
  }
}

void actionOK() {
  switch (ecran) {
    case ACCUEIL:
      optionMenu = 0;
      ecran = MENU;
      break;

    case MENU:
      switch (optionMenu) {
        case 0: ecran = EDIT_TEMP; break;
        case 1: ecran = EDIT_DUREE; break;
        case 2: ecran = EDIT_TOLERANCE; break;
        case 3:
          lancerOui = true;
          ecran = CONFIRMATION;
          break;
        default:
          ecran = ACCUEIL;
          break;
      }
      break;

    case EDIT_TEMP:
    case EDIT_DUREE:
    case EDIT_TOLERANCE:
      sauvegarderReglages();
      ecran = MENU;
      afficherMessage("Reglage", STR_COCHE " enregistre", 700);
      break;

    case CONFIRMATION:
      if (!lancerOui) {
        ecran = MENU;
      } else if (!capteurOK || isnan(temperature)) {
        ecran = MENU;
        afficherMessage("Capteur absent!", "Cycle refuse", 2500);
      } else {
        demarrerCycle();
      }
      break;

    case CYCLE:
      mettreEnPause();
      break;

    case PAUSE:
      reprendreCycle();
      break;

    case TERMINE:
      arreterBip();
      ecran = ACCUEIL;
      break;

    case ALARME:
      arreterBip();
      defaut = DEFAUT_AUCUN;
      ecran = ACCUEIL;
      break;
  }
}

// =====================================================
// INITIALISATION
// =====================================================

void setup() {
  // Relais coupe en priorite.
  digitalWrite(PIN_RELAIS, RELAIS_OFF);
  pinMode(PIN_RELAIS, OUTPUT);
  digitalWrite(PIN_RELAIS, RELAIS_OFF);
  chauffageActif = false;

  pinMode(PIN_PLUS, INPUT_PULLUP);
  pinMode(PIN_MOINS, INPUT_PULLUP);
  pinMode(PIN_OK, INPUT_PULLUP);

  pinMode(PIN_BUZZER, OUTPUT);
  noTone(PIN_BUZZER);

  initialiserLCD();
  ligneLCD(0, "Thermostat SHT31");
  ligneLCD(1, "v" VERSION_LOGICIEL " demarrage");

  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  // Evite qu'un bus I2C bloque indefiniment le programme.
  Wire.setWireTimeout(25000UL, true);
#endif

  capteurOK = detecterCapteur();
  bool reglagesOk = chargerReglages();

  delay(900);

  ligneLCD(0, capteurOK ? "Capteur : OK" : "Capteur : ABSENT");
  ligneLCD(1, reglagesOk ? "Reglages charges" : "Reglages defaut");
  delay(1100);

  lcd.clear();

  unsigned long maintenant = millis();

  boutonPlus.etatBrut = boutonPlus.etatStable = digitalRead(PIN_PLUS);
  boutonMoins.etatBrut = boutonMoins.etatStable = digitalRead(PIN_MOINS);
  boutonOK.etatBrut = boutonOK.etatStable = digitalRead(PIN_OK);

  boutonPlus.dernierChangement = maintenant;
  boutonMoins.dernierChangement = maintenant;
  boutonOK.dernierChangement = maintenant;

  derniereMesure = maintenant;
  dernierAffichage = maintenant;
  derniereActivite = maintenant;
  dernierTick = maintenant;
  derniereMesureValide = maintenant;
  dernierReinitLCD = maintenant;
  dernierCommutation = maintenant - DELAI_MIN_COMMUTATION;

  tempsRestant = dureeEnMillis();

  // Reset par watchdog : un cycle a ete interrompu, on previent.
  if (causeReset & _BV(WDRF)) {
    afficherMessage("RESET WATCHDOG!", "Cycle interrompu", 5000);
    demarrerBip(BIP_FIN);
  }

#if defined(__AVR__)
  wdt_enable(WDTO_4S);
#endif
}

// =====================================================
// BOUCLE PRINCIPALE
// =====================================================

void loop() {
#if defined(__AVR__)
  wdt_reset();
#endif

  unsigned long maintenant = millis();

  byte evPlus = lireBouton(boutonPlus, maintenant);
  byte evMoins = lireBouton(boutonMoins, maintenant);
  byte evOK = lireBouton(boutonOK, maintenant);
  byte evTous = evPlus | evMoins | evOK;

  if (evTous) {
    derniereActivite = maintenant;
    msgActif = false;
    besoinAffichage = true;

    if (modeBip != BIP_AUCUN) {
      // Une touche coupe le bip en cours (l'alarme reste affichee).
      arreterBip();
    } else if (BIP_TOUCHES && (evTous & EV_REPETE)) {
      tone(PIN_BUZZER, 3500, 12);
    }
  }

  if ((evMoins & EV_LONG) && cycleEnCours()) {
    // Appui long sur MOINS : abandonner le cycle.
    abandonnerCycle();
    afficherMessage("Cycle annule", "Chauffage OFF", 2000);

  } else {
    if (enEdition()) {
      // Auto-repetition en edition.
      if (evPlus & EV_REPETE) actionPlus();
      if (evMoins & EV_REPETE) actionMoins();
    } else {
      if (evPlus & EV_APPUI) actionPlus();
      if (evMoins & EV_APPUI) actionMoins();
    }

    if (evOK & EV_APPUI) {
      actionOK();
    }
  }

  maintenant = millis();

  // Retour a l'accueil apres inactivite dans les menus.
  if ((ecran == MENU || enEdition() || ecran == CONFIRMATION) &&
      maintenant - derniereActivite >= TIMEOUT_MENU) {
    sauvegarderReglages();
    ecran = ACCUEIL;
    besoinAffichage = true;
  }

  // Mesure du capteur toutes les secondes.
  if (maintenant - derniereMesure >= PERIODE_MESURE) {
    derniereMesure = maintenant;

    if (!capteurOK) {
      capteurOK = detecterCapteur();
    }

    if (capteurOK) {
      mesurerCapteur(maintenant);
    } else {
      temperature = NAN;
      humidite = NAN;
      commanderRelais(false);
    }
  }

  verifierSecurites(maintenant);
  gererMinuterie(maintenant);

  // Garde-fou : jamais de chauffage hors cycle ou sans mesure valide.
  // Reecrit aussi la broche a chaque tour de boucle.
  if (chauffageActif && (!cycleEnCours() || !capteurOK)) {
    commanderRelais(false);
  } else {
    commanderRelais(chauffageActif);
  }

  maintenant = millis();
  gererBip(maintenant);

  // Re-initialisation periodique du LCD (parasites).
  if (maintenant - dernierReinitLCD >= PERIODE_REINIT_LCD) {
    initialiserLCD();
    besoinAffichage = true;
  }

  // Rafraichissement LCD.
  if (besoinAffichage ||
      maintenant - dernierAffichage >= PERIODE_AFFICHAGE) {
    dernierAffichage = maintenant;
    besoinAffichage = false;
    afficherEcran(maintenant);
  }
}
