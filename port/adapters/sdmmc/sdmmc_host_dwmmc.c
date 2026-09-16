/*
 * Copyright (c) 2026 Phytium Information Technology, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * @file   sdmmc_host_dwmmc.c
 * @brief  Host glue: the fsl_sdmmc SDMMCHOST surface over the DW-MMC
 *         controller driver (sdmmc0 slot).
 *
 * Ported verbatim from the standalone line's fsl_hc_fdwmmc.c (BSD-3,
 * decision D27): the ops table, the transfer conversion, the CMD23
 * pre-command, the response copyback and every capability value are the
 * originals. Only identifiers changed; the controller register accessors
 * are the driver's own (dwc_mmc_regs.h) so the sequences are untouched.
 * The I2C vqmmc helper of the original stays guarded off - this board's
 * rails are fixed.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "board.h"
#include "dwmmc.h"
#include "dwc_mmc_regs.h"
#include "finterrupt.h"
#include "sdmmc_board.h"
#include "fsl_sdmmc.h"
#include "fsl_sdmmc_common.h"
#include "fsl_sdmmc_host.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
static const char *TAG = "SDMMC:dwc_mmc_t";

typedef struct _fdwmmchost_dev_
{
    dwc_mmc_t hc;
    dwc_mmc_config_t hc_cfg;
    dwc_mmc_cmd_t cmd_pkg;
    dwc_mmc_data_t dat_pkg;
    volatile bool cmd_done;
    volatile bool data_done;
    volatile bool cmd_error;
    volatile bool data_error;
} dwmmc_host_dev_t;

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
/*******************************************************************************
 * Variables
 ******************************************************************************/

/*******************************************************************************
 * Code
 ******************************************************************************/
static void dwmmc_host_Relax(void)
{
    SDMMC_OSADelay(1);
}

static void dwmmc_host_CommandDoneCB(void *para)
{
    assert(para);
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)para;

    dev->cmd_done = true;
}

static void dwmmc_host_CommandErrorCB(void *para)
{
    assert(para);
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)para;

    dev->cmd_done = true;
    dev->cmd_error = true;
}

static void dwmmc_host_DataDoneCB(void *para)
{
    assert(para);
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)para;

    dev->data_done = true;
}

static void dwmmc_host_DataErrorCB(void *para)
{
    assert(para);
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)para;

    dev->data_done = true;
    dev->data_error = true;
}

static void dwmmc_host_SetupIrq(dwmmc_host_dev_t *dev)
{
    dwc_mmc_t *ctrl_p = &(dev->hc);
    dwc_mmc_config_t *config_p = &ctrl_p->config;
    uintptr_t base_addr = config_p->base_addr;

    /* disable all interrupt */
    dwc_mmc_set_interrupt_mask(ctrl_p, DWMMC_INTMSK_ALL, false);

    /* clear interrupt status */
    dwc_mmc_clear_interrupt_status(base_addr);

    /* register intr, attach interrupt handler */
    InterruptSetPriority(config_p->irq_num, 0);
    InterruptInstall(config_p->irq_num, dwc_mmc_interrupt_handler, ctrl_p, "DWMMC");

    /* umask and enable fdwmmc interrupt */
    InterruptUmask(config_p->irq_num);

    /* register interrupt event handler */
    dwc_mmc_register_event_handler(ctrl_p, DWMMC_EVT_CMD_DONE, dwmmc_host_CommandDoneCB, dev);
    dwc_mmc_register_event_handler(ctrl_p, DWMMC_EVT_CMD_ERROR, dwmmc_host_CommandErrorCB, dev);
    dwc_mmc_register_event_handler(ctrl_p, DWMMC_EVT_CMD_RESP_ERROR, dwmmc_host_CommandErrorCB, dev);
    dwc_mmc_register_event_handler(ctrl_p, DWMMC_EVT_DATA_READ_DONE, dwmmc_host_DataDoneCB, dev);
    dwc_mmc_register_event_handler(ctrl_p, DWMMC_EVT_DATA_WRITE_DONE, dwmmc_host_DataDoneCB, dev);
    dwc_mmc_register_event_handler(ctrl_p, DWMMC_EVT_DATA_ERROR, dwmmc_host_DataErrorCB, dev);

    return;
}

static void dwmmc_host_RevokeIrq(dwmmc_host_dev_t *dev)
{
    dwc_mmc_t *ctrl_p = &(dev->hc);

    /* disable fdwmmc irq */
    InterruptMask(ctrl_p->config.irq_num);
}

static void dwmmc_host_Deinit(sdmmchost_t *host)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;

    if (NULL == dev)
    {
        return;
    }

    /* 恢复 3.3V 信号电平, 便于下次重新初始化 */
    (void)dwc_mmc_set_signal_voltage(&dev->hc, false);
#if defined(CONFIG_ENABLE_FDWI2C)
    (void)dwc_vqmmc_set_voltage(3300U);
#endif

    dwmmc_host_RevokeIrq(dev);
    dwc_mmc_deinitialize(&dev->hc);

    memset(&dev->hc, 0U, sizeof(dev->hc));
    memset(dev, 0U, sizeof(*dev));
    SDMMC_OSAMemoryFree(dev);
    host->dev = NULL;

    SDMMC_LOG("Dwmmc ctrl deinited !!!");
}

static status_t dwmmc_host_Reset(sdmmchost_t *host)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;
    uintptr_t base_addr = dev->hc.config.base_addr;

    dwc_mmc_software_reset(base_addr, 1000);
    return kStatus_Success;
}

static void dwmmc_host_SwitchToVoltage(sdmmchost_t *host, uint32_t voltage)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;

    if (kSDMMC_OperationVoltage180V == voltage)
    {
        /* DW-MMC 信号电平选择 1.8V + vqmmc(RK809 LDO5)切 1.8V */
        (void)dwc_mmc_set_signal_voltage(&dev->hc, true);
#if defined(CONFIG_ENABLE_FDWI2C)
        (void)dwc_vqmmc_set_voltage(1800U);
#endif
    }
    else
    {
        /* 恢复 3.3V */
        (void)dwc_mmc_set_signal_voltage(&dev->hc, false);
#if defined(CONFIG_ENABLE_FDWI2C)
        (void)dwc_vqmmc_set_voltage(3300U);
#endif
    }
}

static status_t dwmmc_host_ExecuteTuning(sdmmchost_t *host, uint32_t tuningCmd, uint32_t *revBuf, uint32_t blockSize)
{
    /* SDR104 采样相位 tuning: 粗扫 0/90/180/270 相位, 选第一个通过的相位
     * (后续可细化为 linux 式 32 相位扫描) */
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;

    return dwc_mmc_sample_tuning(&dev->hc, tuningCmd, (uint8_t *)revBuf, blockSize);
}

static void dwmmc_host_EnableDDRMode(sdmmchost_t *host, bool enable, uint32_t nibblePos)
{
    /* do not support DDR mode */
}

static void dwmmc_host_EnableHS400Mode(sdmmchost_t *host, bool enable)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
}

static void dwmmc_host_EnableStrobeDll(sdmmchost_t *host, bool enable)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
}

static uint32_t dwmmc_host_GetSignalLineStatus(sdmmchost_t *host, uint32_t signalLine)
{
    /* do not support busy state */
    return true;
}

static void dwmmc_host_ConvertDataToLittleEndian(sdmmchost_t *host, uint32_t *data, uint32_t wordSize, uint32_t format)
{
    /* dw-mmc is little endian native, nothing to do */
}

static status_t dwmmc_host_CardDetectInit(sdmmchost_t *host, void *cd)
{
    return kStatus_Success;
}

static void dwmmc_host_SetCardPower(sdmmchost_t *host, bool enable)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;

    /* 关电时恢复 3.3V, 便于下次重新初始化(1.8V 需断电才能复位) */
    if (!enable)
    {
        (void)dwc_mmc_set_signal_voltage(&dev->hc, false);
#if defined(CONFIG_ENABLE_FDWI2C)
        (void)dwc_vqmmc_set_voltage(3300U);
#endif
    }
}

static void dwmmc_host_EnableCardInt(sdmmchost_t *host, bool enable)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
}

static status_t dwmmc_host_CardIntInit(sdmmchost_t *host, void *sdioInt)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
    return kStatus_Fail;
}

static void dwmmc_host_SetCardBusWidth(sdmmchost_t *host, uint32_t dataBusWidth)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;

    dwc_mmc_set_card_bus_width(&dev->hc, dataBusWidth);
}

static void dwmmc_host_SendCardActive(sdmmchost_t *host)
{
    /* DW-MMC 用 CMD0 + send_initialization, 不需要 host 级 80 clk 特判 */
}

static status_t dwmmc_host_PollingCardDetectStatus(sdmmchost_t *host, uint32_t waitCardStatus, uint32_t timeout)
{
    return kStatus_Success;
}

static uint32_t dwmmc_host_CardDetectStatus(sdmmchost_t *host)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;
    uintptr_t base_addr = dev->hc.config.base_addr;

    return dwc_mmc_card_exists(base_addr) ? kSD_Inserted : kSD_Removed;
}

static uint32_t dwmmc_host_SetCardClock(sdmmchost_t *host, uint32_t targetClock)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;

    if (DWMMC_SUCCESS != dwc_mmc_set_card_clk(&dev->hc, targetClock))
    {
        SDMMC_LOGE(TAG, "Failed to update clock");
        return 0U;
    }

    SDMMC_LOGD(TAG, "BUS CLOCK: %d", targetClock);
    return targetClock;
}

static void dwmmc_host_ForceClockOn(sdmmchost_t *host, bool enable)
{
    /* no support to on/off clock */
}

static bool dwmmc_host_IsCardBusy(sdmmchost_t *host)
{
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;
    uintptr_t base_addr = dev->hc.config.base_addr;

    /* STATUS bit9 = data_busy(反转的 DAT0): 低电平=忙(含电压切换握手) */
    return (0U != (DWMMC_READ_REG(base_addr, DWMMC_STATUS_OFFSET) &
                   DWMMC_STATUS_DATA_BUSY));
}

static status_t dwmmc_host_PreCommand(sdmmchost_t *host, sdmmchost_transfer_t *content)
{
    if ((kSDMMC_ReadSingleBlock == content->command->index) ||
        (kSDMMC_ReadMultipleBlock == content->command->index) ||
        (kSDMMC_WriteSingleBlock == content->command->index) ||
        (kSDMMC_WriteMultipleBlock == content->command->index))
    {
        /* send CMD23 (set block count) before multi-block read/write */
        uint32_t block_count = content->data->blockCount;
        return SDMMC_SetBlockCount(host, block_count);
    }

    return kStatus_Success;
}

static status_t dwmmc_host_PostCommand(sdmmchost_t *host, sdmmchost_transfer_t *content)
{
    return kStatus_Success;
}

static void dwmmc_host_CovertCommandInfo(sdmmchost_t *host, sdmmchost_transfer_t *in_trans,
                                         dwc_mmc_cmd_t *out_cmd, dwc_mmc_data_t *out_data)
{
    uint32_t cmd_ind = in_trans->command->index;

    if (kCARD_ResponseTypeNone != in_trans->command->responseType)
    {
        out_cmd->flag |= DWMMC_CMD_FLAG_EXP_RESP;

        if (kCARD_ResponseTypeR2 == in_trans->command->responseType)
        {
            out_cmd->flag |= DWMMC_CMD_FLAG_EXP_LONG_RESP;
        }
    }

    if (kSDMMC_GoIdleState == cmd_ind)
    {
        out_cmd->flag |= DWMMC_CMD_FLAG_NEED_INIT;
    }

    out_cmd->cmdidx = in_trans->command->index;
    out_cmd->cmdarg = in_trans->command->argument;
    if (NULL != in_trans->data)
    {
        out_cmd->flag |= DWMMC_CMD_FLAG_EXP_DATA;
        if (in_trans->data->rxData)
        {
            out_cmd->flag |= DWMMC_CMD_FLAG_READ_DATA;
            out_data->buf = (uint8_t *)in_trans->data->rxData;
        }
        else
        {
            out_cmd->flag |= DWMMC_CMD_FLAG_WRITE_DATA;
            out_data->buf = (uint8_t *)in_trans->data->txData;
        }

        out_data->blksz = in_trans->data->blockSize;
        out_data->blkcnt = in_trans->data->blockCount;
        out_data->datalen = in_trans->data->blockSize *
                            in_trans->data->blockCount;

        SDMMC_LOGD(TAG, "buf virtual address = 0x%x, len = 0x%x",
                   (uint32_t)(uintptr_t)out_data->buf, out_data->datalen);

        out_cmd->data_p = out_data;
    }

    return;
}

static void dwmmc_host_GetCommandResp(uintptr_t base_addr, dwc_mmc_cmd_t *in_cmd, sdmmchost_transfer_t *out_trans)
{
    if (in_cmd->flag & DWMMC_CMD_FLAG_EXP_RESP)
    {
        if (in_cmd->flag & DWMMC_CMD_FLAG_EXP_LONG_RESP)
        {
            in_cmd->response[0] = DWMMC_READ_REG(base_addr, DWMMC_RESP0_OFFSET);
            in_cmd->response[1] = DWMMC_READ_REG(base_addr, DWMMC_RESP1_OFFSET);
            in_cmd->response[2] = DWMMC_READ_REG(base_addr, DWMMC_RESP2_OFFSET);
            in_cmd->response[3] = DWMMC_READ_REG(base_addr, DWMMC_RESP3_OFFSET);
        }
        else
        {
            in_cmd->response[0] = DWMMC_READ_REG(base_addr, DWMMC_RESP0_OFFSET);
            in_cmd->response[1] = 0U;
            in_cmd->response[2] = 0U;
            in_cmd->response[3] = 0U;
        }

        memcpy((uint32_t *)out_trans->command->response, in_cmd->response, sizeof(uint32_t) * 4);
    }
}

static status_t dwmmc_host_TransferFunction_Poll(sdmmchost_t *host, sdmmchost_transfer_t *content)
{
    assert(content);
    status_t status = kStatus_Success;
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;
    dwc_mmc_cmd_t *cmd_data = &(dev->cmd_pkg);
    dwc_mmc_data_t *trans_data = &(dev->dat_pkg);

#ifdef CONFIG_RK3568_SD_READ_ONLY
    if ((NULL != content->data) && (NULL == content->data->rxData) && (NULL != content->data->txData))
    {
        SDMMC_LOGE(TAG, "Write transfer rejected: RK3568_SD_READ_ONLY.");
        return kStatus_Fail;
    }
#endif

    status = dwmmc_host_PreCommand(host, content);
    if (kStatus_Success != status)
    {
        return status;
    }

    memset(cmd_data, 0U, sizeof(*cmd_data));
    memset(trans_data, 0U, sizeof(*trans_data));

    dwmmc_host_CovertCommandInfo(host, content, cmd_data, trans_data);

    if (DWMMC_SUCCESS == dwc_mmc_poll_transfer(&(dev->hc), cmd_data))
    {
        dwmmc_host_GetCommandResp(dev->hc.config.base_addr, cmd_data, content);
        status = dwmmc_host_PostCommand(host, content);
        SDMMC_LOGI(TAG, "CMD [%d] END: 0x%x.", content->command->index, 0);
    }
    else
    {
        status = kStatus_Fail;
    }

    return status;
}

static status_t dwmmc_host_TransferFunction_Irq(sdmmchost_t *host, sdmmchost_transfer_t *content)
{
    assert(content);
    status_t status = kStatus_Success;
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;
    dwc_mmc_cmd_t *cmd_data = &(dev->cmd_pkg);
    dwc_mmc_data_t *trans_data = &(dev->dat_pkg);
    uint32_t timeout = 2000;
    uint32_t loop;

#ifdef CONFIG_RK3568_SD_READ_ONLY
    if ((NULL != content->data) && (NULL == content->data->rxData) && (NULL != content->data->txData))
    {
        SDMMC_LOGE(TAG, "Write transfer rejected: RK3568_SD_READ_ONLY.");
        return kStatus_Fail;
    }
#endif

    status = dwmmc_host_PreCommand(host, content);
    if (kStatus_Success != status)
    {
        return status;
    }

    memset(cmd_data, 0U, sizeof(*cmd_data));
    memset(trans_data, 0U, sizeof(*trans_data));

    dev->cmd_done = false;
    dev->data_done = false;
    dev->cmd_error = false;
    dev->data_error = false;

    dwmmc_host_CovertCommandInfo(host, content, cmd_data, trans_data);

    if (DWMMC_SUCCESS != dwc_mmc_interrupt_transfer(&(dev->hc), cmd_data))
    {
        return kStatus_Fail;
    }

    /* wait cmd transfer done */
    loop = 0;
    while ((false == dev->cmd_done) && (loop++ < timeout))
    {
        dwmmc_host_Relax();
    }

    if (loop >= timeout)
    {
        SDMMC_LOGE(TAG, "Dwmmc transfer cmd timeout.");
        return kStatus_Fail;
    }

    /* wait data transfer done */
    loop = 0;
    while ((cmd_data->data_p) && (false == dev->data_done) && (loop++ < timeout))
    {
        dwmmc_host_Relax();
    }

    if ((cmd_data->data_p) && (loop >= timeout))
    {
        SDMMC_LOGE(TAG, "Dwmmc transfer data timeout.");
        return kStatus_Fail;
    }

    dwmmc_host_GetCommandResp(dev->hc.config.base_addr, cmd_data, content);
    status = dwmmc_host_PostCommand(host, content);

    return status;
}

static const sdmmchost_ops_t fdwmmc_ops =
{
    .deinit = dwmmc_host_Deinit,
    .reset = dwmmc_host_Reset,

    .switchToVoltage = dwmmc_host_SwitchToVoltage,
    .executeTuning = dwmmc_host_ExecuteTuning,
    .enableDDRMode = dwmmc_host_EnableDDRMode,
    .enableHS400Mode = dwmmc_host_EnableHS400Mode,
    .enableStrobeDll = dwmmc_host_EnableStrobeDll,
    .getSignalLineStatus = dwmmc_host_GetSignalLineStatus,
    .convertDataToLittleEndian = dwmmc_host_ConvertDataToLittleEndian,

    .cardDetectInit = dwmmc_host_CardDetectInit,
    .cardSetPower = dwmmc_host_SetCardPower,
    .cardEnableInt = dwmmc_host_EnableCardInt,
    .cardIntInit = dwmmc_host_CardIntInit,
    .cardSetBusWidth = dwmmc_host_SetCardBusWidth,
    .cardPollingDetectStatus = dwmmc_host_PollingCardDetectStatus,
    .cardDetectStatus = dwmmc_host_CardDetectStatus,
    .cardSendActive = dwmmc_host_SendCardActive,
    .cardSetClock = dwmmc_host_SetCardClock,
    .cardForceClockOn = dwmmc_host_ForceClockOn,
    .cardIsBusy = dwmmc_host_IsCardBusy,

    .transferFunction = NULL,

    .startBoot = NULL,
    .readBootData = NULL,
    .enableBoot = NULL,
};

static status_t dwmmc_host_DoInit(sdmmchost_t *host)
{
    status_t ret = kStatus_Success;
    dwmmc_host_dev_t *dev = (dwmmc_host_dev_t *)host->dev;
    dev->hc_cfg = *dwc_mmc_config(host->config.hostId);

    /* init dwmmc ctrl */
    memset(&dev->hc, 0, sizeof(dev->hc));
    if (DWMMC_SUCCESS != dwc_mmc_initialize(&dev->hc, &dev->hc_cfg))
    {
        SDMMC_LOGE(TAG, "Dwmmc ctrl init failed.");
        ret = kStatus_Fail;
        return ret;
    }

    if (host->config.enableIrq)
    {
        dwmmc_host_SetupIrq(dev);
    }

    return ret;
}

status_t dwmmc_host_Init(sdmmchost_t *host)
{
    assert(host);

    /* find the space for dev instance */
    dwmmc_host_dev_t *dev = SDMMC_OSAMemoryAllocate(sizeof(dwmmc_host_dev_t));
    if (NULL == dev)
    {
        return kStatus_OutOfRange;
    }

    memset(dev, 0U, sizeof(*dev));

    SDMMC_LOGI(TAG, "Allocate INST@0x%x", (uint32_t)(uintptr_t)&(dev->hc));

    host->ops = fdwmmc_ops;
    host->dev = dev;

    if (host->config.enableIrq)
    {
        host->ops.transferFunction = dwmmc_host_TransferFunction_Irq;
    }
    else
    {
        host->ops.transferFunction = dwmmc_host_TransferFunction_Poll;
    }

    return dwmmc_host_DoInit(host);
}

/* NOTE: the original fdwmmc glue carried SDConfig/MMCConfig helpers
 * referencing symbols absent from its own protocol headers
 * (kSDMMC_Support4BitWidth, kMMC_VoltageWindow320to340) - they were never
 * part of any standalone build (only the fdwmshc eMMC line was compiled
 * there) and are not ported. sdmmc0 carries an SDIO card; its capability
 * fields are filled in sdmmc_adapter.c. */
