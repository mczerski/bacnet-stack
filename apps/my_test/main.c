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

void setup(uint32_t object_id)
{
    srand(time(NULL));
    mstimer_init();
    bacnet_init(object_id);

    Analog_Input_COV_Increment_Set(0, 0.01);
    Analog_Input_COV_Increment_Set(1, 1);
    Analog_Input_COV_Increment_Set(2, 0.1);

    Analog_Input_Present_Value_Set(0, rand_ain0());
    Analog_Input_Present_Value_Set(1, rand_ain1());
    Analog_Input_Present_Value_Set(2, rand_ain2());
}

size_t changes = 0;

int main(int argc, const char* argv[])
{
    size_t loopc = 0;
    if (argc != 2) {
        return -1;
    }
    setup(atoi(argv[1]));
    for (;;) {
        bacnet_task();
        usleep(10000);
        loopc++;
        if (loopc % 500 == 0) {
	    float value = rand_ain0();
            Analog_Input_Present_Value_Set(0, value);
	    if (Analog_Input_Change_Of_Value(0)) {
		changes++;
                fprintf(stderr, "AI(0) changed: %f\n", value);
	    }
	}
        if (loopc % 600 == 0) {
	    float value = rand_ain1();
            Analog_Input_Present_Value_Set(1, rand_ain1());
	    if (Analog_Input_Change_Of_Value(1)) {
		changes++;
                fprintf(stderr, "AI(1) changed: %f\n", value);
	    }
	}
        if (loopc % 1000 == 0) {
	    float value = rand_ain2();
            Analog_Input_Present_Value_Set(2, rand_ain2());
	    if (Analog_Input_Change_Of_Value(2)) {
		changes++;
                fprintf(stderr, "AI(2) changed: %f\n", value);
	    }
	}
    }
}
