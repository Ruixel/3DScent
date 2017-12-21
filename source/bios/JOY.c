#include "types.h"

void joy_init ()
{
}

void joy_close()
{
}

void joy_set_type(ubyte type)
{
}

void joy_flush()
{
}

ubyte joystick_read_raw_axis( ubyte mask, int * axis )
{
	return 0;
}

ubyte joy_get_present_mask()
{
	return 0;
}

void joy_get_cal_vals(int *axis_min, int *axis_center, int *axis_max)
{
}

void joy_set_cal_vals(int *axis_min, int *axis_center, int *axis_max)
{
}

int joy_get_scaled_reading( int raw, int axn )
{
	return 0;
}

void joy_set_cen() 
{
}

int joy_get_button_down_cnt( int btn )
{
	return 0;
}

int joy_get_button_state( int btn )
{
	return 0;
}
