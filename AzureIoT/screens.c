#include "screens.h"
#include "display.h"

int drawWait()
{
	int result = fillScreen(0);
	if (result < 0)
		return -1;

	result = drawText("Sync in", 30, 25, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("progress...", 30, 35, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawAlarm()
{
	int result = fillScreen(0xff0000);
	if (result < 0)
		return -1;

	result = drawText("Alarm!", 30, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawBlank()
{
	int result = fillScreen(0);
	if (result < 0)
		return -1;
}

int drawLocked()
{
	int result = fillScreen(0xff0000);
	if (result < 0)
		return -1;

	result = drawText("Locked.", 30, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawUnlocked()
{
	int result = fillScreen(0x00ff00);
	if (result < 0)
		return -1;

	result = drawText("Unlocked.", 25, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawBlockLock()
{
	int result = fillScreen(0xFF0000);
	if (result < 0)
		return -1;

	result = drawText("Too many", 5, 25, 0xFFFFFF);
	if (result < 0)
		return -1;
	
	result = drawText("failed attempts.", 5, 35, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawConfig()
{
	int result = fillScreen(0x0000ff);
	if (result < 0)
		return -1;

	result = drawText("Config.", 30, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawChangePassword()
{
	int result = fillScreen(0x0000ff);
	if (result < 0)
		return -1;

	result = drawText("Change password.", 5, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawChangeLockMode()
{
	int result = fillScreen(0x0000ff);
	if (result < 0)
		return -1;

	result = drawText("Change lock", 10, 10, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("mode.", 10, 20, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("1# Monostable.", 10, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("2# Bistable.", 10, 40, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawChangeContactMode()
{
	int result = fillScreen(0x0000ff);
	if (result < 0)
		return -1;

	result = drawText("Change lock", 5, 10, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("contact mode.", 5, 20, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("1# Normal open.", 5, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("2# Normal closed.", 5, 40, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}

int drawChangeMonoSwitchTime()
{
	int result = fillScreen(0x0000ff);
	if (result < 0)
		return -1;

	result = drawText("Change mono", 8, 20, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("switch time.", 8, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("1 - 999 seconds.", 8, 40, 0xFFFFFF);
	if (result < 0)
		return -1;
	return 0;
}

int drawChangeDisplayMode()
{
	int result = fillScreen(0x0000ff);
	if (result < 0)
		return -1;

	result = drawText("Change display", 10, 10, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("mode.", 10, 20, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("1# None.", 10, 30, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("2# Auto.", 10, 40, 0xFFFFFF);
	if (result < 0)
		return -1;

	result = drawText("3# Constant.", 10, 50, 0xFFFFFF);
	if (result < 0)
		return -1;

	return 0;
}