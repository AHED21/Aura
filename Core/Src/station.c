/* ==========================================================================
 *  STATION AURA : modes de fonctionnement, acquisition, enregistrement SD
 *  Basé sur le cahier des charges « Modes de fonctionnement » et le Livrable 2,
 *  avec les corrections de l'audit (fermeture SD en maintenance, rotation des
 *  fichiers, compteur de timeouts, acquisition interruptible, LED non bloquante).
 * ========================================================================== */
#include "station.h"
#include "fatfs.h"
#include "rgb_led.h"
#include "dht11.h"
#include "bh1750.h"
#include "lcd_i2c.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

extern I2C_HandleTypeDef  hi2c1;
extern RTC_HandleTypeDef  hrtc;
extern SPI_HandleTypeDef  hspi1;
extern UART_HandleTypeDef huart2;   /* console ST-LINK, 115200 */
extern UART_HandleTypeDef huart3;   /* GPS, 9600 */

/* --------------------------------------------------------------------------
 *  Réglages
 * -------------------------------------------------------------------------- */
/* Numéro de build et mode écrits par le script compiler_televerser (fichier généré) */
#if __has_include("version_build.h")
#include "version_build.h"
#endif
#ifndef BUILD_NUMERO
#define BUILD_NUMERO         "0"
#endif
#ifndef BUILD_MODE
#define BUILD_MODE           "debug"
#endif
#define VERSION              "AURA v1.0 build " BUILD_NUMERO " (" BUILD_MODE ")"
#define NUMERO_LOT           "LOT-2026-01"

/* Correspondance boutons du cahier des charges / broches (à échanger ici si besoin) */
#define BTN_ROUGE_PORT       GPIOC
#define BTN_ROUGE_PIN        GPIO_PIN_3      /* bouton externe */
#define BTN_VERT_PORT        GPIOC
#define BTN_VERT_PIN         GPIO_PIN_13     /* bouton bleu B1 de la carte */

#define APPUI_LONG_MS        5000U
/* Retour automatique en STANDARD après TIMEOUT_CONFIG_S secondes sans commande série
 * (cahier des charges : 30 min = 1800 s). */
#define TIMEOUT_CONFIG_S     1800UL
#define TIMEOUT_CONFIG_MS    (TIMEOUT_CONFIG_S * 1000UL)
#define INTERVALLE_MAINT_MS  2000U                    /* affichage en maintenance */

/* Ambiguïté du cahier des charges : depuis Économique, rouge 5 s mène à Standard
 * (règle détaillée, choix du Livrable 2). Mettre à 1 pour aller en Maintenance. */
#define ECO_ROUGE_VERS_MAINTENANCE 0

/* Paramètres sauvegardés dans la dernière page de la Flash (le STM32L476 n'a pas d'EEPROM) */
#define PARAM_ADRESSE        0x080FF800UL             /* banque 2, page 255 */
#define PARAM_MAGIC          0x57575734UL             /* "WWW4" : nouveaux défauts (fichiers de 2 Ko) */

typedef struct {
  int32_t log_interval_s;  /* secondes (LOG_INTERVAL=10 -> 600 s, LOG_INTERVAL=20S -> 20 s) */
  int32_t file_max_size;   /* octets */
  int32_t timeout;         /* secondes */
  int32_t lumin, lumin_low, lumin_high;
  int32_t temp_air, min_temp_air, max_temp_air;
  int32_t hygr, hygr_mint, hygr_maxt;
  int32_t gps;             /* 1 = GPS utilisé, 0 = GPS désactivé (position en NA, pas d'erreur GPS) */
} Parametres;

/* FILE_MAX_SIZE par défaut : 2 Ko (2048 octets), valeur du document « Modes de fonctionnement ».
 * À 10 min une journée fait ~7 Ko : le fichier 0 est archivé en _1 à _9 trois ou quatre fois par jour. */
static const Parametres PARAM_DEFAUT = { 600, 2048, 30, 1, 255, 768, 1, -10, 60, 1, 0, 50, 1 };

/* Texte de l'intervalle : "10 min" ou "20 s" */
static const char *txt_intervalle(int32_t s)
{
  static char t[16];
  if (s % 60 == 0) snprintf(t, sizeof t, "%ld min", (long)(s / 60)); else snprintf(t, sizeof t, "%ld s", (long)s);
  return t;
}
static Parametres P;

/* --------------------------------------------------------------------------
 *  Journal série
 * -------------------------------------------------------------------------- */
static char log_buf[200];
static void LOG(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(log_buf, sizeof log_buf, fmt, ap);
  va_end(ap);
  HAL_UART_Transmit(&huart2, (uint8_t *)log_buf, (uint16_t)strlen(log_buf), 200);
}

/* --------------------------------------------------------------------------
 *  Erreurs et LED (gérée dans l'interruption SysTick : jamais bloquée)
 * -------------------------------------------------------------------------- */
typedef enum {                /* ordre = priorité d'affichage */
  ERR_SD_ECRITURE, ERR_SD_PLEINE, ERR_RTC, ERR_CAPTEUR, ERR_GPS, ERR_INCOHERENTE, ERR_NB
} Erreur;

static volatile uint32_t erreurs = 0;           /* un bit par erreur active */
static volatile Mode     mode_actuel = MODE_STANDARD;
static Mode              mode_precedent = MODE_STANDARD;

static void erreur_set(Erreur e, int active)
{
  if (active) erreurs |= (1UL << e); else erreurs &= ~(1UL << e);
}

/* couleurs : bit 0 = rouge, bit 1 = vert, bit 2 = bleu */
#define C_ROUGE 1
#define C_VERT  2
#define C_BLEU  4
#define C_JAUNE (C_ROUGE | C_VERT)
#define C_BLANC (C_ROUGE | C_VERT | C_BLEU)

static uint32_t tick_led = 0;

static void led_afficher(uint8_t couleur, uint8_t orange)
{
  /* orange : rouge plein + vert à 30 % (MLI logicielle sur 10 ms) */
  uint8_t vert = (couleur & C_VERT) ? 1 : 0;
  if (orange) vert = ((tick_led % 10U) < 3U);
  RGB_LED_Set((couleur & C_ROUGE) ? 1 : 0, vert, (couleur & C_BLEU) ? 1 : 0);
}

static void led_tick(void)
{
  tick_led++;
  uint32_t err = erreurs;
  if (err)
  {
    /* Signal d'erreur à 1 Hz : rouge puis une 2e couleur (durée identique ou 2x plus longue) */
    static const uint8_t c2[ERR_NB]  = { C_BLANC, C_BLANC, C_BLEU, C_VERT, C_JAUNE, C_VERT };
    static const uint8_t long2[ERR_NB] = { 1, 0, 0, 0, 0, 1 };   /* 2e couleur 2 fois plus longue */
    int e = 0;
    while (!(err & (1UL << e))) e++;
    uint32_t phase = tick_led % 1000U;
    uint32_t limite = long2[e] ? 333U : 500U;
    led_afficher(phase < limite ? C_ROUGE : c2[e], 0);
    return;
  }
  switch (mode_actuel)
  {
    case MODE_STANDARD:      led_afficher(C_VERT, 0);  break;
    case MODE_CONFIGURATION: led_afficher(C_JAUNE, 0); break;
    case MODE_ECONOMIQUE:    led_afficher(C_BLEU, 0);  break;
    case MODE_MAINTENANCE:   led_afficher(C_ROUGE, 1); break;
  }
}

/* --------------------------------------------------------------------------
 *  Boutons : interruptions EXTI sur les deux fronts + détection de l'appui long
 *  dans l'interruption SysTick. Aucun changement de mode dans les interruptions :
 *  elles déposent un événement dans une file, traitée par la boucle principale.
 * -------------------------------------------------------------------------- */
typedef enum { EV_LONG_ROUGE = 1, EV_LONG_VERT = 2 } Evenement;

typedef struct {
  volatile uint8_t  enfonce;
  volatile uint8_t  signale;
  volatile uint32_t debut;
  volatile uint32_t dernier_front;
} Bouton;

static Bouton rouge, vert;
static volatile uint8_t file_ev[8];
static volatile uint8_t file_tete = 0, file_queue = 0;

static void file_ajouter(uint8_t ev)
{
  uint8_t suivant = (uint8_t)((file_tete + 1U) % 8U);
  if (suivant != file_queue) { file_ev[file_tete] = ev; file_tete = suivant; }
}

static uint8_t file_retirer(void)
{
  if (file_queue == file_tete) return 0;
  uint8_t ev = file_ev[file_queue];
  file_queue = (uint8_t)((file_queue + 1U) % 8U);
  return ev;
}

static void bouton_front(Bouton *b, GPIO_TypeDef *port, uint16_t pin)
{
  uint32_t t = HAL_GetTick();
  if (t - b->dernier_front < 20U) return;        /* anti-rebond 20 ms */
  b->dernier_front = t;
  if (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET)   /* actif à l'état bas */
  {
    if (!b->enfonce) { b->enfonce = 1; b->signale = 0; b->debut = t; }
  }
  else
  {
    b->enfonce = 0;
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == BTN_ROUGE_PIN) bouton_front(&rouge, BTN_ROUGE_PORT, BTN_ROUGE_PIN);
  if (GPIO_Pin == BTN_VERT_PIN)  bouton_front(&vert,  BTN_VERT_PORT,  BTN_VERT_PIN);
}

void EXTI3_IRQHandler(void)                     /* bouton rouge PC3 (non généré par CubeMX) */
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_3);
}

static void bouton_tick(Bouton *b, uint8_t ev)
{
  if (b->enfonce && !b->signale && (HAL_GetTick() - b->debut) >= APPUI_LONG_MS)
  {
    b->signale = 1;
    file_ajouter(ev);
  }
}

static volatile uint8_t station_active = 0;   /* 0 pendant le test matériel */

void Station_Tick1ms(void)
{
  if (!station_active) return;
  bouton_tick(&rouge, EV_LONG_ROUGE);
  bouton_tick(&vert,  EV_LONG_VERT);
  led_tick();
}

static void boutons_init(void)
{
  GPIO_InitTypeDef g = {0};
  /* PC3 et PC13 : interruption sur front descendant ET montant (appui et relâchement) */
  g.Pin = BTN_ROUGE_PIN; g.Mode = GPIO_MODE_IT_RISING_FALLING; g.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BTN_ROUGE_PORT, &g);
  g.Pin = BTN_VERT_PIN;  g.Mode = GPIO_MODE_IT_RISING_FALLING; g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(BTN_VERT_PORT, &g);
  HAL_NVIC_SetPriority(EXTI3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/* --------------------------------------------------------------------------
 *  Réception série par interruption : console (USART2) et GPS (USART3)
 * -------------------------------------------------------------------------- */
static uint8_t  rx2, rx3;
static char     cmd_buf[64];
static volatile uint8_t cmd_len = 0, cmd_prete = 0;

static char     gps_ligne[96];
static uint8_t  gps_len = 0;
static char     gps_rmc[96];                    /* dernière trame RMC complète */
static volatile uint32_t gps_rmc_tick = 0;      /* instant de réception, 0 = jamais */
static volatile uint32_t gps_trame_tick = 0;    /* dernière trame NMEA quelconque */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    char c = (char)rx2;
    if (!cmd_prete)
    {
      if (c == '\r' || c == '\n') { if (cmd_len) { cmd_buf[cmd_len] = '\0'; cmd_prete = 1; } }
      else if (cmd_len < sizeof cmd_buf - 1U) cmd_buf[cmd_len++] = c;
    }
    HAL_UART_Receive_IT(&huart2, &rx2, 1);
  }
  else if (huart->Instance == USART3)
  {
    char c = (char)rx3;
    if (c == '$') gps_len = 0;
    if (gps_len < sizeof gps_ligne - 1U) gps_ligne[gps_len++] = c;
    if (c == '\n' && gps_len > 6 && gps_ligne[0] == '$')
    {
      gps_ligne[gps_len] = '\0';
      gps_trame_tick = HAL_GetTick();
      if (strncmp(gps_ligne + 3, "RMC", 3) == 0) { memcpy(gps_rmc, gps_ligne, gps_len + 1U); gps_rmc_tick = HAL_GetTick(); }
      gps_len = 0;
    }
    HAL_UART_Receive_IT(&huart3, &rx3, 1);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)   /* débordement : on relance la réception */
{
  if (huart->Instance == USART2) HAL_UART_Receive_IT(&huart2, &rx2, 1);
  if (huart->Instance == USART3) HAL_UART_Receive_IT(&huart3, &rx3, 1);
}

void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&huart2); }
void USART3_IRQHandler(void) { HAL_UART_IRQHandler(&huart3); }

/* --------------------------------------------------------------------------
 *  Paramètres en Flash
 * -------------------------------------------------------------------------- */
typedef union {
  struct { uint32_t magic; Parametres p; uint32_t somme; } s;
  uint64_t mots[(sizeof(uint32_t) * 2 + sizeof(Parametres) + 7) / 8];
} BlocFlash;

static uint32_t somme_param(const Parametres *p)
{
  const uint8_t *b = (const uint8_t *)p;
  uint32_t s = 0x1234;
  for (size_t i = 0; i < sizeof *p; i++) s = s * 31U + b[i];
  return s;
}

static void param_charger(void)
{
  const BlocFlash *f = (const BlocFlash *)PARAM_ADRESSE;
  if (f->s.magic == PARAM_MAGIC && f->s.somme == somme_param(&f->s.p)) P = f->s.p;
  else P = PARAM_DEFAUT;
}

static int param_sauver(void)
{
  BlocFlash b;
  memset(&b, 0xFF, sizeof b);
  b.s.magic = PARAM_MAGIC; b.s.p = P; b.s.somme = somme_param(&P);

  FLASH_EraseInitTypeDef e = { .TypeErase = FLASH_TYPEERASE_PAGES, .Banks = FLASH_BANK_2, .Page = 255, .NbPages = 1 };
  uint32_t err_page = 0;
  int ok = 1;
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  if (HAL_FLASHEx_Erase(&e, &err_page) != HAL_OK) ok = 0;
  for (size_t i = 0; ok && i < sizeof b.mots / sizeof b.mots[0]; i++)
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, PARAM_ADRESSE + 8U * i, b.mots[i]) != HAL_OK) ok = 0;
  HAL_FLASH_Lock();
  return ok;
}

/* --------------------------------------------------------------------------
 *  Changements de mode
 * -------------------------------------------------------------------------- */
static const char *nom_mode(Mode m)
{
  switch (m)
  {
    case MODE_STANDARD:      return "STANDARD";
    case MODE_CONFIGURATION: return "CONFIGURATION";
    case MODE_MAINTENANCE:   return "MAINTENANCE";
    default:                 return "ECONOMIQUE";
  }
}

static uint8_t  sd_prete = 0;
static uint32_t prochaine_acq = 0;        /* instant de la prochaine acquisition */
static uint32_t derniere_activite = 0;    /* mode configuration */
static uint32_t compteur_gps = 0;
static LCD_I2C_t lcd;
static uint8_t  lcd_ok = 0;

static void sd_demonter(void)
{
  if (sd_prete) f_mount(NULL, USERPath, 0);
  sd_prete = 0;
}

static void config_aide(void);

static void changer_mode(Mode m)
{
  Mode ancien = mode_actuel;
  if (ancien == m) return;
  if (m == MODE_MAINTENANCE)
  {
    mode_precedent = ancien;
    sd_demonter();                         /* on peut retirer la carte sans risque */
    LOG("\r\n>>> Mode MAINTENANCE : carte SD retirable, mesures en direct ci-dessous\r\n");
  }
  mode_actuel = m;
  prochaine_acq = HAL_GetTick();           /* première acquisition tout de suite */
  if (m == MODE_CONFIGURATION) { derniere_activite = HAL_GetTick(); config_aide(); }
  else LOG("\r\n>>> Mode %s\r\n", nom_mode(m));
  if (ancien == MODE_MAINTENANCE) LOG("    (la carte SD sera remontee a la prochaine ecriture)\r\n");
}

/* Traite les événements des boutons. Retourne 1 si le mode a changé. */
static int gerer_evenements(void)
{
  uint8_t ev;
  Mode avant = mode_actuel;
  while ((ev = file_retirer()) != 0)
  {
    switch (mode_actuel)
    {
      case MODE_STANDARD:
        if (ev == EV_LONG_ROUGE) changer_mode(MODE_MAINTENANCE);
        else if (ev == EV_LONG_VERT) changer_mode(MODE_ECONOMIQUE);
        break;
      case MODE_ECONOMIQUE:
        if (ev == EV_LONG_ROUGE) changer_mode(ECO_ROUGE_VERS_MAINTENANCE ? MODE_MAINTENANCE : MODE_STANDARD);
        break;
      case MODE_MAINTENANCE:
        if (ev == EV_LONG_ROUGE) changer_mode(mode_precedent);
        break;
      case MODE_CONFIGURATION:               /* boutons ignorés */
        break;
    }
  }
  return mode_actuel != avant;
}

static void lcd_tick(void);

/* Attente qui reste réactive aux boutons et garde l'écran à jour.
 * Retourne 1 si le mode a changé pendant l'attente. */
static int attendre(uint32_t ms, Mode depart)
{
  uint32_t t0 = HAL_GetTick();
  while (HAL_GetTick() - t0 < ms)
  {
    gerer_evenements();
    if (mode_actuel != depart) return 1;
    lcd_tick();
  }
  return 0;
}

/* --------------------------------------------------------------------------
 *  Acquisition
 * -------------------------------------------------------------------------- */
typedef struct {
  RTC_TimeTypeDef heure; RTC_DateTypeDef date; uint8_t date_ok;
  int temp; uint8_t temp_ok;
  int hygro; uint8_t hygro_ok;
  unsigned lux; uint8_t lux_ok;
  char lat[16], lon[16]; uint8_t gps_ok;
} Mesures;

static Mesures M;
static BH1750_t bh;
static uint8_t  bh_ok = 0;
static uint8_t  nb_timeout_dht = 0, nb_timeout_lux = 0, nb_timeout_gps = 0;

typedef enum { ACQ_COMPLETE, ACQ_INTERROMPUE } ResultatAcq;

static void maj_erreur_capteur(void)
{
  erreur_set(ERR_CAPTEUR, nb_timeout_dht >= 2 || nb_timeout_lux >= 2);
}

/* Champ n de la trame NMEA (0 = "$GPRMC") */
static int nmea_champ(const char *trame, int n, char *out, size_t taille)
{
  const char *p = trame;
  for (int i = 0; i < n; i++) { p = strchr(p, ','); if (!p) return 0; p++; }
  size_t k = 0;
  while (*p && *p != ',' && *p != '*' && k < taille - 1U) out[k++] = *p++;
  out[k] = '\0';
  return (int)k;
}

/* Coordonnée NMEA (ddmm.mmmm ou dddmm.mmmm) vers degrés décimaux, en texte, sans flottant
 * (printf de newlib-nano ne gère pas %f). Ex. "3645.1234","N" -> "36.752057" */
static int coord_decimale(const char *nmea, int nb_deg, char hemi, char *out, size_t taille)
{
  const char *point = strchr(nmea, '.');
  if (!point || point - nmea != nb_deg + 2) return 0;
  long deg = 0, min_e4 = 0;
  for (int i = 0; i < nb_deg; i++) deg = deg * 10 + (nmea[i] - '0');
  /* minutes x 10000 : 2 chiffres avant le point, 4 après (complétés par des 0) */
  min_e4 = (nmea[nb_deg] - '0') * 10 + (nmea[nb_deg + 1] - '0');
  const char *d = point + 1;
  for (int i = 0; i < 4; i++) { min_e4 = min_e4 * 10 + ((*d >= '0' && *d <= '9') ? (*d++ - '0') : 0); }
  long frac_e6 = (min_e4 * 100L + 30L) / 60L;          /* minutes / 60, en millionièmes de degré */
  if (frac_e6 >= 1000000L) { deg++; frac_e6 -= 1000000L; }
  int negatif = (hemi == 'S' || hemi == 'W');
  snprintf(out, taille, "%s%ld.%06ld", negatif ? "-" : "", deg, frac_e6);
  return 1;
}

/* Fuseau horaire de la station : l'heure GPS est en UTC. Algérie et France (hiver) : UTC+1. */
#define DECALAGE_UTC_H  1

static uint8_t jours_mois(uint8_t mois, uint8_t annee)
{
  static const uint8_t j[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  return (mois == 2 && (annee % 4U) == 0U) ? 29U : j[mois - 1U];
}

/* Met la RTC à l'heure du GPS (trame RMC valide) : heure = champ 1, date = champ 9.
 * Fait une fois au démarrage, puis si la RTC dérive de plus de 2 s. */
static uint8_t rtc_synchro_gps = 0;
static void rtc_depuis_gps(const char *trame)
{
  char heure[12], date[8];
  if (nmea_champ(trame, 1, heure, sizeof heure) < 6 || nmea_champ(trame, 9, date, sizeof date) != 6) return;
  int h = (heure[0] - '0') * 10 + (heure[1] - '0');
  int m = (heure[2] - '0') * 10 + (heure[3] - '0');
  int s = (heure[4] - '0') * 10 + (heure[5] - '0');
  int jj = (date[0] - '0') * 10 + (date[1] - '0');
  int mo = (date[2] - '0') * 10 + (date[3] - '0');
  int aa = (date[4] - '0') * 10 + (date[5] - '0');
  if (h > 23 || m > 59 || s > 59 || jj < 1 || jj > 31 || mo < 1 || mo > 12) return;

  h += DECALAGE_UTC_H;                                  /* passage à l'heure locale */
  if (h >= 24) { h -= 24; if (++jj > jours_mois((uint8_t)mo, (uint8_t)aa)) { jj = 1; if (++mo > 12) { mo = 1; aa++; } } }

  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN); HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
  long ecart = ((long)t.Hours * 3600 + t.Minutes * 60 + t.Seconds) - ((long)h * 3600 + m * 60 + s);
  if (rtc_synchro_gps && d.Date == jj && d.Month == mo && d.Year == aa && ecart >= -2 && ecart <= 2) return;

  t.Hours = (uint8_t)h; t.Minutes = (uint8_t)m; t.Seconds = (uint8_t)s;
  t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE; t.StoreOperation = RTC_STOREOPERATION_RESET;
  d.Date = (uint8_t)jj; d.Month = (uint8_t)mo; d.Year = (uint8_t)aa;
  HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0x32F2);
  if (!rtc_synchro_gps) LOG("    horloge reglee sur le GPS : %02d/%02d/20%02d %02d:%02d:%02d\r\n", jj, mo, aa, h, m, s);
  rtc_synchro_gps = 1;
}

static ResultatAcq acquerir(int lire_gps)
{
  Mode depart = mode_actuel;
  uint32_t timeout_ms = (uint32_t)P.timeout * 1000U;
  memset(&M, 0, sizeof M);                 /* tous les indicateurs de validité à FAUX */

  /* 1. Horodatage RTC */
  int lecture_ok = (HAL_RTC_GetTime(&hrtc, &M.heure, RTC_FORMAT_BIN) == HAL_OK &&
                    HAL_RTC_GetDate(&hrtc, &M.date, RTC_FORMAT_BIN) == HAL_OK);
  erreur_set(ERR_RTC, !lecture_ok);
  /* Horloge jamais réglée (ni CLOCK/DATE, ni GPS) : date fausse (01/01/2000), on écrit NA */
  M.date_ok = lecture_ok && HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) == 0x32F2;
  static uint8_t avert_horloge = 0;
  if (lecture_ok && !M.date_ok && !avert_horloge)
  {
    avert_horloge = 1;
    LOG("    horloge non reglee : date et heure en NA jusqu au premier signal GPS valide\r\n"
        "    (ou regler en mode CONFIGURATION : CLOCK=HH:MM:SS et DATE=MM,JJ,AAAA)\r\n");
  }
  if (gerer_evenements() || mode_actuel != depart) return ACQ_INTERROMPUE;

  /* 2. DHT11 : température et hygrométrie, nouvel essai toutes les 2 s jusqu'à TIMEOUT */
  if (P.temp_air || P.hygr)
  {
    uint32_t t0 = HAL_GetTick();
    DHT11_Data d;
    int ok = 0;
    for (;;)
    {
      if (DHT11_Read(&d) == HAL_OK) { ok = 1; break; }
      if (HAL_GetTick() - t0 + 2000U > timeout_ms) break;
      if (attendre(2000, depart)) return ACQ_INTERROMPUE;
    }
    if (ok)
    {
      nb_timeout_dht = 0;
      int t = (int)d.temperature, h = (int)d.humidity;
      int incoherent = (t < P.min_temp_air || t > P.max_temp_air);
      erreur_set(ERR_INCOHERENTE, incoherent);
      if (P.temp_air && !incoherent) { M.temp = t; M.temp_ok = 1; }
      /* hygrométrie prise en compte seulement entre HYGR_MINT et HYGR_MAXT */
      if (P.hygr && t >= P.hygr_mint && t <= P.hygr_maxt) { M.hygro = h; M.hygro_ok = 1; }
    }
    else if (nb_timeout_dht < 255) nb_timeout_dht++;
    maj_erreur_capteur();
  }
  if (gerer_evenements() || mode_actuel != depart) return ACQ_INTERROMPUE;

  /* 3. Luminosité BH1750 (I2C1) */
  if (P.lumin)
  {
    uint32_t t0 = HAL_GetTick();
    float lux;
    int ok = 0;
    for (;;)
    {
      if (!bh_ok) bh_ok = (BH1750_Init(&bh, &hi2c1, BH1750_ADDR_LOW, BH1750_CONT_H) == HAL_OK);
      if (bh_ok && BH1750_ReadLux(&bh, &lux) == HAL_OK) { ok = 1; break; }
      bh_ok = 0;
      if (HAL_GetTick() - t0 + 500U > timeout_ms) break;
      if (attendre(500, depart)) return ACQ_INTERROMPUE;
    }
    if (ok) { nb_timeout_lux = 0; M.lux = (unsigned)lux; M.lux_ok = 1; }
    else if (nb_timeout_lux < 255) nb_timeout_lux++;
    maj_erreur_capteur();
  }
  if (gerer_evenements() || mode_actuel != depart) return ACQ_INTERROMPUE;

  /* 4. GPS : trame RMC reçue par interruption. Statut A = position valide.
   *    On attend une trame reçue après le début de l'acquisition, au plus TIMEOUT. */
  if (lire_gps)
  {
    uint32_t t0 = HAL_GetTick();
    while (!(gps_rmc_tick && gps_rmc_tick - t0 < 0x80000000UL) && HAL_GetTick() - t0 < timeout_ms)
    {
      if (attendre(100, depart)) return ACQ_INTERROMPUE;
    }
    if (gps_rmc_tick && gps_rmc_tick - t0 < 0x80000000UL)
    {
      char trame[96], statut[4], lat[12], ns[3], lon[13], ew[3];
      __disable_irq(); memcpy(trame, gps_rmc, sizeof trame); __enable_irq();
      nb_timeout_gps = 0;
      if (nmea_champ(trame, 2, statut, sizeof statut) && statut[0] == 'A' &&
          nmea_champ(trame, 3, lat, sizeof lat) && nmea_champ(trame, 4, ns, sizeof ns) &&
          nmea_champ(trame, 5, lon, sizeof lon) && nmea_champ(trame, 6, ew, sizeof ew))
      {
        /* position en degrés décimaux (directement utilisable dans Google Maps) */
        if (coord_decimale(lat, 2, ns[0], M.lat, sizeof M.lat) &&
            coord_decimale(lon, 3, ew[0], M.lon, sizeof M.lon))
          M.gps_ok = 1;                   /* sinon NA : jamais l'ancienne position */

        /* horloge réglée par le GPS, puis horodatage relu pour que la ligne ait la bonne heure */
        rtc_depuis_gps(trame);
        if (HAL_RTC_GetTime(&hrtc, &M.heure, RTC_FORMAT_BIN) == HAL_OK &&
            HAL_RTC_GetDate(&hrtc, &M.date, RTC_FORMAT_BIN) == HAL_OK) M.date_ok = 1;
      }
    }
    else if (nb_timeout_gps < 255) nb_timeout_gps++;
    erreur_set(ERR_GPS, nb_timeout_gps >= 2);
  }
  else if (!P.gps)                          /* GPS désactivé : pas d'erreur GPS */
  {
    nb_timeout_gps = 0;
    erreur_set(ERR_GPS, 0);
  }
  if (gerer_evenements() || mode_actuel != depart) return ACQ_INTERROMPUE;
  return ACQ_COMPLETE;
}

/* Mesures en clair sur le moniteur série (le fichier SD garde le format CSV) */
static const char *classe_lumiere(unsigned lux)
{
  if (lux < (unsigned)P.lumin_low)  return "faible";
  if (lux > (unsigned)P.lumin_high) return "forte";
  return "moyenne";
}

static void afficher_mesures(int detaille)
{
  char date[32] = "non reglee", pos[40] = "pas de signal GPS", t[16] = "NA", h[16] = "NA", x[32] = "NA";
  if (M.date_ok)  snprintf(date, sizeof date, "%02u/%02u/20%02u %02u:%02u:%02u", M.date.Date, M.date.Month,
                           M.date.Year, M.heure.Hours, M.heure.Minutes, M.heure.Seconds);
  if (!P.gps)     snprintf(pos, sizeof pos, "GPS desactive (GPS=0)");
  else if (M.gps_ok) snprintf(pos, sizeof pos, "%s, %s", M.lat, M.lon);
  if (M.temp_ok)  snprintf(t, sizeof t, "%d C", M.temp);
  if (M.hygro_ok) snprintf(h, sizeof h, "%d %%", M.hygro);
  if (M.lux_ok)   snprintf(x, sizeof x, "%u lux (%s)", M.lux, classe_lumiere(M.lux));
  if (detaille)
  {
    LOG("    Date et heure : %s\r\n", date);
    LOG("    Position      : %s\r\n", pos);
    LOG("    Temperature   : %s\r\n", t);
    LOG("    Humidite      : %s\r\n", h);
    LOG("    Luminosite    : %s\r\n", x);
  }
  else
  {
    LOG("[MAINTENANCE] %s | Temp %s | Hum %s | Lum %s | GPS %s\r\n", date, t, h, x, pos);
  }
}

/* Ligne CSV : date;heure;latitude;longitude;temperature;hygrometrie;luminosite */
static void construire_ligne(char *l, size_t n)
{
  char t[8] = "NA", h[8] = "NA", x[8] = "NA";
  if (M.temp_ok)  snprintf(t, sizeof t, "%d", M.temp);
  if (M.hygro_ok) snprintf(h, sizeof h, "%d", M.hygro);
  if (M.lux_ok)   snprintf(x, sizeof x, "%u", M.lux);
  if (M.date_ok)
    snprintf(l, n, "%02u/%02u/20%02u;%02u:%02u:%02u;%s;%s;%s;%s;%s\r\n",
             M.date.Date, M.date.Month, M.date.Year, M.heure.Hours, M.heure.Minutes, M.heure.Seconds,
             M.gps_ok ? M.lat : "NA", M.gps_ok ? M.lon : "NA", t, h, x);
  else
    snprintf(l, n, "NA;NA;%s;%s;%s;%s;%s\r\n", M.gps_ok ? M.lat : "NA", M.gps_ok ? M.lon : "NA", t, h, x);
}

/* --------------------------------------------------------------------------
 *  Enregistrement sur la carte SD : fichier AAMMJJ_0.LOG + rotation
 * -------------------------------------------------------------------------- */
static void nom_fichier(char *nom, uint8_t rev)
{
  if (M.date_ok) sprintf(nom, "%02u%02u%02u_%u.LOG", M.date.Year, M.date.Month, M.date.Date, rev);
  else           sprintf(nom, "000000_%u.LOG", rev);
}

static int sd_monter(void)
{
  if (sd_prete) return 1;
  FRESULT r = f_mount(&USERFatFS, USERPath, 1);
  if (r == FR_OK) { sd_prete = 1; LOG("    carte SD montee\r\n"); }
  else if (r == FR_NOT_READY)
  {
    extern char sd_diag[];
    LOG("    SD : la carte ne repond pas a l initialisation : %s\r\n", sd_diag);
  }
  else if (r == FR_NO_FILESYSTEM)  LOG("    SD : pas de systeme FAT32 sur la carte (la formater en FAT32)\r\n");
  else if (r == FR_DISK_ERR)       LOG("    SD : erreur de lecture sur la carte\r\n");
  else                             LOG("    SD : echec du montage (code FatFS %d)\r\n", (int)r);
  return sd_prete;
}

/* Copie le fichier 0 vers la première révision libre (1 à 9 : noms 8.3), puis le vide. */
static int archiver(const char *nom0)
{
  char nom[16];
  FILINFO fi;
  uint8_t rev = 0;
  for (uint8_t r = 1; r <= 9; r++) { nom_fichier(nom, r); if (f_stat(nom, &fi) == FR_NO_FILE) { rev = r; break; } }
  if (!rev) return 0;                                   /* plus de numéro libre aujourd'hui */

  FIL src, dst;
  static BYTE tampon[512];
  UINT lu, ecrit;
  int ok = 1;
  if (f_open(&src, nom0, FA_READ) != FR_OK) return 0;
  if (f_open(&dst, nom, FA_CREATE_NEW | FA_WRITE) != FR_OK) { f_close(&src); return 0; }
  do {
    if (f_read(&src, tampon, sizeof tampon, &lu) != FR_OK) { ok = 0; break; }
    if (lu && (f_write(&dst, tampon, lu, &ecrit) != FR_OK || ecrit != lu)) { ok = 0; break; }
  } while (lu == sizeof tampon);
  f_close(&src);
  if (f_close(&dst) != FR_OK) ok = 0;
  if (!ok) { f_unlink(nom); return 0; }                 /* copie ratée : on ne vide pas le fichier 0 */

  if (f_open(&src, nom0, FA_WRITE) != FR_OK) return 0;  /* vider le fichier 0 */
  f_lseek(&src, 0); f_truncate(&src);
  f_close(&src);
  LOG("    archive : %s copie dans %s, puis vide\r\n", nom0, nom);
  return 1;
}

static void enregistrer(const char *ligne)
{
  char nom0[16];
  FIL f;
  UINT ecrit;
  UINT len = (UINT)strlen(ligne);

  if (!sd_monter()) { erreur_set(ERR_SD_ECRITURE, 1); LOG("    ERREUR : carte SD absente ou illisible\r\n"); return; }

  /* place libre (en octets) : il faut la ligne + de quoi copier une archive */
  FATFS *fs; DWORD libres;
  if (f_getfree(USERPath, &libres, &fs) == FR_OK &&
      (uint64_t)libres * fs->csize * 512U < (uint64_t)len + (uint64_t)P.file_max_size)
  {
    erreur_set(ERR_SD_PLEINE, 1); LOG("    ERREUR : carte SD pleine\r\n"); return;
  }
  erreur_set(ERR_SD_PLEINE, 0);

  nom_fichier(nom0, 0);
  if (f_open(&f, nom0, FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) { sd_demonter(); erreur_set(ERR_SD_ECRITURE, 1); return; }
  if (f_size(&f) + len > (DWORD)P.file_max_size)
  {
    f_close(&f);
    if (!archiver(nom0)) { erreur_set(ERR_SD_ECRITURE, 1); LOG("    ERREUR : archivage impossible\r\n"); return; }
    if (f_open(&f, nom0, FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) { erreur_set(ERR_SD_ECRITURE, 1); return; }
  }
  int ok = 1;
  if (f_size(&f) == 0)                       /* nouveau fichier (ou vidé après archivage) : en-tête des colonnes */
  {
    static const char entete[] = "date;heure;latitude;longitude;temperature_C;humidite_pct;luminosite_lux\r\n";
    ok = (f_write(&f, entete, sizeof entete - 1U, &ecrit) == FR_OK && ecrit == sizeof entete - 1U);
  }
  ok = ok && (f_lseek(&f, f_size(&f)) == FR_OK && f_write(&f, ligne, len, &ecrit) == FR_OK && ecrit == len);
  if (f_close(&f) != FR_OK) ok = 0;            /* fermer = tout est réellement écrit */
  if (!ok) { sd_demonter(); erreur_set(ERR_SD_ECRITURE, 1); LOG("    ERREUR : ecriture SD\r\n"); return; }
  erreur_set(ERR_SD_ECRITURE, 0);
  LOG("    ecrit dans %s\r\n", nom0);
}

/* --------------------------------------------------------------------------
 *  Écran LCD : mode + mesures
 * -------------------------------------------------------------------------- */
/* Dernière mesure complète : l'écran l'affiche pendant que la suivante est en cours
 * (sinon il passerait à NA au début de chaque acquisition). */
static Mesures M_aff;
static uint8_t M_aff_ok = 0;

static void lcd_maj(void)
{
  if (!lcd_ok) return;

  /* Un échange I2C raté décale l'écran (caractères bizarres) : on le réinitialise.
   * Réinitialisation aussi toutes les 60 s, par sécurité. */
  static uint32_t derniere_init = 0;
  if (lcd.erreur || HAL_GetTick() - derniere_init >= 60000U)
  {
    LCD_Init(&lcd, &hi2c1, lcd.addr);
    derniere_init = HAL_GetTick();
  }

  /* Noms courts affichés sur l'écran, dans l'ordre de l'enum Mode (station.h) */
  static const char *noms[4] = { "STD", "CONF", "MAINT", "ECO" };
  char l1[17], l2[17];
  RTC_TimeTypeDef heure; RTC_DateTypeDef date;
  HAL_RTC_GetTime(&hrtc, &heure, RTC_FORMAT_BIN); HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);

  /* Ligne 1 : mode (8 caractères) + heure */
  snprintf(l1, sizeof l1, "%-8.8s%02u:%02u:%02u", noms[mode_actuel],
           heure.Hours % 24U, heure.Minutes % 60U, heure.Seconds % 60U);

  /* Ligne 2 : erreur en priorité, sinon une info selon le mode */
  static const char *txt_err[ERR_NB] = { "ERR ECRITURE SD", "ERR SD PLEINE", "ERR HORLOGE RTC",
                                         "ERR CAPTEUR", "ERR GPS", "ERR DONNEE" };
  uint32_t err = erreurs;
  if (err)
  {
    int e = 0;
    while (!(err & (1UL << e))) e++;
    snprintf(l2, sizeof l2, "%-16.16s", txt_err[e]);
  }
  else if (mode_actuel == MODE_CONFIGURATION)
  {
    snprintf(l2, sizeof l2, "Serie LOG=%-6.6s", txt_intervalle(P.log_interval_s));
  }
  else if (!M_aff_ok)
  {
    snprintf(l2, sizeof l2, "%-16.16s", "Mesure en cours");
  }
  else
  {
    /* ex. "28\xDF" "C 43% 436lx" : \xDF = symbole degre sur le LCD */
    char t[6] = "NA", h[5] = "NA", x[8] = "NA";
    if (M_aff.temp_ok)  snprintf(t, sizeof t, "%d\xDF" "C", M_aff.temp);
    if (M_aff.hygro_ok) snprintf(h, sizeof h, "%d%%", M_aff.hygro);
    if (M_aff.lux_ok)   snprintf(x, sizeof x, "%ulx", M_aff.lux % 100000U);
    snprintf(l2, sizeof l2, "%-5.5s %-3.3s %-6.6s", t, h, x);
  }

  /* Chaque ligne fait exactement 16 caractères : l'ancien texte est toujours effacé.
   * Si un octet I2C est perdu pendant l'écriture, l'écran est décalé (caractères bizarres) :
   * on le réinitialise et on réécrit tout de suite, jusqu'à 3 fois. */
  static uint32_t nb_erreurs_lcd = 0, dernier_rapport = 0;
  for (int essai = 0; essai < 3; essai++)
  {
    LCD_Resync(&lcd);                       /* recalage à chaque rafraîchissement : rien ne reste décalé */
    LCD_SetCursor(&lcd, 0, 0); LCD_Print(&lcd, l1);
    LCD_SetCursor(&lcd, 0, 1); LCD_Print(&lcd, l2);
    if (!lcd.erreur) break;
    nb_erreurs_lcd++;
    LCD_Init(&lcd, &hi2c1, lcd.addr);
    derniere_init = HAL_GetTick();
  }
  if (nb_erreurs_lcd && HAL_GetTick() - dernier_rapport >= 60000U)
  {
    dernier_rapport = HAL_GetTick();
    LOG("    LCD : %lu echanges I2C rates depuis le demarrage (fils SDA et SCL trop longs ou mal enfonces ?)\r\n",
        (unsigned long)nb_erreurs_lcd);
  }
}

static void lcd_tick(void)
{
  static uint32_t dernier = 0;
  if (HAL_GetTick() - dernier >= 1000U) { dernier = HAL_GetTick(); lcd_maj(); }
}

/* --------------------------------------------------------------------------
 *  Mode configuration : commandes série
 * -------------------------------------------------------------------------- */
typedef struct { const char *nom; int32_t *val; int32_t min, max; } ParamDef;
static const ParamDef PARAMS[] = {
  /* LOG_INTERVAL est traité à part (minutes, ou secondes avec un S) dans config_commande */
  { "FILE_MAX_SIZE", &P.file_max_size, 256, 4194304 },
  { "TIMEOUT",       &P.timeout,       1, 300 },
  { "LUMIN",         &P.lumin,         0, 1 },
  { "LUMIN_LOW",     &P.lumin_low,     0, 1023 },
  { "LUMIN_HIGH",    &P.lumin_high,    0, 1023 },
  { "TEMP_AIR",      &P.temp_air,      0, 1 },
  { "MIN_TEMP_AIR",  &P.min_temp_air,  -40, 85 },
  { "MAX_TEMP_AIR",  &P.max_temp_air,  -40, 85 },
  { "HYGR",          &P.hygr,          0, 1 },
  { "HYGR_MINT",     &P.hygr_mint,     -40, 85 },
  { "HYGR_MAXT",     &P.hygr_maxt,     -40, 85 },
  { "GPS",           &P.gps,           0, 1 },        /* 1 = marche, 0 = arrêt */
};

static void config_aide(void)
{
  LOG("\r\n>>> Mode CONFIGURATION (acquisition arretee). Commandes :\r\n"
      "    LOG_INTERVAL=10 (minutes) ou LOG_INTERVAL=20S (secondes)\r\n"
      "    FILE_MAX_SIZE=2048 (2 Ko)  TIMEOUT=30\r\n"
      "    LUMIN=1  LUMIN_LOW=255  LUMIN_HIGH=768\r\n"
      "    TEMP_AIR=1  MIN_TEMP_AIR=-10  MAX_TEMP_AIR=60  HYGR=1  HYGR_MINT=0  HYGR_MAXT=50\r\n"
      "    GPS=1 (GPS en marche) ou GPS=0 (GPS arrete)\r\n"
      "    CLOCK=HH:MM:SS  DATE=MM,JJ,AAAA  DAY=MON  RESET  VERSION  PARAMS  EXIT\r\n");
  LOG("    Retour automatique en STANDARD apres %lu s sans commande.\r\n", (unsigned long)TIMEOUT_CONFIG_S);
}

static void config_params(void)
{
  LOG("    LOG_INTERVAL=%s FILE_MAX_SIZE=%ld TIMEOUT=%ld\r\n", txt_intervalle(P.log_interval_s), (long)P.file_max_size, (long)P.timeout);
  LOG("    LUMIN=%ld LUMIN_LOW=%ld LUMIN_HIGH=%ld\r\n", (long)P.lumin, (long)P.lumin_low, (long)P.lumin_high);
  LOG("    TEMP_AIR=%ld MIN_TEMP_AIR=%ld MAX_TEMP_AIR=%ld HYGR=%ld HYGR_MINT=%ld HYGR_MAXT=%ld\r\n",
      (long)P.temp_air, (long)P.min_temp_air, (long)P.max_temp_air, (long)P.hygr, (long)P.hygr_mint, (long)P.hygr_maxt);
  LOG("    GPS=%ld (%s)\r\n", (long)P.gps, P.gps ? "en marche" : "arrete");
}

static int entier(const char *s, long *v)
{
  char *fin;
  if (!*s) return 0;
  *v = strtol(s, &fin, 10);
  return *fin == '\0';
}

static void config_commande(char *ligne)
{
  for (char *p = ligne; *p; p++) *p = (char)toupper((unsigned char)*p);
  char *egal = strchr(ligne, '=');
  char *valeur = "";
  if (egal) { *egal = '\0'; valeur = egal + 1; }
  derniere_activite = HAL_GetTick();

  if (!strcmp(ligne, "RESET"))   { P = PARAM_DEFAUT; param_sauver(); LOG("    OK : parametres par defaut\r\n"); return; }
  if (!strcmp(ligne, "VERSION")) { LOG("    %s  %s\r\n", VERSION, NUMERO_LOT); return; }
  if (!strcmp(ligne, "PARAMS"))  { config_params(); return; }
  if (!strcmp(ligne, "EXIT"))    { changer_mode(MODE_STANDARD); return; }   /* ajout, non exigé par le CDC */
  if (!strncmp(ligne, "PRESSURE", 8)) { LOG("    Capteur de pression non disponible sur cette station\r\n"); return; }

  if (!strcmp(ligne, "CLOCK"))
  {
    unsigned h, m, s;
    RTC_TimeTypeDef t = {0};
    if (sscanf(valeur, "%u:%u:%u", &h, &m, &s) == 3 && h <= 23 && m <= 59 && s <= 59)
    {
      t.Hours = (uint8_t)h; t.Minutes = (uint8_t)m; t.Seconds = (uint8_t)s;
      HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN);
      HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0x32F2);   /* l'heure survit au RESET */
      LOG("    OK\r\n");
    }
    else LOG("    ERREUR format : CLOCK=HH:MM:SS\r\n");
    return;
  }
  if (!strcmp(ligne, "DATE"))
  {
    unsigned mo, j, a;
    RTC_DateTypeDef d;
    HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
    if (sscanf(valeur, "%u,%u,%u", &mo, &j, &a) == 3 && mo >= 1 && mo <= 12 && j >= 1 && j <= 31 && a >= 2000 && a <= 2099)
    {
      d.Month = (uint8_t)mo; d.Date = (uint8_t)j; d.Year = (uint8_t)(a - 2000);
      HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN);
      HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0x32F2);
      LOG("    OK\r\n");
    }
    else LOG("    ERREUR format : DATE=MM,JJ,AAAA\r\n");
    return;
  }
  if (!strcmp(ligne, "DAY"))
  {
    static const char *jours[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };
    for (int i = 0; i < 7; i++)
      if (!strcmp(valeur, jours[i]))
      {
        RTC_DateTypeDef d;
        HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
        d.WeekDay = (uint8_t)(i + 1);
        HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN);
        LOG("    OK\r\n");
        return;
      }
    LOG("    ERREUR format : DAY=MON, TUE, WED, THU, FRI, SAT ou SUN\r\n");
    return;
  }
  /* LOG_INTERVAL=10 : 10 minutes (cahier des charges). LOG_INTERVAL=20S : 20 secondes (tests). */
  if (!strcmp(ligne, "LOG_INTERVAL") || !strcmp(ligne, "LOG_INTERVALL"))
  {
    size_t n = strlen(valeur);
    int secondes = (n > 0 && valeur[n - 1] == 'S');
    if (secondes) valeur[n - 1] = '\0';
    long v;
    if (!egal || !entier(valeur, &v) || (secondes ? (v < 5 || v > 86400) : (v < 1 || v > 1440)))
    {
      LOG("    ERREUR : LOG_INTERVAL=1 a 1440 (minutes) ou LOG_INTERVAL=5S a 86400S (secondes)\r\n");
      return;
    }
    P.log_interval_s = (int32_t)(secondes ? v : v * 60);
    prochaine_acq = HAL_GetTick();
    LOG(param_sauver() ? "    OK : une mesure toutes les %s (sauvegarde)\r\n" : "    OK : une mesure toutes les %s, mais sauvegarde Flash impossible\r\n",
        txt_intervalle(P.log_interval_s));
    return;
  }
  for (size_t i = 0; i < sizeof PARAMS / sizeof PARAMS[0]; i++)
  {
    if (strcmp(ligne, PARAMS[i].nom)) continue;
    long v;
    if (!egal || !entier(valeur, &v) || v < PARAMS[i].min || v > PARAMS[i].max)
    {
      LOG("    ERREUR : valeur entiere entre %ld et %ld attendue\r\n", (long)PARAMS[i].min, (long)PARAMS[i].max);
      return;
    }
    *PARAMS[i].val = (int32_t)v;
    LOG(param_sauver() ? "    OK (sauvegarde)\r\n" : "    OK, mais sauvegarde Flash impossible\r\n");
    return;
  }
  LOG("    Commande inconnue. Tape une ligne vide ou PARAMS ; liste dans l aide ci-dessus.\r\n");
}

/* --------------------------------------------------------------------------
 *  Initialisation et boucle principale
 * -------------------------------------------------------------------------- */
void Station_Init(void)
{
  HAL_SPI_MspInit(&hspi1);           /* BSP_LED_Init a pris PA5 (LD2) : on rend la broche au SPI1 */
  RGB_LED_Init();
  DHT11_Init();
  param_charger();
  boutons_init();

  /* Réception série par interruption */
  HAL_NVIC_SetPriority(USART2_IRQn, 6, 0); HAL_NVIC_EnableIRQ(USART2_IRQn);
  HAL_NVIC_SetPriority(USART3_IRQn, 6, 0); HAL_NVIC_EnableIRQ(USART3_IRQn);
  HAL_UART_Receive_IT(&huart2, &rx2, 1);
  HAL_UART_Receive_IT(&huart3, &rx3, 1);

  /* Écran : recherche automatique de l'adresse (0x27 puis 0x20..0x26, puis 0x3F..0x38) */
  for (int a = 0x27; a >= 0x20 && !lcd_ok; a--)
    if (a != 0x23 && HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2, 10) == HAL_OK)
      lcd_ok = (LCD_Init(&lcd, &hi2c1, (uint8_t)(a << 1)) == HAL_OK);
  for (int a = 0x3F; a >= 0x38 && !lcd_ok; a--)
    if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2, 10) == HAL_OK)
      lcd_ok = (LCD_Init(&lcd, &hi2c1, (uint8_t)(a << 1)) == HAL_OK);
  if (lcd_ok) { LCD_SetCursor(&lcd, 0, 0); LCD_Print(&lcd, "STATION AURA"); }

  LOG("\r\n\r\n======== %s ========\r\n", VERSION);
  LOG("Boutons : ROUGE = bouton externe PC3, VERT = bouton bleu B1 (appui long = 5 s)\r\n");
  config_params();

  /* Démarrage bouton rouge appuyé : mode configuration */
  if (HAL_GPIO_ReadPin(BTN_ROUGE_PORT, BTN_ROUGE_PIN) == GPIO_PIN_RESET)
  {
    mode_actuel = MODE_STANDARD;
    changer_mode(MODE_CONFIGURATION);
  }
  else
  {
    mode_actuel = MODE_STANDARD;
    LOG("\r\n>>> Mode STANDARD : une mesure toutes les %s\r\n", txt_intervalle(P.log_interval_s));
  }
  prochaine_acq = HAL_GetTick();
  station_active = 1;
}

void Station_Loop(void)
{
  gerer_evenements();
  lcd_tick();

  switch (mode_actuel)
  {
    case MODE_CONFIGURATION:
      if (cmd_prete)
      {
        char ligne[64];
        __disable_irq(); memcpy(ligne, cmd_buf, sizeof ligne); cmd_len = 0; cmd_prete = 0; __enable_irq();
        LOG("> %s\r\n", ligne);
        config_commande(ligne);
      }
      if (mode_actuel == MODE_CONFIGURATION && HAL_GetTick() - derniere_activite >= TIMEOUT_CONFIG_MS)
      {
        LOG("    %lu s sans commande\r\n", (unsigned long)TIMEOUT_CONFIG_S);
        changer_mode(MODE_STANDARD);
      }
      break;

    case MODE_STANDARD:
    case MODE_ECONOMIQUE:
    {
      uint32_t intervalle = (uint32_t)P.log_interval_s * 1000U;
      if (mode_actuel == MODE_ECONOMIQUE) intervalle *= 2U;          /* LOG_INTERVAL x 2 */
      if ((int32_t)(HAL_GetTick() - prochaine_acq) < 0) break;
      Mode m = mode_actuel;
      int gps = (m == MODE_STANDARD) || ((compteur_gps++ % 2U) == 0U);   /* éco : GPS 1 fois sur 2 */
      if (!P.gps) gps = 0;                                               /* GPS=0 : jamais lu */
      if (m == MODE_STANDARD) compteur_gps = 0;
      LOG("\r\n[%s] acquisition%s\r\n", nom_mode(m), (gps || !P.gps) ? "" : " (GPS saute en mode eco)");
      if (acquerir(gps) == ACQ_COMPLETE && mode_actuel == m)
      {
        char ligne[96];
        M_aff = M; M_aff_ok = 1;
        lcd_maj();
        construire_ligne(ligne, sizeof ligne);
        afficher_mesures(1);
        LOG("    Ligne SD      : %s", ligne);
        enregistrer(ligne);
        prochaine_acq += intervalle;
        if ((int32_t)(HAL_GetTick() - prochaine_acq) > 0) prochaine_acq = HAL_GetTick() + intervalle;
      }
      else
      {
        LOG("    acquisition interrompue par un changement de mode\r\n");
      }
      break;
    }

    case MODE_MAINTENANCE:
      if ((int32_t)(HAL_GetTick() - prochaine_acq) < 0) break;
      prochaine_acq = HAL_GetTick() + INTERVALLE_MAINT_MS;
      if (acquerir(P.gps ? 1 : 0) == ACQ_COMPLETE && mode_actuel == MODE_MAINTENANCE)
      {
        M_aff = M; M_aff_ok = 1;
        afficher_mesures(0);                              /* pas d'écriture sur la carte SD */
      }
      break;
  }
}
