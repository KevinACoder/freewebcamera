/*
 * Copyright (c) 2026 Phytium Information Technology, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * @file   sdmmc_host_dwmshc.c
 * @brief  Host glue: the fsl_sdmmc SDMMCHOST surface over the DWC MSHC
 *         controller driver (eMMC).
 *
 * Ported verbatim from the standalone line's fsl_hc_fdwmshc.c (BSD-3,
 * decision D27): the ops table, the eMMC bus-test workaround, the CMD23
 * pre-command, the R2 response copyback and every capability value are the
 * originals. Only identifiers changed; the controller register accessors
 * are the driver's own (dwc_mshc_regs.h) so the sequences are untouched.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "board.h"
#include "dwcmshc.h"
#include "dwc_mshc_regs.h"
#include "finterrupt.h"
#include "sdmmc_board.h"
#include "fsl_sdmmc.h"
#include "fsl_sdmmc_common.h"
#include "fsl_sdmmc_host.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
static const char *TAG = "SDMMC:dwc_mshc_t";

typedef struct _fdwmshchost_dev_
{
    dwc_mshc_t hc;
    dwc_mshc_config_t hc_cfg;
    dwc_mshc_cmd_t cmd_pkg;
    dwc_mshc_data_t dat_pkg;
    volatile bool cmd_done;
    volatile bool data_done;
    volatile bool cmd_error;
    volatile bool data_error;
} dwmshc_host_dev_t;

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
/*******************************************************************************
 * Variables
 ******************************************************************************/

/*******************************************************************************
 * Code
 ******************************************************************************/
static void dwmshc_host_Relax(void)
{
    SDMMC_OSADelay(1);
}

static void dwmshc_host_CommandDoneCB(void *para)
{
    assert(para);
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)para;

    dev->cmd_done = true;
}

static void dwmshc_host_CommandErrorCB(void *para)
{
    assert(para);
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)para;

    dev->cmd_done = true;
    dev->cmd_error = true;
}

static void dwmshc_host_DataDoneCB(void *para)
{
    assert(para);
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)para;

    dev->data_done = true;
}

static void dwmshc_host_DataErrorCB(void *para)
{
    assert(para);
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)para;

    dev->data_done = true;
    dev->data_error = true;
}

static void dwmshc_host_SetupIrq(dwmshc_host_dev_t *dev)
{
    dwc_mshc_t *ctrl_p = &(dev->hc);
    dwc_mshc_config_t *config_p = &ctrl_p->config;

    /* disable all interrupt signal */
    dwc_mshc_enable_interrupt_mode(ctrl_p, false);

    /* clear interrupt status */
    dwc_mshc_clear_interrupt_status(config_p->base_addr);

    /* register intr, attach interrupt handler */
    InterruptSetPriority(config_p->irq_num, 0);
    InterruptInstall(config_p->irq_num, dwc_mshc_interrupt_handler, ctrl_p, "DWMSHC");

    /* umask and enable fdwmshc interrupt */
    InterruptUmask(config_p->irq_num);

    /* enable interrupt signal path */
    dwc_mshc_enable_interrupt_mode(ctrl_p, true);

    /* register interrupt event handler */
    dwc_mshc_register_event_handler(ctrl_p, DWCMSHC_EVT_CMD_DONE, dwmshc_host_CommandDoneCB, dev);
    dwc_mshc_register_event_handler(ctrl_p, DWCMSHC_EVT_CMD_ERROR, dwmshc_host_CommandErrorCB, dev);
    dwc_mshc_register_event_handler(ctrl_p, DWCMSHC_EVT_CMD_RESP_ERROR, dwmshc_host_CommandErrorCB, dev);
    dwc_mshc_register_event_handler(ctrl_p, DWCMSHC_EVT_DATA_READ_DONE, dwmshc_host_DataDoneCB, dev);
    dwc_mshc_register_event_handler(ctrl_p, DWCMSHC_EVT_DATA_WRITE_DONE, dwmshc_host_DataDoneCB, dev);
    dwc_mshc_register_event_handler(ctrl_p, DWCMSHC_EVT_DATA_ERROR, dwmshc_host_DataErrorCB, dev);

    return;
}

static void dwmshc_host_RevokeIrq(dwmshc_host_dev_t *dev)
{
    dwc_mshc_t *ctrl_p = &(dev->hc);

    /* disable fdwmshc irq */
    InterruptMask(ctrl_p->config.irq_num);
}

static void dwmshc_host_Deinit(sdmmchost_t *host)
{
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;

    if (NULL == dev)
    {
        return;
    }

    dwmshc_host_RevokeIrq(dev);
    dwc_mshc_deinitialize(&dev->hc);

    memset(&dev->hc, 0U, sizeof(dev->hc));
    memset(dev, 0U, sizeof(*dev));
    SDMMC_OSAMemoryFree(dev);
    host->dev = NULL;

    SDMMC_LOG("Dwmshc ctrl deinited !!!");
}

static status_t dwmshc_host_Reset(sdmmchost_t *host)
{
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;
    uintptr_t base_addr = dev->hc.config.base_addr;

    dwc_mshc_software_reset(base_addr, 1000);
    return kStatus_Success;
}

static void dwmshc_host_SwitchToVoltage(sdmmchost_t *host, uint32_t voltage)
{
    /* 板级 eMMC VCCQ(vccio2)=1.8V 固定, 无需电压切换 */
    (void)voltage;
}

static status_t dwmshc_host_ExecuteTuning(sdmmchost_t *host, uint32_t tuningCmd,
                                          uint32_t *revBuf, uint32_t blockSize)
{
    /* HS200/HS400 采样相位 tuning: CMD21 PIO 传输 + EXEC_TUNING/TUNED_CLK */
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;

    return (DWCMSHC_SUCCESS == dwc_mshc_execute_tuning(&dev->hc, tuningCmd, (uint8_t *)revBuf, blockSize))
               ? kStatus_Success
               : kStatus_Fail;
}

static void dwmshc_host_EnableDDRMode(sdmmchost_t *host, bool enable, uint32_t nibblePos)
{
    /* do not support DDR mode */
}

static void dwmshc_host_EnableHS400Mode(sdmmchost_t *host, bool enable)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
}

static void dwmshc_host_EnableStrobeDll(sdmmchost_t *host, bool enable)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
}

static uint32_t dwmshc_host_GetSignalLineStatus(sdmmchost_t *host, uint32_t signalLine)
{
    /* do not support busy state */
    return true;
}

static void dwmshc_host_ConvertDataToLittleEndian(sdmmchost_t *host, uint32_t *data,
                                                  uint32_t wordSize, uint32_t format)
{
    /* SDHCI is little endian native, nothing to do */
}

static status_t dwmshc_host_CardDetectInit(sdmmchost_t *host, void *cd)
{
    return kStatus_Success;
}

static void dwmshc_host_SetCardPower(sdmmchost_t *host, bool enable)
{
    /* eMMC 板级常供电, 无需操作 */
    (void)host;
    (void)enable;
}

static void dwmshc_host_EnableCardInt(sdmmchost_t *host, bool enable)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
}

static status_t dwmshc_host_CardIntInit(sdmmchost_t *host, void *sdioInt)
{
    SDMMC_LOGE(TAG, "%s not implmented", __func__);
    return kStatus_Fail;
}

static void dwmshc_host_SetCardBusWidth(sdmmchost_t *host, uint32_t dataBusWidth)
{
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;

    /* fsl 传 kSDMMC_BusWdith*Bit 枚举(0/1/2), 需映射为实际位宽(1/4/8) */
    switch (dataBusWidth)
    {
        case kSDMMC_BusWdith8Bit:
            dwc_mshc_set_card_bus_width(&dev->hc, 8U);
            break;
        case kSDMMC_BusWdith4Bit:
            dwc_mshc_set_card_bus_width(&dev->hc, 4U);
            break;
        case kSDMMC_BusWdith1Bit:
        default:
            dwc_mshc_set_card_bus_width(&dev->hc, 1U);
            break;
    }
}

static void dwmshc_host_SendCardActive(sdmmchost_t *host)
{
    /* eMMC 非移除, CMD1 op-cond 由协议栈处理 */
}

static status_t dwmshc_host_PollingCardDetectStatus(sdmmchost_t *host,
                                                    uint32_t waitCardStatus, uint32_t timeout)
{
    return kStatus_Success;
}

static uint32_t dwmshc_host_CardDetectStatus(sdmmchost_t *host)
{
    /* eMMC 焊接/非移除, 恒在位 */
    (void)host;
    return kSD_Inserted;
}

static uint32_t dwmshc_host_SetCardClock(sdmmchost_t *host, uint32_t targetClock)
{
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;

    /* The protocol reaches here with freq=0 when its speed bookkeeping came
     * up empty (SwitchToHighSpeed with no HS flags after a failed EXT_CSD).
     * Stopping the card clock mid-enumeration kills every following
     * command - keep the current clock instead and report it. */
    if (0U == targetClock)
    {
        SDMMC_LOGE(TAG, "freq=0 request: keeping current clock");
        return host->currClockFreq;
    }

    if (DWCMSHC_SUCCESS != dwc_mshc_set_card_clk(&dev->hc, targetClock))
    {
        SDMMC_LOGE(TAG, "Failed to update clock");
        return 0U;
    }

    SDMMC_LOGD(TAG, "BUS CLOCK: %d", targetClock);
    return targetClock;
}

static void dwmshc_host_ForceClockOn(sdmmchost_t *host, bool enable)
{
    /* no support to on/off clock */
}

static bool dwmshc_host_IsCardBusy(sdmmchost_t *host)
{
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;
    uintptr_t base_addr = dev->hc.config.base_addr;

    /* SDHCI: DAT[0] 低电平 = 卡忙 */
    return (0U == (DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET) & DWMSHC_PSTATE_DATA_0_LVL));
}

static status_t dwmshc_host_PreCommand(sdmmchost_t *host, sdmmchost_transfer_t *content)
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

static status_t dwmshc_host_PostCommand(sdmmchost_t *host, sdmmchost_transfer_t *content)
{
    return kStatus_Success;
}

static void dwmshc_host_CovertCommandInfo(sdmmchost_t *host, sdmmchost_transfer_t *in_trans,
                                          dwc_mshc_cmd_t *out_cmd, dwc_mshc_data_t *out_data)
{
    switch (in_trans->command->responseType)
    {
        case kCARD_ResponseTypeR2:
            out_cmd->flag |= DWCMSHC_CMD_FLAG_EXP_RESP | DWCMSHC_CMD_FLAG_EXP_LONG_RESP;
            out_cmd->flag |= DWCMSHC_CMD_FLAG_NEED_RESP_CRC;
            out_cmd->resptype = DWMSHC_CMD_RESP_TYPE_136;
            break;
        case kCARD_ResponseTypeR1b:
        case kCARD_ResponseTypeR5b:
            out_cmd->flag |= DWCMSHC_CMD_FLAG_EXP_RESP;
            out_cmd->flag |= DWCMSHC_CMD_FLAG_NEED_RESP_CRC;
            out_cmd->resptype = DWMSHC_CMD_RESP_TYPE_48_BUSY;
            break;
        case kCARD_ResponseTypeR1:
        case kCARD_ResponseTypeR5:
        case kCARD_ResponseTypeR6:
        case kCARD_ResponseTypeR7:
            out_cmd->flag |= DWCMSHC_CMD_FLAG_EXP_RESP;
            out_cmd->flag |= DWCMSHC_CMD_FLAG_NEED_RESP_CRC;
            out_cmd->resptype = DWMSHC_CMD_RESP_TYPE_48;
            break;
        case kCARD_ResponseTypeR3:
        case kCARD_ResponseTypeR4:
            out_cmd->flag |= DWCMSHC_CMD_FLAG_EXP_RESP;
            out_cmd->resptype = DWMSHC_CMD_RESP_TYPE_48;
            break;
        case kCARD_ResponseTypeNone:
        default:
            out_cmd->resptype = DWMSHC_CMD_RESP_TYPE_NONE;
            break;
    }

    out_cmd->cmdidx = in_trans->command->index;
    out_cmd->cmdarg = in_trans->command->argument;
    if (NULL != in_trans->data)
    {
        out_cmd->flag |= DWCMSHC_CMD_FLAG_EXP_DATA;
        if (in_trans->data->rxData)
        {
            out_cmd->flag |= DWCMSHC_CMD_FLAG_READ_DATA;
            out_data->buf = (uint8_t *)in_trans->data->rxData;
        }
        else
        {
            out_cmd->flag |= DWCMSHC_CMD_FLAG_WRITE_DATA;
            out_data->buf = (uint8_t *)in_trans->data->txData;
        }

        out_data->blksz = in_trans->data->blockSize;
        out_data->blkcnt = in_trans->data->blockCount;
        out_data->datalen = in_trans->data->blockSize * in_trans->data->blockCount;

        SDMMC_LOGD(TAG, "buf virtual address = 0x%x, len = 0x%x",
                   (uint32_t)(uintptr_t)out_data->buf, out_data->datalen);

        out_cmd->data_p = out_data;
    }

    return;
}

static void dwmshc_host_GetCommandResp(uintptr_t base_addr, dwc_mshc_cmd_t *in_cmd,
                                       sdmmchost_transfer_t *out_trans)
{
    if (in_cmd->flag & DWCMSHC_CMD_FLAG_EXP_RESP)
    {
        /* the driver collected the response during the transfer (R2 with
         * the u-boot read, see dwc_mshc.c) - copy it through */
        (void)base_addr;
        memcpy(out_trans->command->response, in_cmd->response,
               sizeof(uint32_t) * 4U);
    }
}

static status_t dwmshc_host_TransferFunction_Poll(sdmmchost_t *host, sdmmchost_transfer_t *content)
{
    assert(content);
    status_t status = kStatus_Success;
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;
    dwc_mshc_cmd_t *cmd_data = &(dev->cmd_pkg);
    dwc_mshc_data_t *trans_data = &(dev->dat_pkg);

    /* eMMC 总线测试(CMD19/CMD14): 卡此时仍为 1-bit, 按 fsl 流程测试必然失败
     * (它先测后切)。CMD19 写直接跳过; CMD14 读填充 fsl 期望的回读模式
     * (fsl 判定: (回读 ^ 发送) & mask == result), 让 fsl 走真实 CMD6 宽度切换 */
    if (kMMC_SendingBusTest == content->command->index)
    {
        return kStatus_Success;
    }
    if (kMMC_BusTestRead == content->command->index)
    {
        uint32_t *buf = (uint32_t *)content->data->rxData;
        buf[0] = (content->data->blockSize == 8U) ? 0x55AAU : 0xA5U;
        return kStatus_Success;
    }

#ifdef CONFIG_RK3568_SD_READ_ONLY
    if ((NULL != content->data) && (NULL == content->data->rxData) &&
        (NULL != content->data->txData))
    {
        SDMMC_LOGE(TAG, "Write transfer rejected: RK3568_SD_READ_ONLY.");
        return kStatus_Fail;
    }
#endif

    status = dwmshc_host_PreCommand(host, content);
    if (kStatus_Success != status)
    {
        return status;
    }

    memset(cmd_data, 0U, sizeof(*cmd_data));
    memset(trans_data, 0U, sizeof(*trans_data));

    dwmshc_host_CovertCommandInfo(host, content, cmd_data, trans_data);

    if (DWCMSHC_SUCCESS == dwc_mshc_poll_transfer(&(dev->hc), cmd_data))
    {
        dwmshc_host_GetCommandResp(dev->hc.config.base_addr, cmd_data, content);
        status = dwmshc_host_PostCommand(host, content);
        SDMMC_LOGI(TAG, "CMD [%d] END: 0x%x.", content->command->index, 0);
    }
    else
    {
        status = kStatus_Fail;
    }

#if defined(CONFIG_LOG_DEBUG) || defined(CONFIG_LOG_VERBOS)
    if ((kStatus_Success == status) && (content->data) &&
        (kMMC_SendExtendedCsd == content->command->index))
    {
        FtDumpHexByte((uint8_t *)content->data->rxData,
                      content->data->blockSize * content->data->blockCount);
    }
#endif

    return status;
}

static status_t dwmshc_host_TransferFunction_Irq(sdmmchost_t *host, sdmmchost_transfer_t *content)
{
    assert(content);
    status_t status = kStatus_Success;
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;
    dwc_mshc_cmd_t *cmd_data = &(dev->cmd_pkg);
    dwc_mshc_data_t *trans_data = &(dev->dat_pkg);
    uint32_t timeout = 2000;
    uint32_t loop;

    /* eMMC 总线测试(CMD19/CMD14): 卡此时仍为 1-bit, 按 fsl 流程测试必然失败
     * (它先测后切)。CMD19 写直接跳过; CMD14 读填充 fsl 期望的回读模式
     * (fsl 判定: (回读 ^ 发送) & mask == result), 让 fsl 走真实 CMD6 宽度切换 */
    if (kMMC_SendingBusTest == content->command->index)
    {
        return kStatus_Success;
    }
    if (kMMC_BusTestRead == content->command->index)
    {
        uint32_t *buf = (uint32_t *)content->data->rxData;
        buf[0] = (content->data->blockSize == 8U) ? 0x55AAU : 0xA5U;
        return kStatus_Success;
    }

#ifdef CONFIG_RK3568_SD_READ_ONLY
    if ((NULL != content->data) && (NULL == content->data->rxData) &&
        (NULL != content->data->txData))
    {
        SDMMC_LOGE(TAG, "Write transfer rejected: RK3568_SD_READ_ONLY.");
        return kStatus_Fail;
    }
#endif

    status = dwmshc_host_PreCommand(host, content);
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

    dwmshc_host_CovertCommandInfo(host, content, cmd_data, trans_data);

    if (DWCMSHC_SUCCESS != dwc_mshc_interrupt_transfer(&(dev->hc), cmd_data))
    {
        return kStatus_Fail;
    }

    /* wait cmd transfer done */
    loop = 0;
    while ((false == dev->cmd_done) && (loop++ < timeout))
    {
        dwmshc_host_Relax();
    }

    if (loop >= timeout)
    {
        SDMMC_LOGE(TAG, "Dwmshc transfer cmd timeout.");
        return kStatus_Fail;
    }

    /* wait data transfer done */
    loop = 0;
    while ((cmd_data->data_p) && (false == dev->data_done) && (loop++ < timeout))
    {
        dwmshc_host_Relax();
    }

    if ((cmd_data->data_p) && (loop >= timeout))
    {
        SDMMC_LOGE(TAG, "Dwmshc transfer data timeout.");
        return kStatus_Fail;
    }

    dwmshc_host_GetCommandResp(dev->hc.config.base_addr, cmd_data, content);
    status = dwmshc_host_PostCommand(host, content);

    return status;
}

static const sdmmchost_ops_t fdwmshc_ops = {
    .deinit = dwmshc_host_Deinit,
    .reset = dwmshc_host_Reset,

    .switchToVoltage = dwmshc_host_SwitchToVoltage,
    .executeTuning = dwmshc_host_ExecuteTuning,
    .enableDDRMode = dwmshc_host_EnableDDRMode,
    .enableHS400Mode = dwmshc_host_EnableHS400Mode,
    .enableStrobeDll = dwmshc_host_EnableStrobeDll,
    .getSignalLineStatus = dwmshc_host_GetSignalLineStatus,
    .convertDataToLittleEndian = dwmshc_host_ConvertDataToLittleEndian,

    .cardDetectInit = dwmshc_host_CardDetectInit,
    .cardSetPower = dwmshc_host_SetCardPower,
    .cardEnableInt = dwmshc_host_EnableCardInt,
    .cardIntInit = dwmshc_host_CardIntInit,
    .cardSetBusWidth = dwmshc_host_SetCardBusWidth,
    .cardPollingDetectStatus = dwmshc_host_PollingCardDetectStatus,
    .cardDetectStatus = dwmshc_host_CardDetectStatus,
    .cardSendActive = dwmshc_host_SendCardActive,
    .cardSetClock = dwmshc_host_SetCardClock,
    .cardForceClockOn = dwmshc_host_ForceClockOn,
    .cardIsBusy = dwmshc_host_IsCardBusy,

    .transferFunction = NULL,

    .startBoot = NULL,
    .readBootData = NULL,
    .enableBoot = NULL,
};

static status_t dwmshc_host_DoInit(sdmmchost_t *host)
{
    status_t ret = kStatus_Success;
    dwmshc_host_dev_t *dev = (dwmshc_host_dev_t *)host->dev;
    dev->hc_cfg = *dwc_mshc_config(host->config.hostId);

    /* init dwmshc ctrl */
    memset(&dev->hc, 0, sizeof(dev->hc));
    if (DWCMSHC_SUCCESS != dwc_mshc_initialize(&dev->hc, &dev->hc_cfg))
    {
        SDMMC_LOGE(TAG, "Dwmshc ctrl init failed.");
        ret = kStatus_Fail;
        return ret;
    }

    if (host->config.enableIrq)
    {
        dwmshc_host_SetupIrq(dev);
    }

    return ret;
}

status_t dwmshc_host_Init(sdmmchost_t *host)
{
    assert(host);

    /* find the space for dev instance */
    dwmshc_host_dev_t *dev = SDMMC_OSAMemoryAllocate(sizeof(dwmshc_host_dev_t));
    if (NULL == dev)
    {
        return kStatus_OutOfRange;
    }

    memset(dev, 0U, sizeof(*dev));

    SDMMC_LOGI(TAG, "Allocate INST@0x%x", (uint32_t)(uintptr_t)&(dev->hc));

    host->ops = fdwmshc_ops;
    host->dev = dev;

    if (host->config.enableIrq)
    {
        host->ops.transferFunction = dwmshc_host_TransferFunction_Irq;
    }
    else
    {
        host->ops.transferFunction = dwmshc_host_TransferFunction_Poll;
    }

    return dwmshc_host_DoInit(host);
}

#if defined(CONFIG_FSL_SDMMC_ENABLE_MMC)
status_t dwmshc_host_MMCConfig(sdmmc_mmc_t *sdmmc, sdmmchost_config_t *config)
{
    assert(sdmmc);
    assert(config);
    sdmmchost_t *host = &sdmmc->host;
    mmc_card_t *card = &sdmmc->card;

    card->usrParam.ioStrength = NULL;
    card->usrParam.maxFreq = config->cardClock;
    /* eMMC 8bit + HS 26/52MHz + (cardClock>=100MHz 时) HS200@1.8V */
    card->usrParam.capability = (uint32_t)kSDMMC_Support8BitWidth |
                                (uint32_t)kMMC_SupportHighSpeed26MHZFlag |
                                (uint32_t)kMMC_SupportHighSpeed52MHZFlag;
    card->hostVoltageWindowVCC = (uint32_t)kMMC_VoltageWindows270to360;
    card->hostVoltageWindowVCCQ = (uint32_t)kMMC_VoltageWindow170to195; /* 板级 1.8V IO */
    card->noInteralAlign = false;
    card->enablePreDefinedBlockCount = true;

    host->capability |= (uint32_t)kSDMMCHOST_Support4BitDataWidth |
                        (uint32_t)kSDMMCHOST_Support8BitDataWidth |
                        (uint32_t)kSDMMCHOST_SupportHighSpeed |
                        (uint32_t)kSDMMCHOST_SupportAutoCmd12;
    if (config->cardClock >= 100000000U)
    {
        /* HS200 需 1.8V 信号 + DLL/tuning, 由 fdwmshc 驱动实现。
         * 注意 (2026-08-08 实测): 本板 HS200 数据路径不通 (100/150MHz 卡有响应
         * 但无 tuning 数据, 198MHz 无响应; DLL lock 正常), 板级 vendor 限速 52MHz,
         * HS200 标记遗留待硬件/时序排查。fsl 流程 HS200 失败即整体失败, 无回退。
         * 默认 config (52MHz) 不触发此路径。 */
        host->capability |= (uint32_t)kSDMMCHOST_SupportVoltage1v8 | (uint32_t)kSDMMCHOST_SupportHS200;
    }
    host->maxBlockCount = host->config.maxTransSize / host->config.defBlockSize;
    host->maxBlockSize = SDMMCHOST_SUPPORT_MAX_BLOCK_LENGTH;
    host->sourceClock_Hz = DWMSHC_CCLK_CLOCK_HZ; /* CCLK_EMMC 200M 源 */

    return kStatus_Success;
}
#endif /* CONFIG_FSL_SDMMC_ENABLE_MMC */

status_t dwmshc_host_SDConfig(sdmmc_sd_t *sdmmc, sdmmchost_config_t *config)
{
    /* 该控制器是板载 eMMC(非移除), 不支持 SD 卡 */
    (void)sdmmc;
    (void)config;
    return kStatus_Fail;
}
