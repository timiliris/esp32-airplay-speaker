# 🔊 ESP32 AirPlay Speaker

Transforme un **ESP32-S3** en enceinte **AirPlay 2** DIY, hautement personnalisable :
interface web façon Apple, traitement audio logiciel (EQ, limiteur, protection
HP), effets lumineux réactifs à la musique (bande LED adressable), et intégration
Home Assistant.

> **Fork** de [`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32)
> (le récepteur AirPlay 2, ESP-IDF v5.5) — voir [Crédits & Licence](#-crédits--licence).
> Ce dépôt ajoute une nouvelle UI web, un pipeline audio DSP, les effets LED ARGB,
> la protection HP / veille ampli, le mode mono, et une cible **XIAO ESP32-S3**.

---

## ✨ Fonctionnalités

**Audio**
- 📡 Récepteur **AirPlay 2** (Centre de contrôle, multiroom, sync PTP) — *upstream*
- 🔉 **Plafond de volume logiciel** (« niveau maximum ») pour protéger les HP
- 🎚️ **Égaliseur tonalité 3 bandes** (graves / médiums / aigus, ±12 dB) + **coupe-bas
  Butterworth 4ᵉ ordre** (24 dB/oct, jusqu'à 400 Hz) — idéal pour protéger des
  petits satellites du grave qu'ils ne tiennent pas
- 🛡️ **Limiteur anti-clipping** feed-forward (attaque ~3 ms / release ~150 ms)
- 🔀 **Mode canal de sortie** : Stéréo / **Mono (L+R)** / Gauche / Droite
  (parfait pour une enceinte mono)
- ⚡ **Veille ampli** : pilotage d'un GPIO SD/mute → coupe l'ampli à l'arrêt
  (anti-souffle, anti-pop, éco)

**Lumière**
- 🌈 **Bande LED adressable WS2812** (driver RMT) — **12 effets** réactifs à la
  musique + ambiance, **couleur** et **vitesse** réglables
  *(VU-mètre, Spectre, Pulsation basses, VU centre, Strobe basses, Niveau→couleur,
  Arc-en-ciel, Veilleuse, Couleur fixe, Respiration, Comète, Scintillement)*
- 🔲 **Matrice LED 8×8 MAX7219** (optionnelle, effets audio)

**Interface & intégration**
- 🍎 **Interface web** moderne (dark, frosted, onglets desktop + barre d'onglets mobile)
- 🔐 **Onboarding + mot de passe** sur tous les réglages sensibles, sessions à jeton
- ⬆️ **OTA** (mise à jour par le web) avec rollback, rechargement auto de la page au reboot
- 🏠 **Home Assistant** via **MQTT auto-discovery** (capteurs, volume, EQ, LED, restart…)

---

## 🧰 Matériel

| Carte | Build (env) | Flash / PSRAM | Notes |
|---|---|---|---|
| ESP32-S3 générique (N16R8) | `esp32s3` | 16 Mo / 8 Mo octale | DevKitC, WROOM-1 |
| **Seeed XIAO ESP32-S3** | `xiao-s3` | 8 Mo / 8 Mo octale | format vignette, USB-C, antenne U.FL |
| Cartes upstream (SqueezeAmp, Esparagus…) | voir `platformio.ini` | — | héritées du projet d'origine |

> ⚠️ AirPlay 2 nécessite de la **PSRAM** → uniquement des ESP32-**S3** (ou ESP32 classique
> avec PSRAM). Les ESP32-C3/C6 et les S3 sans PSRAM ne conviennent pas.

### DAC / Ampli audio (I2S)
PCM5102A, **MAX98357A** (ampli I2S mono), MAX98357 + ampli, etc.

**Brochage I2S par défaut**

| Signal | `esp32s3` (DevKit) | `xiao-s3` (XIAO) |
|---|---|---|
| BCLK | GPIO 11 | GPIO 1 (D0) |
| LRCLK (WS) | GPIO 13 | GPIO 2 (D1) |
| DIN (DO) | GPIO 12 | GPIO 4 (D3) |
| Ampli SD/enable | *(configurable)* | GPIO 5 (D4) |
| Bande LED (data) | *(configurable)* | GPIO 8 (D9) |

### ⚡ Alimentation (important pour un ampli classe D / une bande LED)
- Alimente l'**ampli** et la **bande LED** sur un **5 V externe dédié** (pas le 5 V
  tiré de l'USB, qui s'effondre sous charge → son faible + distorsion).
- Ajoute un **condensateur de découplage** (~470 µF) au plus près du Vin de l'ampli.
- 🔴 **Masse commune obligatoire** : GND alim ↔ GND ampli ↔ GND ESP (sinon l'I2S n'a
  pas de référence → son corrompu).
- Bande WS2812 en 5 V : le data 3,3 V de l'ESP peut nécessiter un **level-shifter**
  (74AHCT125) sur les longues bandes.

---

## ⚙️ Build & Flash (PlatformIO + ESP-IDF)

```bash
git clone --recursive https://github.com/timiliris/esp32-airplay-speaker.git
cd esp32-airplay-speaker
# (si déjà cloné sans --recursive) : git submodule update --init --recursive

# Compiler
pio run -e xiao-s3            # ou -e esp32s3

# Flasher — l'appli PUIS le filesystem, en DEUX commandes séparées :
pio run -e xiao-s3 -t upload          # bootloader + partitions + appli
pio run -e xiao-s3 -t uploadfs        # filesystem SPIFFS (interface web)
```

> ⚠️ **Ne pas** combiner `-t upload -t uploadfs` en une seule commande : les deux
> finissent par flasher le filesystem (piège PlatformIO/esp-idf). Lancez-les séparément.

Première mise en route : rejoindre le WiFi **`ESP32-AirPlay-Setup`** →
`http://192.168.4.1` → assistant (nom, mot de passe, WiFi).

---

## 🎛️ Interface web

4 onglets : **Lecture** (now-playing, volume, EQ) · **Effets** (bande LED / matrice) ·
**Réglages** (WiFi, appareil, boutons, protection & ampli, Home Assistant) ·
**Système** (infos, OTA, logs, redémarrage).

---

## 📁 Structure

```
main/            firmware (audio/, network/, rtsp/, hap/, led_argb.c, eq_dsp.c, ...)
data/www/        interface web (index.html autonome, logs, speedtest)
components/      composants ESP-IDF (DAC, boards, audio-resampler, u8g2[submodule])
sdkconfig.defaults.*   configs par cible (dont sdkconfig.defaults.xiao)
platformio.ini   environnements de build
```

---

## 📜 Crédits & Licence

Ce projet est un **fork** de **[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32)**
par **Rémi Bouteiller**, qui fournit le cœur récepteur AirPlay 2. Merci à lui 🙏
ainsi qu'aux bibliothèques tierces (u8g2, audio-resampler, etc.).

Distribué sous la **Non-Commercial License** d'origine (voir [`LICENSE`](LICENSE) et
[`NOTICE`](NOTICE)) : usage, copie, modification et distribution autorisés **à des
fins non commerciales uniquement**. Pour un usage commercial, contacter l'auteur
upstream.

> ⚠️ Projet personnel / DIY fourni « tel quel », sans garantie. AirPlay est une
> marque d'Apple Inc. ; ce projet n'est ni affilié ni approuvé par Apple.
