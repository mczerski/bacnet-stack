#include <stdint.h>
#include <stdbool.h>
/* hardware layer includes */
#include "bacnet/datalink/dlmstp.h"
#include "bacnet/basic/sys/mstimer.h"
/* BACnet Stack includes */
#include "bacnet/datalink/datalink.h"
#include "bacnet/npdu.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/service/h_arf.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/dcc.h"
#include "bacnet/iam.h"
/* BACnet objects */
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/bo.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/bacfile.h"
/* me */
#include "bacnet.h"


/* timer for device communications control */
static struct mstimer BACnet_Task_Timer;
static struct mstimer BACnet_TSM_Timer;
#define MAX_BINARY_OUTPUTS 2

static object_functions_t My_Object_Table[] = {
    { OBJECT_DEVICE, NULL /* Init - don't init Device or it will recourse! */,
        Device_Count, Device_Index_To_Instance,
        Device_Valid_Object_Instance_Number, Device_Object_Name,
        Device_Read_Property_Local, Device_Write_Property_Local,
        Device_Property_Lists, DeviceGetRRInfo, NULL /* Iterator */,
        NULL /* Value_Lists */, NULL /* COV */, NULL /* COV Clear */,
        NULL /* Intrinsic Reporting */, NULL /* Add_List_Element */,
        NULL /* Remove_List_Element */, NULL /* Create */, NULL /* Delete */,
        NULL /* Timer */ },
    { OBJECT_BINARY_INPUT, Binary_Input_Init, Binary_Input_Count,
        Binary_Input_Index_To_Instance, Binary_Input_Valid_Instance,
        Binary_Input_Object_Name, Binary_Input_Read_Property,
        Binary_Input_Write_Property, Binary_Input_Property_Lists,
        NULL /* ReadRangeInfo */, NULL /* Iterator */,
        Binary_Input_Encode_Value_List, Binary_Input_Change_Of_Value,
        Binary_Input_Change_Of_Value_Clear, NULL /* Intrinsic Reporting */, NULL /* Add_List_Element */,
        NULL /* Remove_List_Element */, NULL /* Create */, NULL /* Delete */,
        NULL /* Timer */ },
    { OBJECT_BINARY_OUTPUT, Binary_Output_Init, Binary_Output_Count,
        Binary_Output_Index_To_Instance, Binary_Output_Valid_Instance,
        Binary_Output_Object_Name, Binary_Output_Read_Property,
        Binary_Output_Write_Property, Binary_Output_Property_Lists,
        NULL /* ReadRangeInfo */, NULL /* Iterator */,
        Binary_Output_Encode_Value_List, Binary_Output_Change_Of_Value,
        Binary_Output_Change_Of_Value_Clear, NULL /* Intrinsic Reporting */, NULL /* Add_List_Element */,
        NULL /* Remove_List_Element */, NULL /* Create */, NULL /* Delete */,
        NULL /* Timer */ },
    { OBJECT_ANALOG_INPUT, Analog_Input_Init, Analog_Input_Count,
        Analog_Input_Index_To_Instance, Analog_Input_Valid_Instance,
        Analog_Input_Object_Name, Analog_Input_Read_Property,
        Analog_Input_Write_Property, Analog_Input_Property_Lists,
        NULL /* ReadRangeInfo */, NULL /* Iterator */,
        Analog_Input_Encode_Value_List, Analog_Input_Change_Of_Value,
        Analog_Input_Change_Of_Value_Clear, Analog_Input_Intrinsic_Reporting, NULL /* Add_List_Element */,
        NULL /* Remove_List_Element */, NULL /* Create */, NULL /* Delete */,
        NULL /* Timer */ },
    { OBJECT_FILE, bacfile_init, (unsigned (*)())bacfile_count, bacfile_index_to_instance,
        bacfile_valid_instance, bacfile_object_name, bacfile_read_property,
        bacfile_write_property, BACfile_Property_Lists,
        NULL /* ReadRangeInfo */, NULL /* Iterator */, NULL /* Value_Lists */,
        NULL /* COV */, NULL /* COV Clear */, NULL /* Intrinsic Reporting */, NULL /* Add_List_Element */,
        NULL /* Remove_List_Element */, NULL /* Create */, NULL /* Delete */,
        NULL /* Timer */ },
    { MAX_BACNET_OBJECT_TYPE, NULL /* Init */, NULL /* Count */,
        NULL /* Index_To_Instance */, NULL /* Valid_Instance */,
        NULL /* Object_Name */, NULL /* Read_Property */,
        NULL /* Write_Property */, NULL /* Property_Lists */,
        NULL /* ReadRangeInfo */, NULL /* Iterator */, NULL /* Value_Lists */,
        NULL /* COV */, NULL /* COV Clear */,
        NULL /* Intrinsic Reporting */, NULL /* Add_List_Element */,
        NULL /* Remove_List_Element */, NULL /* Create */, NULL /* Delete */,
        NULL /* Timer */ }
};

void binary_output_callback(
    uint32_t object_instance, BACNET_BINARY_PV old_value,
    BACNET_BINARY_PV value)
{
}

void bacnet_init(const char* ifname)
{
    int i = 0;
    dlmstp_set_mac_address(1);
    dlmstp_set_max_master(3);
    dlmstp_set_max_info_frames(1);
    /* initialize datalink layer */
    dlmstp_init(ifname);
    handler_cov_init();
    /* initialize objects */
    Device_Init(My_Object_Table);
    for (i = 0; i < MAX_BINARY_OUTPUTS; i++) {
        Binary_Output_Create(i);
    }
    Binary_Output_Write_Present_Value_Callback_Set(binary_output_callback);

    /* set up our confirmed service unrecognized service handler - required! */
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    /* we need to handle who-is to support dynamic device binding */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    /* Set the handlers for any confirmed services that we support. */
    /* We must implement read property - it's required! */
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_REINITIALIZE_DEVICE, handler_reinitialize_device);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_ATOMIC_READ_FILE, handler_atomic_read_file);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_ATOMIC_WRITE_FILE, handler_atomic_write_file);
    /* handle communication so we can shutup when asked */
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL,
        handler_device_communication_control);
    /* start the cyclic 1 second timer for DCC */
    mstimer_set(&BACnet_Task_Timer, 1000);
    mstimer_set(&BACnet_TSM_Timer, 50);
    /* Hello World! */
    Send_I_Am(&Handler_Transmit_Buffer[0]);
}

/** Static receive buffer, initialized with zeros by the C Library Startup Code. */

static uint8_t PDUBuffer[MAX_MPDU + 16 /* Add a little safety margin to the buffer,
                                        * so that in the rare case, the message
                                        * would be filled up to MAX_MPDU and some
                                        * decoding functions would overrun, these
                                        * decoding functions will just end up in
                                        * a safe field of static zeros. */];

/** BACnet task handling receiving and transmitting of messages.  */
void bacnet_task(void)
{
    uint16_t pdu_len;
    BACNET_ADDRESS src; /* source address */
    uint8_t i;
    uint32_t elapsed_milliseconds = 0;

    /* handle the communication timer */
    if (mstimer_expired(&BACnet_Task_Timer)) {
        mstimer_reset(&BACnet_Task_Timer);
	elapsed_milliseconds = mstimer_interval(&BACnet_Task_Timer);
        dcc_timer_seconds(elapsed_milliseconds/1000);
        datalink_maintenance_timer(elapsed_milliseconds/1000);
        handler_cov_timer_seconds(elapsed_milliseconds/1000);
    }
    if (mstimer_expired(&BACnet_TSM_Timer)) {
        mstimer_reset(&BACnet_TSM_Timer);
	elapsed_milliseconds = mstimer_interval(&BACnet_TSM_Timer);
        tsm_timer_milliseconds(elapsed_milliseconds);
    }
    /* handle the messaging */
    pdu_len = datalink_receive(&src, &PDUBuffer[0], MAX_MPDU, 0);
    if (pdu_len) {
        npdu_handler(&src, &PDUBuffer[0], pdu_len);
    }

    handler_cov_task();
}

//void datetime_init(void)
//{
//}
//
//bool datetime_local(
//    BACNET_DATE * bdate,
//    BACNET_TIME * btime,
//    int16_t * utc_offset_minutes,
//    bool * dst_active)
//{
//    return true;
//}
