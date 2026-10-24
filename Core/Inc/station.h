#ifndef STATION_H
#define STATION_H

#include "main.h"

/* Station météo Worldwide Weather Watcher : les 4 modes du cahier des charges
 * (A2 Projet Système embarqué, Modes de fonctionnement).
 *
 *   STANDARD      LED verte   : acquisition toutes les LOG_INTERVAL minutes, ligne écrite sur la carte SD
 *   CONFIGURATION LED jaune   : démarrage bouton rouge appuyé ; commandes sur le port série ; pas d'acquisition
 *   MAINTENANCE   LED orange  : rouge 5 s ; mesures affichées sur le port série ; carte SD retirable
 *   ECONOMIQUE    LED bleue   : vert 5 s depuis Standard ; intervalle x2, GPS une mesure sur deux
 */
typedef enum { MODE_STANDARD, MODE_CONFIGURATION, MODE_MAINTENANCE, MODE_ECONOMIQUE } Mode;

void Station_Init(void);      /* à appeler une fois, après les MX_xxx_Init */
void Station_Loop(void);      /* à appeler en boucle dans while(1) */
void Station_Tick1ms(void);   /* à appeler dans SysTick_Handler (1 ms) : LED + appuis longs */

#endif /* STATION_H */
