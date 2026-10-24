#ifndef SENSORS_APP_H
#define SENSORS_APP_H

/* A appeler une fois, avant la boucle while(1) */
void SensorsApp_Init(void);

/* A appeler en boucle dans while(1) */
void SensorsApp_Loop(void);

#endif /* SENSORS_APP_H */
