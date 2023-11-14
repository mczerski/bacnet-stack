#include "bacnet/basic/sys/mstimer.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/bactext.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet.h"
#include <unistd.h>
#include <time.h>
#include <stdlib.h>

float rand_ain0() {
    return 10.0 * rand() / RAND_MAX;
}

float rand_ain1() {
    return 300.0 * rand() / RAND_MAX;
}

float rand_ain2() {
    return 30.0 * rand() / RAND_MAX;
}

bool rand_bin0() {
    return 2 * rand() / RAND_MAX;
}

bool rand_bin1() {
    return 2 * rand() / RAND_MAX;
}

void setup()
{
    srand(time(NULL));
    mstimer_init();
    bacnet_init();

    Analog_Input_COV_Increment_Set(0, 0.01);
    Analog_Input_COV_Increment_Set(1, 1);
    Analog_Input_COV_Increment_Set(2, 0.1);

    Analog_Input_Present_Value_Set(0, rand_ain0());
    Analog_Input_Present_Value_Set(1, rand_ain1());
    Analog_Input_Present_Value_Set(2, rand_ain2());
    Binary_Input_Present_Value_Set(0, rand_bin0());
    Binary_Input_Present_Value_Set(1, rand_bin1());
}

int main()
{
    size_t loopc = 0;
    setup();
    for (;;) {
        bacnet_task();
	sleep(0.01);
	loopc++;
	if (loopc % 500 == 0)
	    Analog_Input_Present_Value_Set(0, rand_ain0());
	if (loopc % 600 == 0)
	    Analog_Input_Present_Value_Set(1, rand_ain1());
        if (loopc % 1000 == 0)
	    Analog_Input_Present_Value_Set(2, rand_ain2());
	if (loopc % 750 == 0)
	    Binary_Input_Present_Value_Set(0, rand_bin0());
	if (loopc % 750 == 0)
	    Binary_Input_Present_Value_Set(1, rand_bin1());
    }
}
