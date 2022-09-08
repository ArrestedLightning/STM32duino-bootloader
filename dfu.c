/* *****************************************************************************
 * The MIT License
 *
 * Copyright (c) 2010 LeafLabs LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * ****************************************************************************/

/**
 *  @file dfu.c
 *
 *  @brief The principle dfu state machine as well as the data
 *  transfer callbacks accessed by the usb library
 *
 *
 */
#include "hardware.h"
#include "dfu.h"
#include "usb.h"

/* DFU globals */
static volatile u32 userAppAddr; /* default RAM user code location */
static volatile u32 userAppEnd;
static volatile DFUStatus dfuAppStatus;       /* includes state */
volatile bool dfuBusy = FALSE;


static volatile u8 recvBuffer[LARGEST_FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static volatile u32 userFirmwareLen = 0;
static volatile u16 thisBlockLen = 0;
static volatile u16 uploadBlockLen = 0;


volatile PLOT code_copy_lock;

/* todo: force dfu globals to be singleton to avoid re-inits? */
void dfuInit(void) {
    dfuAppStatus.bStatus = OK;
    dfuAppStatus.bwPollTimeout0 = 0x00;
    dfuAppStatus.bwPollTimeout1 = 0x00;
    dfuAppStatus.bwPollTimeout2 = 0x00;
    dfuAppStatus.bState = dfuIDLE;
    dfuAppStatus.iString = 0x00;          /* all strings must be 0x00 until we make them! */
    userFirmwareLen = 0;
    thisBlockLen = 0;
    userAppAddr = USER_CODE_FLASH0X8002000;
    userAppEnd = getFlashEnd();
    code_copy_lock = WAIT;
    dfuBusy = FALSE;
}




bool dfuUpdateByRequest(void) {
    /* were using the global pInformation struct from usb_lib here,
       see comment in maple_dfu.h around DFUEvent struct */
    dfuBusy = TRUE;

    u8 startState = dfuAppStatus.bState;
    dfuAppStatus.bStatus = OK;
    /* often leaner to nest if's then embed a switch/case */
    if (startState == dfuIDLE)  {
        /*  device running inside DFU mode */
        dfuBusy = TRUE; // signals the main loop to defer to the dfu write-loop

        if (pInformation->USBbRequest == DFU_DNLOAD) {

            if (pInformation->USBwLengths.w > 0) {
                userFirmwareLen = 0;
                dfuAppStatus.bState  = dfuDNLOAD_SYNC;
            } else {
                dfuAppStatus.bState  = dfuERROR;
                dfuAppStatus.bStatus = errNOTDONE;
            }
        } else if (pInformation->USBbRequest == DFU_UPLOAD) {
            dfuAppStatus.bState  = dfuUPLOAD_IDLE;
            /* record length of first block for calculating target
               address from wValue in consecutive blocks */
            uploadBlockLen = pInformation->USBwLengths.w;
            thisBlockLen = uploadBlockLen; /* for this first block as well */
            /* calculate where the data should be copied from */
            userFirmwareLen = uploadBlockLen * pInformation->USBwValue;
        } else if (pInformation->USBbRequest == DFU_ABORT) {
            dfuAppStatus.bState  = dfuIDLE;
            dfuAppStatus.bStatus = OK;  /* are we really ok? we were just aborted */
        } else if (pInformation->USBbRequest == DFU_GETSTATUS) {
            dfuAppStatus.bState  = dfuIDLE;
        } else if (pInformation->USBbRequest == DFU_GETSTATE) {
            dfuAppStatus.bState  = dfuIDLE;
        } else {
            dfuAppStatus.bState  = dfuERROR;
            dfuAppStatus.bStatus = errSTALLEDPKT;
        }

    } else if (startState == dfuDNLOAD_SYNC)         {
        /* device received block, waiting for DFU_GETSTATUS request */

        if (pInformation->USBbRequest == DFU_GETSTATUS) {
            /* todo, add routine to wait for last block write to finish */

            {
                dfuAppStatus.bState = dfuDNLOAD_IDLE;
                dfuCopyBufferToExec();
            }

        } else if (pInformation->USBbRequest == DFU_GETSTATE) {
            dfuAppStatus.bState  = dfuDNLOAD_SYNC;
        } else {
            dfuAppStatus.bState  = dfuERROR;
            dfuAppStatus.bStatus = errSTALLEDPKT;
        }

    } else if (startState == dfuDNBUSY)              {
        /* if were actually done writing, goto sync, else stay busy */
        if (code_copy_lock == END) {
            dfuAppStatus.bwPollTimeout0 = 0x00;
            code_copy_lock = WAIT;
            dfuAppStatus.bState = dfuDNLOAD_IDLE;
        } else {
            dfuAppStatus.bState = dfuDNBUSY;
        }

    } else if (startState == dfuDNLOAD_IDLE)         {
        /* device is expecting dfu_dnload requests */
        if (pInformation->USBbRequest == DFU_DNLOAD) {
            if (pInformation->USBwLengths.w > 0) {
                dfuAppStatus.bState  = dfuDNLOAD_SYNC;
            } else {
                /* todo, support "disagreement" if device expects more data than this */
                dfuAppStatus.bState  = dfuMANIFEST_SYNC;

                /* relock the flash */
                flashLock();
            }
        } else if (pInformation->USBbRequest == DFU_ABORT) {
            dfuAppStatus.bState  = dfuIDLE;
        } else if (pInformation->USBbRequest == DFU_GETSTATUS) {
            dfuAppStatus.bState  = dfuIDLE;
        } else if (pInformation->USBbRequest == DFU_GETSTATE) {
            dfuAppStatus.bState  = dfuIDLE;
        } else {
            dfuAppStatus.bState  = dfuERROR;
            dfuAppStatus.bStatus = errSTALLEDPKT;
        }

    } else if (startState == dfuMANIFEST_SYNC)       {
        /* device has received last block, waiting DFU_GETSTATUS request */

        if (pInformation->USBbRequest == DFU_GETSTATUS) {
            dfuAppStatus.bState  = dfuMANIFEST_WAIT_RESET;
            dfuAppStatus.bStatus = OK;
        } else if (pInformation->USBbRequest == DFU_GETSTATE) {
            dfuAppStatus.bState  = dfuMANIFEST_SYNC;
        } else {
            dfuAppStatus.bState  = dfuERROR;
            dfuAppStatus.bStatus = errSTALLEDPKT;
        }

    } else if (startState == dfuMANIFEST)            {
        /* device is in manifestation phase */

        /* should never receive request while in manifest! */
        dfuAppStatus.bState  = dfuMANIFEST_WAIT_RESET;
        dfuAppStatus.bStatus = OK;

    } else if (startState == dfuMANIFEST_WAIT_RESET) {
        /* device has programmed new firmware but needs external
           usb reset or power on reset to run the new code */

        /* consider timing out and self-resetting */
        dfuAppStatus.bState  = dfuMANIFEST_WAIT_RESET;

    } else if (startState == dfuUPLOAD_IDLE)         {
        /* device expecting further dfu_upload requests */

        if (pInformation->USBbRequest == DFU_UPLOAD) {
            if (pInformation->USBwLengths.w > 0) {
                /* check that this is not the last possible block */
                userFirmwareLen = uploadBlockLen * pInformation->USBwValue;
                if (userAppAddr + userFirmwareLen + uploadBlockLen <= userAppEnd) {
                    thisBlockLen = uploadBlockLen;
                    dfuAppStatus.bState  = dfuUPLOAD_IDLE;
                } else {
                    /* if above comparison was just equal, thisBlockLen becomes zero
                    next time when USBWValue has been increased by one */
                    thisBlockLen = userAppEnd - userAppAddr - userFirmwareLen;
                    /* check for overflow due to USBwValue out of range */
                    if (thisBlockLen >= pInformation->USBwLengths.w) {
                        thisBlockLen = 0;
                    }
                    dfuAppStatus.bState  = dfuIDLE;
                }
            } else {
                dfuAppStatus.bState  = dfuERROR;
                dfuAppStatus.bStatus = errNOTDONE;
            }
        } else if (pInformation->USBbRequest == DFU_ABORT) {
            dfuAppStatus.bState  = dfuIDLE;
        } else if (pInformation->USBbRequest == DFU_GETSTATUS) {
            dfuAppStatus.bState  = dfuUPLOAD_IDLE;
        } else if (pInformation->USBbRequest == DFU_GETSTATE) {
            dfuAppStatus.bState  = dfuUPLOAD_IDLE;
        } else {
            dfuAppStatus.bState  = dfuERROR;
            dfuAppStatus.bStatus = errSTALLEDPKT;
        }


    } else if (startState == dfuERROR)               {
        /* status is in error, awaiting DFU_CLRSTATUS request */

        if (pInformation->USBbRequest == DFU_GETSTATUS) {
            /* todo, add routine to wait for last block write to finish */
            dfuAppStatus.bState  = dfuERROR;
        } else if (pInformation->USBbRequest == DFU_GETSTATE) {
            dfuAppStatus.bState  = dfuERROR;
        } else if (pInformation->USBbRequest == DFU_CLRSTATUS) {
            /* todo handle any cleanup we need here */
            dfuAppStatus.bState  = dfuIDLE;
            dfuAppStatus.bStatus = OK;
        } else {
            dfuAppStatus.bState  = dfuERROR;
            dfuAppStatus.bStatus = errSTALLEDPKT;
        }

    } else {
        /* some kind of error... */
        dfuAppStatus.bState  = dfuERROR;
        dfuAppStatus.bStatus = errSTALLEDPKT;
    }

//    /* Don't wait for a USB reset and just reset ourselves after the firmware is uploaded and manifested */
//    if (dfuAppStatus.bState == dfuMANIFEST_WAIT_RESET) {
//        systemHardReset();
//    }

    if (dfuAppStatus.bStatus == OK) {
        return TRUE;
    } else {
        return FALSE;
    }
}

void dfuUpdateByReset(void) {
    u8 startState = dfuAppStatus.bState;
    userFirmwareLen = 0;

    if (startState == appDETACH) {
        dfuAppStatus.bState = dfuIDLE;
        dfuAppStatus.bStatus = OK;

        nvicDisableInterrupts();
        usbEnbISR();

    } else if (startState == appIDLE || startState == dfuIDLE) {
        /* do nothing...might be normal usb bus activity */
    } else {
        /* we reset from the dfu, reset everything and startover,
           which is the correct operation if this is an erroneous
           event or properly following a MANIFEST */
        dfuAppStatus.bState = dfuIDLE;
        dfuAppStatus.bStatus = OK;

        systemHardReset();
    }
}

void dfuUpdateByTimeout(void) {
}

u8 *dfuCopyState(u16 length) {
    if (length == 0) {
        pInformation->Ctrl_Info.Usb_wLength = 1;
        return NULL;
    } else {
        return (&(dfuAppStatus.bState));
    }
}

u8 *dfuCopyStatus(u16 length) {
    if (length == 0) {
        pInformation->Ctrl_Info.Usb_wLength = 6;
        return NULL;
    } else {
        return (u8*)(&dfuAppStatus);
    }
}


u8 *dfuCopyDNLOAD(u16 length) {
    if (length == 0) {
        pInformation->Ctrl_Info.Usb_wLength = pInformation->USBwLengths.w - pInformation->Ctrl_Info.Usb_wOffset;
        thisBlockLen = pInformation->USBwLengths.w;
        return NULL;
    } else {
        return ((u8 *)recvBuffer + pInformation->Ctrl_Info.Usb_wOffset);
    }
}

u8 *dfuCopyUPLOAD(u16 length) {
    if (length == 0) {
        pInformation->Ctrl_Info.Usb_wLength = thisBlockLen - pInformation->Ctrl_Info.Usb_wOffset;
        return NULL;
    } else {
        return((u8*) userAppAddr + userFirmwareLen + pInformation->Ctrl_Info.Usb_wOffset);
    }
}

void dfuCopyBufferToExec(void) {
    int i;
    u32 userSpace;
    u32 start, end;

    userSpace = USER_CODE_FLASH0X8002000 + userFirmwareLen;

    /* Bail out if trying to flash out of bounds. */
    start = userSpace;
    end = start + thisBlockLen;
    if (end < start || start < userAppAddr || end > userAppEnd) {
        return;
    }

    flashErasePage(userSpace);

    for (i = 0; i < thisBlockLen; i = i + 4) {
        flashWriteWord(userSpace + i, *(u32 *)(recvBuffer + i));
    }

    userFirmwareLen += thisBlockLen;

    thisBlockLen = 0;
}

u8 dfuGetState(void) {
    return dfuAppStatus.bState;
}

void dfuSetState(u8 newState) {
    dfuAppStatus.bState = newState;
}

bool dfuUploadStarted(void) {
    return dfuBusy;
}

void dfuFinishUpload(void) {
    while (1)
    {
        __asm("nop");

        /* otherwise do nothing, dfu state machine resets itself */
    }
}

