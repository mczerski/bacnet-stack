/*####COPYRIGHTBEGIN####
 -------------------------------------------
 Copyright (C) 2003-2007 Steve Karg

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to:
 The Free Software Foundation, Inc.
 59 Temple Place - Suite 330
 Boston, MA  02111-1307
 USA.

 As a special exception, if other files instantiate templates or
 use macros or inline functions from this file, or you compile
 this file and link it with other works to produce a work based
 on this file, this file does not by itself cause the resulting
 work to be covered by the GNU General Public License. However
 the source code for this file must still be made available in
 accordance with section (3) of the GNU General Public License.

 This exception does not invalidate any other reasons why a work
 based on this file might be covered by the GNU General Public
 License.
 -------------------------------------------
####COPYRIGHTEND####*/

/** @file mstp.c  BACnet Master-Slave Twisted Pair (MS/TP) functions */

/* This clause describes a Master-Slave/Token-Passing (MS/TP) data link  */
/* protocol, which provides the same services to the network layer as  */
/* ISO 8802-2 Logical Link Control. It uses services provided by the  */
/* EIA-485 physical layer. Relevant clauses of EIA-485 are deemed to be  */
/* included in this standard by reference. The following hardware is assumed: */
/* (a)  A UART (Universal Asynchronous Receiver/Transmitter) capable of */
/*      transmitting and receiving eight data bits with one stop bit  */
/*      and no parity. */
/* (b)  An EIA-485 transceiver whose driver may be disabled.  */
/* (c)  A timer with a resolution of five milliseconds or less */

#include <stddef.h>
#include <stdint.h>
#if PRINT_ENABLED
#include <stdio.h>
#endif
#include "bacnet/datalink/cobs.h"
#include "bacnet/datalink/crc.h"
#include "bacnet/datalink/mstp.h"
#include "bacnet/datalink/mstptext.h"
#include "bacnet/npdu.h"

#ifndef DEBUG_ENABLED
#define DEBUG_ENABLED 0
#endif
#include "bacnet/basic/sys/debug.h"

#if PRINT_ENABLED
#undef PRINT_ENABLED_RECEIVE
#undef PRINT_ENABLED_RECEIVE_DATA
#undef PRINT_ENABLED_RECEIVE_ERRORS
#undef PRINT_ENABLED_MASTER
#endif

#if defined(PRINT_ENABLED_RECEIVE)
#define printf_receive debug_printf
#else
static inline void printf_receive(const char *format, ...)
{
    (void)format;
}
#endif

#if defined(PRINT_ENABLED_RECEIVE_DATA)
#define printf_receive_data debug_printf
#else
static inline void printf_receive_data(const char *format, ...)
{
    (void)format;
}
#endif

#if defined(PRINT_ENABLED_RECEIVE_ERRORS)
#define printf_receive_error debug_printf
#else
static inline void printf_receive_error(const char *format, ...)
{
    (void)format;
}
#endif

#if defined(PRINT_ENABLED_MASTER)
#define printf_master debug_printf
#else
static inline void printf_master(const char *format, ...)
{
    (void)format;
}
#endif

/* MS/TP Frame Format */
/* All frames are of the following format: */
/* */
/* Preamble: two octet preamble: X`55', X`FF' */
/* Frame Type: one octet */
/* Destination Address: one octet address */
/* Source Address: one octet address */
/* Length: two octets, most significant octet first, of the Data field */
/* Header CRC: one octet */
/* Data: (present only if Length is non-zero) */
/* Data CRC: (present only if Length is non-zero) two octets, */
/*           least significant octet first */
/* (pad): (optional) at most one octet of padding: X'FF' */

/* The minimum number of DataAvailable or ReceiveError events that must be */
/* seen by a receiving node in order to declare the line "active": 4. */
#define Nmin_octets 4

/* The minimum time without a DataAvailable or ReceiveError event within */
/* a frame before a receiving node may discard the frame: 60 bit times. */
/* (Implementations may use larger values for this timeout, */
/* not to exceed 100 milliseconds.) */
/* At 9600 baud, 60 bit times would be about 6.25 milliseconds */
/* const uint16_t Tframe_abort = 1 + ((1000 * 60) / 9600); */
#ifndef Tframe_abort
#define Tframe_abort 95
#endif

/* The maximum time a node may wait after reception of a frame that expects */
/* a reply before sending the first octet of a reply or Reply Postponed */
/* frame: 250 milliseconds. */
#ifndef Treply_delay
#define Treply_delay 250
#endif

/* Repeater turnoff delay. The duration of a continuous logical one state */
/* at the active input port of an MS/TP repeater after which the repeater */
/* will enter the IDLE state: 29 bit times < Troff < 40 bit times. */
#ifndef Troff
#define Troff 30
#endif

/* The minimum time without a DataAvailable or ReceiveError event */
/* that a node must wait for a station to begin replying to a */
/* confirmed request: 255 milliseconds. (Implementations may use */
/* larger values for this timeout, not to exceed 300 milliseconds.) */
#ifndef Treply_timeout
#define Treply_timeout 295
#endif

/* The time without a DataAvailable or ReceiveError event that a node must */
/* wait for a remote node to begin using a token or replying to a Poll For */
/* Master frame: 20 milliseconds. (Implementations may use larger values for */
/* this timeout, not to exceed 35 milliseconds.) */
#ifndef Tusage_timeout
#define Tusage_timeout 30
#endif

/* we need to be able to increment without rolling over */
#define INCREMENT_AND_LIMIT_UINT8(x) \
    {                                \
        if (x < 0xFF)                \
            x++;                     \
    }

bool MSTP_Line_Active(volatile struct mstp_port_struct_t *mstp_port)
{
    return (mstp_port->EventCount > Nmin_octets);
}

void MSTP_Fill_BACnet_Address(BACNET_ADDRESS *src, uint8_t mstp_address)
{
    int i = 0;

    if (mstp_address == MSTP_BROADCAST_ADDRESS) {
        /* mac_len = 0 if broadcast address */
        src->mac_len = 0;
        src->mac[0] = 0;
    } else {
        src->mac_len = 1;
        src->mac[0] = mstp_address;
    }
    /* fill with 0's starting with index 1; index 0 filled above */
    for (i = 1; i < MAX_MAC_LEN; i++) {
        src->mac[i] = 0;
    }
    src->net = 0;
    src->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        src->adr[i] = 0;
    }
}

/**
 * @brief Create an MS/TP Frame
 *
 * All MS/TP frames are of the following format:
 *   Preamble: two octet preamble: X`55', X`FF'
 *   Frame Type: one octet
 *   Destination Address: one octet address
 *   Source Address: one octet address
 *   Length: two octets, most significant octet first, of the Data field
 *   Header CRC: one octet
 *   Data: (present only if Length is non-zero)
 *   Data CRC: (present only if Length is non-zero) two octets,
 *     least significant octet first
 *   (pad): (optional) at most one octet of padding: X'FF'
 *
 * @param buffer - where frame is loaded
 * @param buffer_size - amount of space available in the buffer
 * @param frame_type - type of frame to send - see defines
 * @param destination - destination address
 * @param source - source address
 * @param data - any data to be sent - may be null
 * @param data_len - number of bytes of data (up to 501)
 * @return number of bytes encoded, or 0 on error
 */
uint16_t MSTP_Create_Frame(uint8_t *buffer,
    uint16_t buffer_size,
    uint8_t frame_type,
    uint8_t destination,
    uint8_t source,
    uint8_t *data,
    uint16_t data_len)
{
    uint8_t crc8 = 0xFF; /* used to calculate the crc value */
    uint16_t crc16 = 0xFFFF; /* used to calculate the crc value */
    uint16_t index = 0; /* used to load the data portion of the frame */
    uint16_t cobs_len; /* length of the COBS encoded frame */
    bool cobs_bacnet_frame = false; /* true for COBS BACnet frames */

    /* not enough to do a header */
    if (buffer_size < 8) {
        return 0;
    }
    buffer[0] = 0x55;
    buffer[1] = 0xFF;
    buffer[2] = frame_type;
    crc8 = CRC_Calc_Header(buffer[2], crc8);
    buffer[3] = destination;
    crc8 = CRC_Calc_Header(buffer[3], crc8);
    buffer[4] = source;
    crc8 = CRC_Calc_Header(buffer[4], crc8);
    buffer[5] = data_len >> 8; /* MSB first */
    crc8 = CRC_Calc_Header(buffer[5], crc8);
    buffer[6] = data_len & 0xFF;
    crc8 = CRC_Calc_Header(buffer[6], crc8);
    buffer[7] = ~crc8;
    index = 8;

    if ((data_len > 501) || ((frame_type >= Nmin_COBS_type) &&
        (frame_type <= Nmax_COBS_type))) {
        /* COBS encoded frame */
        if (frame_type == FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY) {
            frame_type = FRAME_TYPE_BACNET_EXTENDED_DATA_EXPECTING_REPLY;
            cobs_bacnet_frame = true;
        } else if (frame_type == FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY) {
            frame_type = FRAME_TYPE_BACNET_EXTENDED_DATA_NOT_EXPECTING_REPLY;
            cobs_bacnet_frame = true;
        } else if ((frame_type < Nmin_COBS_type) ||
            (frame_type > Nmax_COBS_type)) {
            /* I'm sorry, Dave, I'm afraid I can't do that. */
            return 0;
        }
        cobs_len = cobs_frame_encode(buffer, buffer_size, data, data_len);
        /* check the results of COBs encoding for validity */
        if (cobs_bacnet_frame) {
            if (cobs_len < Nmin_COBS_length_BACnet) {
                return 0;
            } else if (cobs_len > Nmax_COBS_length_BACnet) {
                return 0;
            }
        } else {
            if (cobs_len < Nmin_COBS_length) {
                return 0;
            } else if (cobs_len > Nmax_COBS_length) {
                return 0;
            }
        }
        /* for COBS, we must subtract two before use as the
           MS/TP frame length field since CRC32 is 2 bytes longer
           than CRC16 in original MSTP and non-COBS devices need
           to be able to ingest the entire frame */
        index = index + cobs_len - 2;
    } else if (data_len > 0) {
        while (data_len && data && (index < buffer_size)) {
            buffer[index] = *data;
            crc16 = CRC_Calc_Data(buffer[index], crc16);
            data++;
            index++;
            data_len--;
        }
        if ((index + 2) > buffer_size) {
            return 0;
        }
        crc16 = ~crc16;
        buffer[index] = crc16 & 0xFF; /* LSB first */
        index++;
        buffer[index] = crc16 >> 8;
        index++;
    }

    return index; /* returns the frame length */
}

/**
 * @brief Send an MS/TP Frame
 * @param mstp_port - port to send from
 * @param frame_type - type of frame to send - see defines
 * @param destination - destination address
 * @param source - source address
 * @param data - any data to be sent - may be null
 * @param data_len - number of bytes of data (up to 501)
 */
void MSTP_Create_And_Send_Frame(
    volatile struct mstp_port_struct_t *mstp_port,
    uint8_t frame_type,
    uint8_t destination,
    uint8_t source,
    uint8_t *data,
    uint16_t data_len)
{
    uint16_t len = 0; /* number of bytes to send */

    len = MSTP_Create_Frame((uint8_t *)&mstp_port->OutputBuffer[0],
        mstp_port->OutputBufferSize, frame_type, destination, source, data,
        data_len);

    MSTP_Send_Frame(mstp_port, (uint8_t *)&mstp_port->OutputBuffer[0], len);
    /* FIXME: be sure to reset SilenceTimer() after each octet is sent! */
}

void MSTP_Receive_Frame_FSM(volatile struct mstp_port_struct_t *mstp_port)
{
    MSTP_RECEIVE_STATE receive_state = mstp_port->receive_state;
    printf_receive(
        "MSTP Rx: State=%s Data=%02X hCRC=%02X Index=%u EC=%u DateLen=%u "
        "Silence=%u\n",
        mstptext_receive_state(mstp_port->receive_state),
        mstp_port->DataRegister, mstp_port->HeaderCRC, mstp_port->Index,
        mstp_port->EventCount, mstp_port->DataLength,
        mstp_port->SilenceTimer((void *)mstp_port));
    switch (mstp_port->receive_state) {
            /* In the IDLE state, the node waits for the beginning of a frame.
             */
        case MSTP_RECEIVE_STATE_IDLE:
            /* EatAnError */
            if (mstp_port->ReceiveError == true) {
                mstp_port->ReceiveError = false;
                mstp_port->SilenceTimerReset((void *)mstp_port);
                INCREMENT_AND_LIMIT_UINT8(mstp_port->EventCount);
                /* wait for the start of a frame. */
            } else if (mstp_port->DataAvailable == true) {
                printf_receive_data("MSTP Rx: %02X ", mstp_port->DataRegister);
                /* Preamble1 */
                if (mstp_port->DataRegister == 0x55) {
                    /* receive the remainder of the frame. */
                    mstp_port->receive_state = MSTP_RECEIVE_STATE_PREAMBLE;
                }
                /* EatAnOctet */
                else {
                    printf_receive_data("\n");
                    /* wait for the start of a frame. */
                }
                mstp_port->DataAvailable = false;
                mstp_port->SilenceTimerReset((void *)mstp_port);
                INCREMENT_AND_LIMIT_UINT8(mstp_port->EventCount);
            }
            break;
            /* In the PREAMBLE state, the node waits for the second octet of the
             * preamble. */
        case MSTP_RECEIVE_STATE_PREAMBLE:
            /* Timeout */
            if (mstp_port->SilenceTimer((void *)mstp_port) > Tframe_abort) {
                /* a correct preamble has not been received */
                /* wait for the start of a frame. */
                mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
            }
            /* Error */
            else if (mstp_port->ReceiveError == true) {
                mstp_port->ReceiveError = false;
                mstp_port->SilenceTimerReset((void *)mstp_port);
                INCREMENT_AND_LIMIT_UINT8(mstp_port->EventCount);
                /* wait for the start of a frame. */
                mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
            } else if (mstp_port->DataAvailable == true) {
                printf_receive_data("%02X ", mstp_port->DataRegister);
                /* Preamble2 */
                if (mstp_port->DataRegister == 0xFF) {
                    mstp_port->Index = 0;
                    mstp_port->HeaderCRC = 0xFF;
                    /* receive the remainder of the frame. */
                    mstp_port->receive_state = MSTP_RECEIVE_STATE_HEADER;
                }
                /* ignore RepeatedPreamble1 */
                else if (mstp_port->DataRegister == 0x55) {
                    /* wait for the second preamble octet. */
                }
                /* NotPreamble */
                else {
                    /* wait for the start of a frame. */
                    mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
                }
                mstp_port->DataAvailable = false;
                mstp_port->SilenceTimerReset((void *)mstp_port);
                INCREMENT_AND_LIMIT_UINT8(mstp_port->EventCount);
            }
            break;
            /* In the HEADER state, the node waits for the fixed message header.
             */
        case MSTP_RECEIVE_STATE_HEADER:
            /* Timeout */
            if (mstp_port->SilenceTimer((void *)mstp_port) > Tframe_abort) {
                /* indicate that an error has occurred during the reception of a
                 * frame */
                mstp_port->ReceivedInvalidFrame = true;
                /* wait for the start of a frame. */
                mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
                printf_receive_error("MSTP: Rx Header: SilenceTimer %u > %d\n",
                    (unsigned)mstp_port->SilenceTimer((void *)mstp_port),
                    Tframe_abort);
            }
            /* Error */
            else if (mstp_port->ReceiveError == true) {
                mstp_port->ReceiveError = false;
                mstp_port->SilenceTimerReset((void *)mstp_port);
                INCREMENT_AND_LIMIT_UINT8(mstp_port->EventCount);
                /* indicate that an error has occurred during the reception of a
                 * frame */
                mstp_port->ReceivedInvalidFrame = true;
                printf_receive_error("MSTP: Rx Header: ReceiveError\n");
                /* wait for the start of a frame. */
                mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
            } else if (mstp_port->DataAvailable == true) {
                printf_receive_data("%02X ", mstp_port->DataRegister);
                /* FrameType */
                if (mstp_port->Index == 0) {
                    mstp_port->HeaderCRC = CRC_Calc_Header(
                        mstp_port->DataRegister, mstp_port->HeaderCRC);
                    mstp_port->FrameType = mstp_port->DataRegister;
                    mstp_port->Index = 1;
                }
                /* Destination */
                else if (mstp_port->Index == 1) {
                    mstp_port->HeaderCRC = CRC_Calc_Header(
                        mstp_port->DataRegister, mstp_port->HeaderCRC);
                    mstp_port->DestinationAddress = mstp_port->DataRegister;
                    mstp_port->Index = 2;
                }
                /* Source */
                else if (mstp_port->Index == 2) {
                    mstp_port->HeaderCRC = CRC_Calc_Header(
                        mstp_port->DataRegister, mstp_port->HeaderCRC);
                    mstp_port->SourceAddress = mstp_port->DataRegister;
                    mstp_port->Index = 3;
                }
                /* Length1 */
                else if (mstp_port->Index == 3) {
                    mstp_port->HeaderCRC = CRC_Calc_Header(
                        mstp_port->DataRegister, mstp_port->HeaderCRC);
                    mstp_port->DataLength = mstp_port->DataRegister * 256;
                    mstp_port->Index = 4;
                }
                /* Length2 */
                else if (mstp_port->Index == 4) {
                    mstp_port->HeaderCRC = CRC_Calc_Header(
                        mstp_port->DataRegister, mstp_port->HeaderCRC);
                    mstp_port->DataLength += mstp_port->DataRegister;
                    mstp_port->Index = 5;
                }
                /* HeaderCRC */
                else if (mstp_port->Index == 5) {
                    mstp_port->HeaderCRC = CRC_Calc_Header(
                        mstp_port->DataRegister, mstp_port->HeaderCRC);
                    mstp_port->HeaderCRCActual = mstp_port->DataRegister;
                    /* don't wait for next state - do it here */
                    if (mstp_port->HeaderCRC != 0x55) {
                        /* BadCRC */
                        /* indicate that an error has occurred during
                           the reception of a frame */
                        mstp_port->ReceivedInvalidFrame = true;
                        printf_receive_error("MSTP: Rx Header: BadCRC [%02X]\n",
                            mstp_port->DataRegister);
                        /* wait for the start of the next frame. */
                        mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
                    } else {
                        if (mstp_port->DataLength == 0) {
                            /* NoData */
                            printf_receive_data("%s",
                                mstptext_frame_type(
                                    (unsigned)mstp_port->FrameType));
                            if ((mstp_port->DestinationAddress ==
                                    mstp_port->This_Station) ||
                                (mstp_port->DestinationAddress ==
                                    MSTP_BROADCAST_ADDRESS)) {
                                /* ForUs */
                                /* indicate that a frame with no data has been
                                 * received */
                                mstp_port->ReceivedValidFrame = true;
                            } else {
                                /* NotForUs */
                                mstp_port->ReceivedValidFrameNotForUs = true;
                            }
                            /* wait for the start of the next frame. */
                            mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
                        } else {
                            /* receive the data portion of the frame. */
                            if ((mstp_port->DestinationAddress ==
                                    mstp_port->This_Station) ||
                                (mstp_port->DestinationAddress ==
                                    MSTP_BROADCAST_ADDRESS)) {
                                if (mstp_port->DataLength <=
                                    mstp_port->InputBufferSize) {
                                    /* Data */
                                    mstp_port->receive_state =
                                        MSTP_RECEIVE_STATE_DATA;
                                } else {
                                    /* FrameTooLong */
                                    printf_receive_error(
                                        "MSTP: Rx Header: FrameTooLong %u\n",
                                        (unsigned)mstp_port->DataLength);
                                    mstp_port->receive_state =
                                        MSTP_RECEIVE_STATE_SKIP_DATA;
                                }
                            } else {
                                /* NotForUs */
                                mstp_port->receive_state =
                                    MSTP_RECEIVE_STATE_SKIP_DATA;
                            }
                            mstp_port->Index = 0;
                            mstp_port->DataCRC = 0xFFFF;
                        }
                    }
                }
                /* not per MS/TP standard, but it is a case not covered */
                else {
                    mstp_port->ReceiveError = false;
                    /* indicate that an error has occurred during  */
                    /* the reception of a frame */
                    mstp_port->ReceivedInvalidFrame = true;
                    printf_receive_error("MSTP: Rx Data: BadIndex %u\n",
                        (unsigned)mstp_port->Index);
                    /* wait for the start of a frame. */
                    mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
                }
                mstp_port->SilenceTimerReset((void *)mstp_port);
                INCREMENT_AND_LIMIT_UINT8(mstp_port->EventCount);
                mstp_port->DataAvailable = false;
            }
            break;
            /* In the DATA state, the node waits for the data portion of a
             * frame. */
        case MSTP_RECEIVE_STATE_DATA:
        case MSTP_RECEIVE_STATE_SKIP_DATA:
            /* Timeout */
            if (mstp_port->SilenceTimer((void *)mstp_port) > Tframe_abort) {
                /* indicate that an error has occurred during the reception of a
                 * frame */
                mstp_port->ReceivedInvalidFrame = true;
                printf_receive_error(
                    "MSTP: Rx Data: SilenceTimer %ums > %dms\n",
                    (unsigned)mstp_port->SilenceTimer((void *)mstp_port),
                    Tframe_abort);
                /* wait for the start of the next frame. */
                mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
            }
            /* Error */
            else if (mstp_port->ReceiveError == true) {
                mstp_port->ReceiveError = false;
                mstp_port->SilenceTimerReset((void *)mstp_port);
                /* indicate that an error has occurred during the reception of a
                 * frame */
                mstp_port->ReceivedInvalidFrame = true;
                printf_receive_error("MSTP: Rx Data: ReceiveError\n");
                /* wait for the start of the next frame. */
                mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
            } else if (mstp_port->DataAvailable == true) {
                printf_receive_data("%02X ", mstp_port->DataRegister);
                if (mstp_port->Index < mstp_port->DataLength) {
                    /* DataOctet */
                    mstp_port->DataCRC = CRC_Calc_Data(
                        mstp_port->DataRegister, mstp_port->DataCRC);
                    if (mstp_port->Index < mstp_port->InputBufferSize) {
                        mstp_port->InputBuffer[mstp_port->Index] =
                            mstp_port->DataRegister;
                    }
                    mstp_port->Index++;
                    /* SKIP_DATA or DATA - no change in state */
                } else if (mstp_port->Index == mstp_port->DataLength) {
                    /* CRC1 */
                    mstp_port->DataCRC = CRC_Calc_Data(
                        mstp_port->DataRegister, mstp_port->DataCRC);
                    mstp_port->DataCRCActualMSB = mstp_port->DataRegister;
                    if (mstp_port->Index < mstp_port->InputBufferSize) {
                        mstp_port->InputBuffer[mstp_port->Index] =
                            mstp_port->DataRegister;
                    }
                    mstp_port->Index++;
                    /* SKIP_DATA or DATA - no change in state */
                } else if (mstp_port->Index == (mstp_port->DataLength + 1)) {
                    /* CRC2 */
                    if (mstp_port->Index < mstp_port->InputBufferSize) {
                        mstp_port->InputBuffer[mstp_port->Index] =
                            mstp_port->DataRegister;
                    }
                    mstp_port->DataCRC = CRC_Calc_Data(
                        mstp_port->DataRegister, mstp_port->DataCRC);
                    mstp_port->DataCRCActualLSB = mstp_port->DataRegister;
                    printf_receive_data("%s",
                        mstptext_frame_type((unsigned)mstp_port->FrameType));
                    if (((mstp_port->Index+1) < mstp_port->InputBufferSize) &&
                        (mstp_port->FrameType >= Nmin_COBS_type) &&
                        (mstp_port->FrameType <= Nmax_COBS_type)) {
                        if (cobs_frame_decode(
                                &mstp_port->InputBuffer[mstp_port->Index + 1],
                                mstp_port->InputBufferSize,
                                mstp_port->InputBuffer, mstp_port->Index + 1)) {
                            if (mstp_port->receive_state ==
                                MSTP_RECEIVE_STATE_DATA) {
                                /* ForUs */
                                mstp_port->ReceivedValidFrame = true;
                            } else {
                                /* NotForUs */
                                mstp_port->ReceivedValidFrameNotForUs = true;
                            }
                        } else {
                            mstp_port->ReceivedInvalidFrame = true;
                        }
                    } else {
                        /* STATE DATA CRC - no need for new state */
                        /* indicate the complete reception of a valid frame */
                        if (mstp_port->DataCRC == 0xF0B8) {
                            if (mstp_port->receive_state ==
                                MSTP_RECEIVE_STATE_DATA) {
                                /* ForUs */
                                mstp_port->ReceivedValidFrame = true;
                            } else {
                                /* NotForUs */
                                mstp_port->ReceivedValidFrameNotForUs = true;
                            }
                        } else {
                            mstp_port->ReceivedInvalidFrame = true;
                            printf_receive_error(
                                "MSTP: Rx Data: BadCRC [%02X]\n",
                                mstp_port->DataRegister);
                        }
                    }
                    mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
                } else {
                    mstp_port->ReceivedInvalidFrame = true;
                    mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
                }
                mstp_port->DataAvailable = false;
                mstp_port->SilenceTimerReset((void *)mstp_port);
            }
            break;
        default:
            /* shouldn't get here - but if we do... */
            mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
            break;
    }
    if ((receive_state != MSTP_RECEIVE_STATE_IDLE) &&
        (mstp_port->receive_state == MSTP_RECEIVE_STATE_IDLE)) {
        printf_receive_data("\n");
        fflush(stderr);
    }
    return;
}

/* returns true if we need to transition immediately */
bool MSTP_Master_Node_FSM(volatile struct mstp_port_struct_t *mstp_port)
{
    unsigned length = 0;
    uint8_t next_poll_station = 0;
    uint8_t next_this_station = 0;
    uint8_t next_next_station = 0;
    uint16_t my_timeout = 10, ns_timeout = 0, mm_timeout = 0;
    /* transition immediately to the next state */
    bool transition_now = false;
    MSTP_MASTER_STATE master_state = mstp_port->master_state;

    /* some calculations that several states need */
    next_poll_station =
        (mstp_port->Poll_Station + 1) % (mstp_port->Nmax_master + 1);
    next_this_station =
        (mstp_port->This_Station + 1) % (mstp_port->Nmax_master + 1);
    next_next_station =
        (mstp_port->Next_Station + 1) % (mstp_port->Nmax_master + 1);
    switch (mstp_port->master_state) {
        case MSTP_MASTER_STATE_INITIALIZE:
            /* DoneInitializing */
            /* indicate that the next station is unknown */
            mstp_port->Next_Station = mstp_port->This_Station;
            mstp_port->Poll_Station = mstp_port->This_Station;
            /* cause a Poll For Master to be sent when this node first */
            /* receives the token */
            mstp_port->TokenCount = Npoll;
            mstp_port->SoleMaster = false;
            mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
            transition_now = true;
            break;
        case MSTP_MASTER_STATE_IDLE:
            /* In the IDLE state, the node waits for a frame. */
            if (mstp_port->ReceivedInvalidFrame == true) {
                /* ReceivedInvalidFrame */
                /* invalid frame was received */
                /* wait for the next frame - remain in IDLE */
                mstp_port->ReceivedInvalidFrame = false;
            } else if (mstp_port->ReceivedValidFrame == true) {
                printf_master("MSTP: ReceivedValidFrame "
                              "Src=%02X Dest=%02X DataLen=%u "
                              "FC=%u ST=%u Type=%s\n",
                    mstp_port->SourceAddress, mstp_port->DestinationAddress,
                    mstp_port->DataLength, mstp_port->FrameCount,
                    mstp_port->SilenceTimer((void *)mstp_port),
                    mstptext_frame_type((unsigned)mstp_port->FrameType));
                if ((mstp_port->DestinationAddress ==
                        mstp_port->This_Station) ||
                    (mstp_port->DestinationAddress == MSTP_BROADCAST_ADDRESS)) {
                    /* destined for me! */
                    switch (mstp_port->FrameType) {
                        case FRAME_TYPE_TOKEN:
                            /* ReceivedToken */
                            /* tokens can't be broadcast */
                            if (mstp_port->DestinationAddress ==
                                MSTP_BROADCAST_ADDRESS) {
                                break;
                            }
                            mstp_port->ReceivedValidFrame = false;
                            mstp_port->FrameCount = 0;
                            mstp_port->SoleMaster = false;
                            mstp_port->master_state =
                                MSTP_MASTER_STATE_USE_TOKEN;
                            transition_now = true;
                            break;
                        case FRAME_TYPE_POLL_FOR_MASTER:
                            /* ReceivedPFM */
                            /* DestinationAddress is equal to TS */
                            if (mstp_port->DestinationAddress ==
                                mstp_port->This_Station) {
                                MSTP_Create_And_Send_Frame(mstp_port,
                                    FRAME_TYPE_REPLY_TO_POLL_FOR_MASTER,
                                    mstp_port->SourceAddress,
                                    mstp_port->This_Station, NULL, 0);
                            }
                            break;
                        case FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY:
                            if ((mstp_port->DestinationAddress ==
                                    MSTP_BROADCAST_ADDRESS) &&
                                (npdu_confirmed_service(mstp_port->InputBuffer,
                                    mstp_port->DataLength))) {
                                /* BTL test: verifies that the IUT will quietly
                                   discard any Confirmed-Request-PDU, whose
                                   destination address is a multicast or
                                   broadcast address, received from the
                                   network layer. */
                            } else {
                                /* indicate successful reception to the higher
                                 * layers */
                                (void)MSTP_Put_Receive(mstp_port);
                            }
                            break;
                        case FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY:
                            if (mstp_port->DestinationAddress ==
                                MSTP_BROADCAST_ADDRESS) {
                                /* broadcast DER just remains IDLE */
                            } else {
                                /* indicate successful reception to the higher
                                 * layers  */
                                (void)MSTP_Put_Receive(mstp_port);
                                mstp_port->master_state =
                                    MSTP_MASTER_STATE_ANSWER_DATA_REQUEST;
                            }
                            break;
                        case FRAME_TYPE_TEST_REQUEST:
                            MSTP_Create_And_Send_Frame(mstp_port,
                                FRAME_TYPE_TEST_RESPONSE,
                                mstp_port->SourceAddress,
                                mstp_port->This_Station, mstp_port->InputBuffer,
                                mstp_port->DataLength);
                            break;
                        case FRAME_TYPE_TEST_RESPONSE:
                        default:
                            break;
                    }
                }
                /* For DATA_EXPECTING_REPLY, we will keep the Rx Frame for
                   reference, and the flag will be cleared in the next state */
                if (mstp_port->master_state !=
                    MSTP_MASTER_STATE_ANSWER_DATA_REQUEST) {
                    mstp_port->ReceivedValidFrame = false;
                }
            } else if (mstp_port->SilenceTimer((void *)mstp_port) >=
                Tno_token) {
                /* LostToken */
                /* assume that the token has been lost */
                mstp_port->EventCount = 0; /* Addendum 135-2004d-8 */
                mstp_port->master_state = MSTP_MASTER_STATE_NO_TOKEN;
                /* set the receive frame flags to false in case we received
                   some bytes and had a timeout for some reason */
                mstp_port->ReceivedInvalidFrame = false;
                mstp_port->ReceivedValidFrame = false;
                transition_now = true;
            }
            break;
        case MSTP_MASTER_STATE_USE_TOKEN:
            /* In the USE_TOKEN state, the node is allowed to send one or  */
            /* more data frames. These may be BACnet Data frames or */
            /* proprietary frames. */
            /* FIXME: We could wait for up to Tusage_delay */
            length = (unsigned)MSTP_Get_Send(mstp_port, 0);
            if (length < 1) {
                /* NothingToSend */
                mstp_port->FrameCount = mstp_port->Nmax_info_frames;
                mstp_port->master_state = MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                transition_now = true;
            } else {
                uint8_t frame_type = mstp_port->OutputBuffer[2];
                uint8_t destination = mstp_port->OutputBuffer[3];
                MSTP_Send_Frame(mstp_port,
                    (uint8_t *)&mstp_port->OutputBuffer[0], (uint16_t)length);
                mstp_port->FrameCount++;
                switch (frame_type) {
                    case FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY:
                        if (destination == MSTP_BROADCAST_ADDRESS) {
                            /* SendNoWait */
                            mstp_port->master_state =
                                MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                        } else {
                            /* SendAndWait */
                            mstp_port->master_state =
                                MSTP_MASTER_STATE_WAIT_FOR_REPLY;
                        }
                        break;
                    case FRAME_TYPE_TEST_REQUEST:
                        /* SendAndWait */
                        mstp_port->master_state =
                            MSTP_MASTER_STATE_WAIT_FOR_REPLY;
                        break;
                    case FRAME_TYPE_TEST_RESPONSE:
                    case FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY:
                    default:
                        /* SendNoWait */
                        mstp_port->master_state =
                            MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                        break;
                }
            }
            break;
        case MSTP_MASTER_STATE_WAIT_FOR_REPLY:
            /* In the WAIT_FOR_REPLY state, the node waits for  */
            /* a reply from another node. */
            if (mstp_port->SilenceTimer((void *)mstp_port) >= Treply_timeout) {
                /* ReplyTimeout */
                /* assume that the request has failed */
                mstp_port->FrameCount = mstp_port->Nmax_info_frames;
                mstp_port->master_state = MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                /* Any retry of the data frame shall await the next entry */
                /* to the USE_TOKEN state. (Because of the length of the
                 * timeout,  */
                /* this transition will cause the token to be passed regardless
                 */
                /* of the initial value of FrameCount.) */
                transition_now = true;
            } else {
                if (mstp_port->ReceivedInvalidFrame == true) {
                    /* InvalidFrame */
                    /* error in frame reception */
                    mstp_port->ReceivedInvalidFrame = false;
                    mstp_port->master_state = MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                    transition_now = true;
                } else if (mstp_port->ReceivedValidFrame == true) {
                    if (mstp_port->DestinationAddress ==
                        mstp_port->This_Station) {
                        switch (mstp_port->FrameType) {
                            case FRAME_TYPE_REPLY_POSTPONED:
                                /* ReceivedReplyPostponed */
                                mstp_port->master_state =
                                    MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                                break;
                            case FRAME_TYPE_TEST_RESPONSE:
                                mstp_port->master_state =
                                    MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                                break;
                            case FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY:
                                /* ReceivedReply */
                                /* or a proprietary type that indicates a reply
                                 */
                                /* indicate successful reception to the higher
                                 * layers */
                                (void)MSTP_Put_Receive(mstp_port);
                                mstp_port->master_state =
                                    MSTP_MASTER_STATE_DONE_WITH_TOKEN;
                                break;
                            default:
                                /* if proprietary frame was expected, you might
                                   need to transition to DONE WITH TOKEN */
                                mstp_port->master_state =
                                    MSTP_MASTER_STATE_IDLE;
                                break;
                        }
                    } else {
                        /* ReceivedUnexpectedFrame */
                        /* an unexpected frame was received */
                        /* This may indicate the presence of multiple tokens. */
                        /* Synchronize with the network. */
                        /* This action drops the token. */
                        mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
                    }
                    mstp_port->ReceivedValidFrame = false;
                    transition_now = true;
                }
            }
            break;
        case MSTP_MASTER_STATE_DONE_WITH_TOKEN:
            /* The DONE_WITH_TOKEN state either sends another data frame,  */
            /* passes the token, or initiates a Poll For Master cycle. */
            /* SendAnotherFrame */
            if (mstp_port->FrameCount < mstp_port->Nmax_info_frames) {
                /* then this node may send another information frame  */
                /* before passing the token.  */
                mstp_port->master_state = MSTP_MASTER_STATE_USE_TOKEN;
                transition_now = true;
            } else if ((mstp_port->SoleMaster == false) &&
                (mstp_port->Next_Station == mstp_port->This_Station)) {
                /* NextStationUnknown - added in Addendum 135-2008v-1 */
                /*  then the next station to which the token
                   should be sent is unknown - so PollForMaster */
                mstp_port->Poll_Station = next_this_station;
                MSTP_Create_And_Send_Frame(mstp_port,
                    FRAME_TYPE_POLL_FOR_MASTER, mstp_port->Poll_Station,
                    mstp_port->This_Station, NULL, 0);
                mstp_port->RetryCount = 0;
                mstp_port->master_state = MSTP_MASTER_STATE_POLL_FOR_MASTER;
            } else if (mstp_port->TokenCount < (Npoll - 1)) {
                /* Npoll changed in Errata SSPC-135-2004 */
                if ((mstp_port->SoleMaster == true) &&
                    (mstp_port->Next_Station != next_this_station)) {
                    /* SoleMaster */
                    /* there are no other known master nodes to */
                    /* which the token may be sent (true master-slave
                     * operation).  */
                    mstp_port->FrameCount = 0;
                    mstp_port->TokenCount++;
                    mstp_port->master_state = MSTP_MASTER_STATE_USE_TOKEN;
                    transition_now = true;
                } else {
                    /* SendToken */
                    /* Npoll changed in Errata SSPC-135-2004 */
                    /* The comparison of NS and TS+1 eliminates the Poll For
                     * Master  */
                    /* if there are no addresses between TS and NS, since there
                     * is no  */
                    /* address at which a new master node may be found in that
                     * case. */
                    mstp_port->TokenCount++;
                    /* transmit a Token frame to NS */
                    MSTP_Create_And_Send_Frame(mstp_port, FRAME_TYPE_TOKEN,
                        mstp_port->Next_Station, mstp_port->This_Station, NULL,
                        0);
                    mstp_port->RetryCount = 0;
                    mstp_port->EventCount = 0;
                    mstp_port->master_state = MSTP_MASTER_STATE_PASS_TOKEN;
                }
            } else if (next_poll_station == mstp_port->Next_Station) {
                if (mstp_port->SoleMaster == true) {
                    /* SoleMasterRestartMaintenancePFM */
                    mstp_port->Poll_Station = next_next_station;
                    MSTP_Create_And_Send_Frame(mstp_port,
                        FRAME_TYPE_POLL_FOR_MASTER, mstp_port->Poll_Station,
                        mstp_port->This_Station, NULL, 0);
                    /* no known successor node */
                    mstp_port->Next_Station = mstp_port->This_Station;
                    mstp_port->RetryCount = 0;
                    /* changed in Errata SSPC-135-2004 */
                    mstp_port->TokenCount = 1;
                    /* mstp_port->EventCount = 0; removed in Addendum
                     * 135-2004d-8 */
                    /* find a new successor to TS */
                    mstp_port->master_state = MSTP_MASTER_STATE_POLL_FOR_MASTER;
                } else {
                    /* ResetMaintenancePFM */
                    mstp_port->Poll_Station = mstp_port->This_Station;
                    /* transmit a Token frame to NS */
                    MSTP_Create_And_Send_Frame(mstp_port, FRAME_TYPE_TOKEN,
                        mstp_port->Next_Station, mstp_port->This_Station, NULL,
                        0);
                    mstp_port->RetryCount = 0;
                    /* changed in Errata SSPC-135-2004 */
                    mstp_port->TokenCount = 1;
                    mstp_port->EventCount = 0;
                    mstp_port->master_state = MSTP_MASTER_STATE_PASS_TOKEN;
                }
            } else {
                /* SendMaintenancePFM */
                mstp_port->Poll_Station = next_poll_station;
                MSTP_Create_And_Send_Frame(mstp_port,
                    FRAME_TYPE_POLL_FOR_MASTER, mstp_port->Poll_Station,
                    mstp_port->This_Station, NULL, 0);
                mstp_port->RetryCount = 0;
                mstp_port->master_state = MSTP_MASTER_STATE_POLL_FOR_MASTER;
            }
            break;
        case MSTP_MASTER_STATE_PASS_TOKEN:
            /* The PASS_TOKEN state listens for a successor to begin using */
            /* the token that this node has just attempted to pass. */
            if (mstp_port->SilenceTimer((void *)mstp_port) <= Tusage_timeout) {
                if (mstp_port->EventCount > Nmin_octets) {
                    /* SawTokenUser */
                    /* Assume that a frame has been sent by the new token user.
                     */
                    /* Enter the IDLE state to process the frame. */
                    mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
                    transition_now = true;
                }
            } else {
                if (mstp_port->RetryCount < Nretry_token) {
                    /* RetrySendToken */
                    mstp_port->RetryCount++;
                    /* Transmit a Token frame to NS */
                    MSTP_Create_And_Send_Frame(mstp_port, FRAME_TYPE_TOKEN,
                        mstp_port->Next_Station, mstp_port->This_Station, NULL,
                        0);
                    mstp_port->EventCount = 0;
                    /* re-enter the current state to listen for NS  */
                    /* to begin using the token. */
                } else {
                    /* FindNewSuccessor */
                    /* Assume that NS has failed.  */
                    /* note: if NS=TS-1, this node could send PFM to self! */
                    mstp_port->Poll_Station = next_next_station;
                    /* Transmit a Poll For Master frame to PS. */
                    MSTP_Create_And_Send_Frame(mstp_port,
                        FRAME_TYPE_POLL_FOR_MASTER, mstp_port->Poll_Station,
                        mstp_port->This_Station, NULL, 0);
                    /* no known successor node */
                    mstp_port->Next_Station = mstp_port->This_Station;
                    mstp_port->RetryCount = 0;
                    mstp_port->TokenCount = 0;
                    /* mstp_port->EventCount = 0; removed in Addendum
                     * 135-2004d-8 */
                    /* find a new successor to TS */
                    mstp_port->master_state = MSTP_MASTER_STATE_POLL_FOR_MASTER;
                }
            }
            break;
        case MSTP_MASTER_STATE_NO_TOKEN:
            /* The NO_TOKEN state is entered if mstp_port->SilenceTimer()
             * becomes greater  */
            /* than Tno_token, indicating that there has been no network
             * activity */
            /* for that period of time. The timeout is continued to determine */
            /* whether or not this node may create a token. */
            my_timeout = Tno_token + (Tslot * mstp_port->This_Station);
            if (mstp_port->SilenceTimer((void *)mstp_port) < my_timeout) {
                if (mstp_port->EventCount > Nmin_octets) {
                    /* SawFrame */
                    /* Some other node exists at a lower address.  */
                    /* Enter the IDLE state to receive and process the incoming
                     * frame. */
                    mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
                    transition_now = true;
                }
            } else {
                ns_timeout =
                    Tno_token + (Tslot * (mstp_port->This_Station + 1));
                mm_timeout = Tno_token + (Tslot * (mstp_port->Nmax_master + 1));
                if ((mstp_port->SilenceTimer((void *)mstp_port) < ns_timeout) ||
                    (mstp_port->SilenceTimer((void *)mstp_port) > mm_timeout)) {
                    /* GenerateToken */
                    /* Assume that this node is the lowest numerical address  */
                    /* on the network and is empowered to create a token.  */
                    mstp_port->Poll_Station = next_this_station;
                    /* Transmit a Poll For Master frame to PS. */
                    MSTP_Create_And_Send_Frame(mstp_port,
                        FRAME_TYPE_POLL_FOR_MASTER, mstp_port->Poll_Station,
                        mstp_port->This_Station, NULL, 0);
                    /* indicate that the next station is unknown */
                    mstp_port->Next_Station = mstp_port->This_Station;
                    mstp_port->RetryCount = 0;
                    mstp_port->TokenCount = 0;
                    /* mstp_port->EventCount = 0;
                       removed Addendum 135-2004d-8 */
                    /* enter the POLL_FOR_MASTER state
                       to find a new successor to TS. */
                    mstp_port->master_state = MSTP_MASTER_STATE_POLL_FOR_MASTER;
                } else {
                    /* We missed our time slot!
                       We should never get here unless
                       OS timer resolution is poor or we were busy */
                    if (mstp_port->EventCount > Nmin_octets) {
                        /* SawFrame */
                        /* Some other node exists at a lower address.  */
                        /* Enter the IDLE state to receive and
                           process the incoming frame. */
                        mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
                        transition_now = true;
                    }
                }
            }
            break;
        case MSTP_MASTER_STATE_POLL_FOR_MASTER:
            /* In the POLL_FOR_MASTER state, the node listens for a reply to */
            /* a previously sent Poll For Master frame in order to find  */
            /* a successor node. */
            if (mstp_port->ReceivedValidFrame == true) {
                if ((mstp_port->DestinationAddress ==
                        mstp_port->This_Station) &&
                    (mstp_port->FrameType ==
                        FRAME_TYPE_REPLY_TO_POLL_FOR_MASTER)) {
                    /* ReceivedReplyToPFM */
                    mstp_port->SoleMaster = false;
                    mstp_port->Next_Station = mstp_port->SourceAddress;
                    mstp_port->EventCount = 0;
                    /* Transmit a Token frame to NS */
                    MSTP_Create_And_Send_Frame(mstp_port, FRAME_TYPE_TOKEN,
                        mstp_port->Next_Station, mstp_port->This_Station, NULL,
                        0);
                    mstp_port->Poll_Station = mstp_port->This_Station;
                    mstp_port->TokenCount = 0;
                    mstp_port->RetryCount = 0;
                    mstp_port->master_state = MSTP_MASTER_STATE_PASS_TOKEN;
                } else {
                    /* ReceivedUnexpectedFrame */
                    /* An unexpected frame was received.  */
                    /* This may indicate the presence of multiple tokens. */
                    /* enter the IDLE state to synchronize with the network.  */
                    /* This action drops the token. */
                    mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
                    transition_now = true;
                }
                mstp_port->ReceivedValidFrame = false;
            } else if ((mstp_port->SilenceTimer((void *)mstp_port) >
                           Tusage_timeout) ||
                (mstp_port->ReceivedInvalidFrame == true)) {
                if (mstp_port->SoleMaster == true) {
                    /* SoleMaster */
                    /* There was no valid reply to the periodic poll  */
                    /* by the sole known master for other masters. */
                    mstp_port->FrameCount = 0;
                    /* mstp_port->TokenCount++; removed in 2004 */
                    mstp_port->master_state = MSTP_MASTER_STATE_USE_TOKEN;
                    transition_now = true;
                } else {
                    if (mstp_port->Next_Station != mstp_port->This_Station) {
                        /* DoneWithPFM */
                        /* There was no valid reply to the maintenance  */
                        /* poll for a master at address PS.  */
                        mstp_port->EventCount = 0;
                        /* transmit a Token frame to NS */
                        MSTP_Create_And_Send_Frame(mstp_port, FRAME_TYPE_TOKEN,
                            mstp_port->Next_Station, mstp_port->This_Station,
                            NULL, 0);
                        mstp_port->RetryCount = 0;
                        mstp_port->master_state = MSTP_MASTER_STATE_PASS_TOKEN;
                    } else {
                        if (next_poll_station != mstp_port->This_Station) {
                            /* SendNextPFM */
                            mstp_port->Poll_Station = next_poll_station;
                            /* Transmit a Poll For Master frame to PS. */
                            MSTP_Create_And_Send_Frame(mstp_port,
                                FRAME_TYPE_POLL_FOR_MASTER,
                                mstp_port->Poll_Station,
                                mstp_port->This_Station, NULL, 0);
                            mstp_port->RetryCount = 0;
                            /* Re-enter the current state. */
                        } else {
                            /* DeclareSoleMaster */
                            /* to indicate that this station is the only master
                             */
                            mstp_port->SoleMaster = true;
                            mstp_port->FrameCount = 0;
                            mstp_port->master_state =
                                MSTP_MASTER_STATE_USE_TOKEN;
                            transition_now = true;
                        }
                    }
                }
                mstp_port->ReceivedInvalidFrame = false;
            }
            break;
        case MSTP_MASTER_STATE_ANSWER_DATA_REQUEST:
            /* The ANSWER_DATA_REQUEST state is entered when a  */
            /* BACnet Data Expecting Reply, a Test_Request, or  */
            /* a proprietary frame that expects a reply is received. */
            /* FIXME: MSTP_Get_Reply waits for a matching reply, but
               if the next queued message doesn't match, then we
               sit here for Treply_delay doing nothing */
            length = (unsigned)MSTP_Get_Reply(mstp_port, 0);
            if (length > 0) {
                /* Reply */
                /* If a reply is available from the higher layers  */
                /* within Treply_delay after the reception of the  */
                /* final octet of the requesting frame  */
                /* (the mechanism used to determine this is a local matter), */
                /* then call MSTP_Create_And_Send_Frame to transmit the reply
                 * frame  */
                /* and enter the IDLE state to wait for the next frame. */
                MSTP_Send_Frame(mstp_port,
                    (uint8_t *)&mstp_port->OutputBuffer[0], (uint16_t)length);
                mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
                /* clear our flag we were holding for comparison */
                mstp_port->ReceivedValidFrame = false;
            } else if (mstp_port->SilenceTimer((void *)mstp_port) >
                Treply_delay) {
                /* DeferredReply */
                /* If no reply will be available from the higher layers */
                /* within Treply_delay after the reception of the */
                /* final octet of the requesting frame (the mechanism */
                /* used to determine this is a local matter), */
                /* then an immediate reply is not possible. */
                /* Any reply shall wait until this node receives the token. */
                /* Call MSTP_Create_And_Send_Frame to transmit a Reply Postponed
                 * frame, */
                /* and enter the IDLE state. */
                MSTP_Create_And_Send_Frame(mstp_port,
                    FRAME_TYPE_REPLY_POSTPONED, mstp_port->SourceAddress,
                    mstp_port->This_Station, NULL, 0);
                mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
                /* clear our flag we were holding for comparison */
                mstp_port->ReceivedValidFrame = false;
            }
            break;
        default:
            mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
            break;
    }
    if (mstp_port->master_state != master_state) {
        /* change of state detected - so print the details for debugging */
        printf_master(
            "MSTP: TS=%02X[%02X] NS=%02X[%02X] PS=%02X[%02X] EC=%u TC=%u ST=%u "
            "%s\n",
            mstp_port->This_Station, next_this_station, mstp_port->Next_Station,
            next_next_station, mstp_port->Poll_Station, next_poll_station,
            mstp_port->EventCount, mstp_port->TokenCount,
            mstp_port->SilenceTimer((void *)mstp_port),
            mstptext_master_state(mstp_port->master_state));
    }

    return transition_now;
}

void MSTP_Slave_Node_FSM(volatile struct mstp_port_struct_t *mstp_port)
{
    unsigned length = 0;

    mstp_port->master_state = MSTP_MASTER_STATE_IDLE;
    if (mstp_port->ReceivedInvalidFrame == true) {
        /* ReceivedInvalidFrame */
        /* invalid frame was received */
        mstp_port->ReceivedInvalidFrame = false;
    } else if (mstp_port->ReceivedValidFrame) {
        switch (mstp_port->FrameType) {
            case FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY:
                if (mstp_port->DestinationAddress != MSTP_BROADCAST_ADDRESS) {
                    /* The ANSWER_DATA_REQUEST state is entered when a  */
                    /* BACnet Data Expecting Reply, a Test_Request, or  */
                    /* a proprietary frame that expects a reply is received. */
                    length = (unsigned)MSTP_Get_Reply(mstp_port, 0);
                    if (length > 0) {
                        /* Reply */
                        /* If a reply is available from the higher layers  */
                        /* within Treply_delay after the reception of the  */
                        /* final octet of the requesting frame  */
                        /* (the mechanism used to determine this is a local
                         * matter), */
                        /* then call MSTP_Create_And_Send_Frame to transmit the
                         * reply frame  */
                        /* and enter the IDLE state to wait for the next frame.
                         */
                        MSTP_Send_Frame(mstp_port,
                            (uint8_t *)&mstp_port->OutputBuffer[0],
                            (uint16_t)length);
                        /* clear our flag we were holding for comparison */
                        mstp_port->ReceivedValidFrame = false;
                    } else if (mstp_port->SilenceTimer((void *)mstp_port) >
                        Treply_delay) {
                        /* If no reply will be available from the higher layers
                           within Treply_delay after the reception of the final
                           octet of the requesting frame (the mechanism used to
                           determine this is a local matter), then no reply is
                           possible. */
                        /* clear our flag we were holding for comparison */
                        mstp_port->ReceivedValidFrame = false;
                    }
                } else {
                    mstp_port->ReceivedValidFrame = false;
                }
                break;
            case FRAME_TYPE_TEST_REQUEST:
                mstp_port->ReceivedValidFrame = false;
                MSTP_Create_And_Send_Frame(mstp_port, FRAME_TYPE_TEST_RESPONSE,
                    mstp_port->SourceAddress, mstp_port->This_Station,
                    &mstp_port->InputBuffer[0], mstp_port->DataLength);
                break;
            case FRAME_TYPE_TOKEN:
            case FRAME_TYPE_POLL_FOR_MASTER:
            case FRAME_TYPE_TEST_RESPONSE:
            case FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY:
            default:
                mstp_port->ReceivedValidFrame = false;
                break;
        }
    }
}

/* note: This_Station assumed to be set with the MAC address */
/* note: Nmax_info_frames assumed to be set (default=1) */
/* note: Nmax_master assumed to be set (default=127) */
/* note: InputBuffer and InputBufferSize assumed to be set */
/* note: OutputBuffer and OutputBufferSize assumed to be set */
/* note: SilenceTimer and SilenceTimerReset assumed to be set */
void MSTP_Init(volatile struct mstp_port_struct_t *mstp_port)
{
    if (mstp_port) {
#if 0
        /* FIXME: you must point these buffers to actual byte buckets
           in the dlmstp function before calling this init. */
        mstp_port->InputBuffer = &InputBuffer[0];
        mstp_port->InputBufferSize = sizeof(InputBuffer);
        mstp_port->OutputBuffer = &OutputBuffer[0];
        mstp_port->OutputBufferSize = sizeof(OutputBuffer);
        /* FIXME: these are adjustable, so you must set these in dlmstp */
        mstp_port->Nmax_info_frames = DEFAULT_MAX_INFO_FRAMES;
        mstp_port->Nmax_master = DEFAULT_MAX_MASTER;
        /* FIXME: point to functions */
        mstp_port->SilenceTimer = Timer_Silence;
        mstp_port = >SilenceTimerReset = Timer_Silence_Reset;
#endif
        mstp_port->receive_state = MSTP_RECEIVE_STATE_IDLE;
        mstp_port->master_state = MSTP_MASTER_STATE_INITIALIZE;
        mstp_port->ReceiveError = false;
        mstp_port->DataAvailable = false;
        mstp_port->DataRegister = 0;
        mstp_port->DataCRC = 0;
        mstp_port->DataLength = 0;
        mstp_port->DestinationAddress = 0;
        mstp_port->EventCount = 0;
        mstp_port->FrameType = FRAME_TYPE_TOKEN;
        mstp_port->FrameCount = 0;
        mstp_port->HeaderCRC = 0;
        mstp_port->Index = 0;
        mstp_port->Next_Station = mstp_port->This_Station;
        mstp_port->Poll_Station = mstp_port->This_Station;
        mstp_port->ReceivedInvalidFrame = false;
        mstp_port->ReceivedValidFrame = false;
        mstp_port->ReceivedValidFrameNotForUs = false;
        mstp_port->RetryCount = 0;
        mstp_port->SilenceTimerReset((void *)mstp_port);
        mstp_port->SoleMaster = false;
        mstp_port->SourceAddress = 0;
        mstp_port->TokenCount = 0;
    }
}
