/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * @file    user_diskio.c
  * @brief   This file includes a diskio driver skeleton to be completed by the user.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
 /* USER CODE END Header */

#ifdef USE_OBSOLETE_USER_CODE_SECTION_0
/*
 * Warning: the user section 0 is no more in use (starting from CubeMx version 4.16.0)
 * To be suppressed in the future.
 * Kept to ensure backward compatibility with previous CubeMx versions when
 * migrating projects.
 * User code previously added there should be copied in the new user sections before
 * the section contents can be deleted.
 */
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */
#endif

/* USER CODE BEGIN DECL */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include <stdio.h>
#include "ff_gen_drv.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* ---- Pilote carte SD en mode SPI (SPI1 : SCK PA5, MISO PA6, MOSI PA7, CS PB6) ---- */
#include "main.h"
extern SPI_HandleTypeDef hspi1;

#define CT_SD1    0x02            /* SD version 1 */
#define CT_SD2    0x04            /* SD version 2 */
#define CT_BLOCK  0x08            /* adressage par bloc (SDHC / SDXC) */
static uint8_t card_type = 0;
char sd_diag[64] = "";            /* dernière étape de l'initialisation, lisible depuis station.c */

static void sd_cs_high(void) { HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET); }
static void sd_cs_low(void)  { HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET); }

static uint8_t sd_xfer(uint8_t out)
{
  uint8_t in = 0xFF;
  HAL_SPI_TransmitReceive(&hspi1, &out, &in, 1, 50);
  return in;
}

static void sd_set_prescaler(uint32_t prescaler)
{
  __HAL_SPI_DISABLE(&hspi1);
  hspi1.Instance->CR1 = (hspi1.Instance->CR1 & ~SPI_CR1_BR) | prescaler;
  __HAL_SPI_ENABLE(&hspi1);
}

static int sd_wait_ready(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();
  do { if (sd_xfer(0xFF) == 0xFF) return 1; } while (HAL_GetTick() - t0 < timeout_ms);
  return 0;
}

static void sd_deselect(void) { sd_cs_high(); sd_xfer(0xFF); }

static int sd_select(void)
{
  sd_cs_low();
  sd_xfer(0xFF);
  if (sd_wait_ready(500)) return 1;
  sd_deselect();
  return 0;
}

/* Envoie une commande (bit 7 = commande « ACMD », précédée de CMD55). Retourne la réponse R1. */
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg)
{
  uint8_t r;
  if (cmd & 0x80)
  {
    cmd &= 0x7F;
    r = sd_cmd(55, 0);
    if (r > 1) return r;
  }
  sd_deselect();
  /* Certains modules SD (tampon 74LVC125) ne libèrent pas MISO assez vite : si la carte
   * ne signale pas « prête », on envoie quand même la commande au lieu d'abandonner. */
  if (!sd_select()) { sd_cs_low(); sd_xfer(0xFF); }

  sd_xfer(0x40 | cmd);
  sd_xfer((uint8_t)(arg >> 24)); sd_xfer((uint8_t)(arg >> 16));
  sd_xfer((uint8_t)(arg >> 8));  sd_xfer((uint8_t)arg);
  sd_xfer(cmd == 0 ? 0x95 : (cmd == 8 ? 0x87 : 0x01));   /* CRC valide pour CMD0 et CMD8 */

  if (cmd == 12) sd_xfer(0xFF);                          /* octet de bourrage après CMD12 */
  int n = 64;                                            /* certaines cartes répondent tard */
  do { r = sd_xfer(0xFF); } while ((r & 0x80) && --n);
  return r;
}

static int sd_recv_block(BYTE *buff, UINT len)
{
  uint8_t token;
  uint32_t t0 = HAL_GetTick();
  do { token = sd_xfer(0xFF); } while (token == 0xFF && HAL_GetTick() - t0 < 200);
  if (token != 0xFE) return 0;
  while (len--) *buff++ = sd_xfer(0xFF);
  sd_xfer(0xFF); sd_xfer(0xFF);                          /* CRC ignoré */
  return 1;
}

static int sd_send_block(const BYTE *buff, uint8_t token)
{
  if (!sd_wait_ready(500)) return 0;
  sd_xfer(token);
  for (UINT i = 0; i < 512; i++) sd_xfer(buff[i]);
  sd_xfer(0xFF); sd_xfer(0xFF);                          /* CRC factice */
  return ((sd_xfer(0xFF) & 0x1F) == 0x05);               /* données acceptées */
}

/* USER CODE END DECL */

/* Private function prototypes -----------------------------------------------*/
DSTATUS USER_initialize (BYTE pdrv);
DSTATUS USER_status (BYTE pdrv);
DRESULT USER_read (BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
#if _USE_WRITE == 1
  DRESULT USER_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
  DRESULT USER_ioctl (BYTE pdrv, BYTE cmd, void *buff);
#endif /* _USE_IOCTL == 1 */

Diskio_drvTypeDef  USER_Driver =
{
  USER_initialize,
  USER_status,
  USER_read,
#if  _USE_WRITE
  USER_write,
#endif  /* _USE_WRITE == 1 */
#if  _USE_IOCTL == 1
  USER_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_initialize (
	BYTE pdrv           /* Physical drive nmuber to identify the drive */
)
{
  /* USER CODE BEGIN INIT */
  uint8_t ocr[4], type = 0;
  uint32_t t0;

  if (pdrv) return STA_NOINIT;
  HAL_SPI_MspInit(&hspi1);                     /* PA5 peut avoir été pris par la LED LD2 (BSP) */
  sd_set_prescaler(SPI_BAUDRATEPRESCALER_256); /* < 400 kHz pendant l'initialisation */
  sd_cs_high();
  for (int i = 0; i < 10; i++) sd_xfer(0xFF);  /* 80 impulsions d'horloge */

  uint8_t r0 = 0xFF;
  for (int essai = 0; essai < 5 && r0 != 1; essai++) r0 = sd_cmd(0, 0);   /* passage en mode SPI (idle) */
  if (r0 == 1)
  {
    t0 = HAL_GetTick();
    uint8_t r8 = sd_cmd(8, 0x1AA);
    if (r8 == 1)                               /* carte SD version 2 */
    {
      for (int i = 0; i < 4; i++) ocr[i] = sd_xfer(0xFF);
      if (ocr[2] == 0x01 && ocr[3] == 0xAA)
      {
        uint8_t r41;
        uint8_t r55 = 0xFF;
        do {
          r55 = sd_cmd(55, 0);
          r41 = (r55 <= 1) ? sd_cmd(41, 1UL << 30) : 0xFF;
          if (r41) HAL_Delay(5);
        } while (r41 && HAL_GetTick() - t0 < 2000);
        if (r41 != 0) { snprintf(sd_diag, sizeof sd_diag, "ACMD41 : CMD55=0x%02X ACMD41=0x%02X", r55, r41); }
        else if (sd_cmd(58, 0) == 0)
        {
          for (int i = 0; i < 4; i++) ocr[i] = sd_xfer(0xFF);
          type = (ocr[0] & 0x40) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
          snprintf(sd_diag, sizeof sd_diag, "carte SD v2 %s detectee", (type & CT_BLOCK) ? "SDHC" : "SDSC");
        }
        else snprintf(sd_diag, sizeof sd_diag, "CMD58 sans reponse");
      }
      else snprintf(sd_diag, sizeof sd_diag, "CMD8 : echo incorrect %02X %02X", ocr[2], ocr[3]);
    }
    else                                       /* carte SD version 1 */
    {
      uint8_t r41;
      do { r41 = sd_cmd(0x80 | 41, 0); } while (r41 && HAL_GetTick() - t0 < 1000);
      if (r41 == 0 && sd_cmd(16, 512) == 0) { type = CT_SD1; snprintf(sd_diag, sizeof sd_diag, "carte SD v1 detectee"); }
      else snprintf(sd_diag, sizeof sd_diag, "CMD8=0x%02X puis ACMD41=0x%02X", r8, r41);
    }
  }
  else snprintf(sd_diag, sizeof sd_diag, "CMD0 sans reponse (0x%02X)", r0);
  card_type = type;
  sd_deselect();

  if (type)
  {
    sd_set_prescaler(SPI_BAUDRATEPRESCALER_8); /* 10 MHz ensuite */
    Stat &= (DSTATUS)~STA_NOINIT;
  }
  else
  {
    Stat = STA_NOINIT;
  }
  return Stat;
  /* USER CODE END INIT */
}

/**
  * @brief  Gets Disk Status
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_status (
	BYTE pdrv       /* Physical drive number to identify the drive */
)
{
  /* USER CODE BEGIN STATUS */
  if (pdrv) return STA_NOINIT;
  return Stat;
  /* USER CODE END STATUS */
}

/**
  * @brief  Reads Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */
DRESULT USER_read (
	BYTE pdrv,      /* Physical drive nmuber to identify the drive */
	BYTE *buff,     /* Data buffer to store read data */
	DWORD sector,   /* Sector address in LBA */
	UINT count      /* Number of sectors to read */
)
{
  /* USER CODE BEGIN READ */
  if (pdrv || !count) return RES_PARERR;
  if (Stat & STA_NOINIT) return RES_NOTRDY;
  if (!(card_type & CT_BLOCK)) sector *= 512;     /* anciennes cartes : adresse en octets */

  if (count == 1)
  {
    if (sd_cmd(17, sector) == 0 && sd_recv_block(buff, 512)) count = 0;
  }
  else if (sd_cmd(18, sector) == 0)
  {
    do { if (!sd_recv_block(buff, 512)) break; buff += 512; } while (--count);
    sd_cmd(12, 0);                                /* fin de lecture multiple */
  }
  sd_deselect();
  return count ? RES_ERROR : RES_OK;
  /* USER CODE END READ */
}

/**
  * @brief  Writes Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1
DRESULT USER_write (
	BYTE pdrv,          /* Physical drive nmuber to identify the drive */
	const BYTE *buff,   /* Data to be written */
	DWORD sector,       /* Sector address in LBA */
	UINT count          /* Number of sectors to write */
)
{
  /* USER CODE BEGIN WRITE */
  if (pdrv || !count) return RES_PARERR;
  if (Stat & STA_NOINIT) return RES_NOTRDY;

  /* Un secteur à la fois (CMD24) : simple et suffisant pour des lignes de mesures. */
  while (count)
  {
    DWORD addr = (card_type & CT_BLOCK) ? sector : sector * 512;
    if (sd_cmd(24, addr) != 0 || !sd_send_block(buff, 0xFE)) break;
    buff += 512; sector++; count--;
  }
  sd_deselect();
  return count ? RES_ERROR : RES_OK;
  /* USER CODE END WRITE */
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  pdrv: Physical drive number (0..)
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT USER_ioctl (
	BYTE pdrv,      /* Physical drive nmuber (0..) */
	BYTE cmd,       /* Control code */
	void *buff      /* Buffer to send/receive control data */
)
{
  /* USER CODE BEGIN IOCTL */
  DRESULT res = RES_ERROR;
  uint8_t csd[16];

  if (pdrv) return RES_PARERR;
  if (Stat & STA_NOINIT) return RES_NOTRDY;

  switch (cmd)
  {
    case CTRL_SYNC:                               /* attendre la fin des écritures */
      if (sd_select()) res = RES_OK;
      break;
    case GET_SECTOR_SIZE:
      *(WORD *)buff = 512; res = RES_OK;
      break;
    case GET_BLOCK_SIZE:
      *(DWORD *)buff = 1; res = RES_OK;
      break;
    case GET_SECTOR_COUNT:                        /* capacité lue dans le registre CSD (CMD9) */
      if (sd_cmd(9, 0) == 0 && sd_recv_block(csd, 16))
      {
        if ((csd[0] >> 6) == 1)                   /* CSD version 2 (SDHC / SDXC) */
        {
          DWORD csize = csd[9] + ((DWORD)csd[8] << 8) + ((DWORD)(csd[7] & 0x3F) << 16) + 1;
          *(DWORD *)buff = csize << 10;
        }
        else                                      /* CSD version 1 */
        {
          BYTE n = (BYTE)((csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2);
          DWORD csize = (csd[8] >> 6) + ((DWORD)csd[7] << 2) + ((DWORD)(csd[6] & 3) << 10) + 1;
          *(DWORD *)buff = csize << (n - 9);
        }
        res = RES_OK;
      }
      break;
    default:
      res = RES_PARERR;
  }
  sd_deselect();
  return res;
  /* USER CODE END IOCTL */
}
#endif /* _USE_IOCTL == 1 */

