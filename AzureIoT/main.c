#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/time.h>

#include "applibs_versions.h"
#include <applibs/log.h>
#include <applibs/networking.h>
#include <applibs/gpio.h>

#include <hw/sample_hardware.h>
#include "parson.h"

#include "epoll_timerfd_utilities.h"

#include "azure.h"
#include "display.h"
#include "keyboard.h"
#include "screens.h"

static volatile sig_atomic_t terminationRequired = false;

enum LockState {
	OPEN,
	CLOSED
} lockState = CLOSED;

//currently selected menu
enum CurrentMenu {
	NORMAL_OP,
	CHANGE_PASSWORD,
	CONFIG,
	CHANGE_CONFIG_PASSWORD,
	CHANGE_LOCK_MODE,
	CHANGE_LOCK_CONTACT_MODE,
	CHANGE_MONO_SWITCH_TIME,
	CHANGE_DISPLAY_BACKLIGHT_MODE,
	FACTORY_RESET
} currentMenu = NORMAL_OP;

enum LockMode {
	BI,
	MONO
} lockMode = MONO;

enum ContactMode {
	NORMAL_OPEN,
	NORMAL_CLOSED
} contactMode = NORMAL_OPEN;

enum DisplayBacklight {
	NONE,
	AUTO,
	CONSTANT
} displayBacklight = AUTO;

char defaultUserPassword[12] = { '1', '2', '3', '4', '\0' };
char userPassword[12] = { '1', '2', '3', '4', '\0' };
char defaultAdminPassword[12] = { '1', '2', '3', '4', '5', '\0' };
char adminPassword[12] = { '1', '2', '3', '4', '5', '\0' };
char charBuffer[12] = { 0 };//stores keystrokes
int invalidTries = 0;//incremented when hash or star function used with invalid credentials

unsigned long unlockStartTime = 0;//point in time where lock was unlocked used to close it after monoSwitchTime in mono lock mode
unsigned long int monoSwitchTime = 5000;

unsigned long actionStartTime = 0;//point in time where some action took place used for timeout stuff i.e for display auto mode and leaving config menu after actionTimeout time of inactivity
const unsigned long actionTimeout = 15000;

bool blockLock = false;//if true then keyboard is inaccessible and "too many invalid attempts" is displayed, set after 3 invalid attempts to use hash or star function, released after blockLockTimeout time
unsigned long blockLockStartTime = 0;//time where blockLock happened
unsigned long blockLockTimeout = 30000;

unsigned long failedAttemptsResetStartTime = 0;//point in time where invalid attempt took place, used to reset invalid attempt counter after some time
unsigned long failedAttemptsResetTimeout = 30000;

bool isAlarm = false;//door opened when it should be locked -> lock broken or intrusion
bool displayOff = false;//if true and backlight mode = auto then display is off, set after some time of inactivity
bool alwaysOpen = false;//flag received from azure, set lock always open
bool alwaysClosed = false;//flag received from azure, set lock always closed

//app stuff
static int runApp();

static int doStarAction();//performed when user pressed '*' on matrix keypad
static int doHashAction();//performed when user pressed '#' on matrix keypad
static int goBack();//performed when user pressed 'B' on matrix keypad

static bool addToBuffer(char c);//adds c to buffer if not empty
static void clearBuffer();//clears buffer used when necessary and when user pressed 'C' on matrix keypad

static unsigned long getTimeMs();//returns system time in milliseconds

static int lock();//locks the door relay
static int unlock();//unlocks the door relay
static int isLocked(bool *v);//set v to given bool when door relay is locked

static int drawNormalOp();//draw locked,unlocked or alarm on display

static int isDoorOpen(bool *v);//set given bool to true if door sensor returns open
static int doorStateChanged(bool *v);//sets given bool to true if door sensor's state changed

static int setAlarm();//opens alarm relay's circuit and triggers alarm on master device
static int resetAlarm();//closes alarm relay's circuit and triggers alarm on master device

static void factoryReset();

static void AppTimerEventHandler(EventData* eventData);

//azure stuff
extern IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle;
extern bool iothubAuthenticated;
static bool synced = false;//set to true after first twin report state

static void AzureTimerEventHandler(EventData* eventData);

// Initialization/Cleanup
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);

//gpio
static int doorLockPin = MT3620_GPIO0;
static int doorLockFd = -1;
static int doorSensorPin = MT3620_GPIO42;
static int doorSensorFd = -1;
static int alarmPin = MT3620_GPIO29;
static int alarmFd;


// Timer / polling
static int appTimerFd = -1;
static int azureTimerFd = -1;
static int epollFd = -1;

static void TerminationHandler(int signalNumber)
{
    terminationRequired = true;
}

int main(int argc, char *argv[])
{
    Log_Debug("IoT Hub/Central Application starting.\n");

    if (argc == 2) {
        Log_Debug("Setting Azure Scope ID %s\n", argv[1]);
        strncpy(scopeId, argv[1], SCOPEID_LENGTH);
    } else {
        Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs\n");
        return -1;
    }

    if (InitPeripheralsAndHandlers() != 0) {
        terminationRequired = true;
    }

	//lock door on startup
	if (lock() < 0)
	{
		terminationRequired = true;
	}

	resetAlarm();//close relay's circuit

	actionStartTime = getTimeMs();

	drawWait();

    while (!terminationRequired) {
        if (WaitForEventAndCallHandler(epollFd) != 0) {
            terminationRequired = true;
        }
    }

    ClosePeripheralsAndHandlers();

    Log_Debug("Application exiting.\n");

    return 0;
}

static int runApp()
{
	if (!synced)//if lock hasn't received configuration state from azure yet then don't do nothing
		return 0;

	if (alwaysOpen)//azure sent always open flag so keep the lock open
	{
		unlock();
		unlockStartTime = getTimeMs();//for monostable only to close it after some time when always open goes off
	}

	if (alwaysClosed)//azure sent always closed flag so keep the lock closed
	{
		lock();
	}

	bool doorOpen;
	bool doorChanged;
	if (doorStateChanged(&doorChanged)  < 0)
		return -1;
	if (isDoorOpen(&doorOpen) < 0)
		return -1;

	if (doorChanged)
	{
		if (doorOpen)
		{
			TwinReportState("IsDoorOpen", "true");
			SendTelemetry("DoorEvent", "Door opened.");
			Log_Debug("Door opened.\n");
		}
		else
		{
			TwinReportState("IsDoorOpen", "false");
			SendTelemetry("DoorEvent", "Door closed.");
			Log_Debug("Door closed.\n");
		}
	}

	//door opened when lock was closed so trigger the alarm
	//it's triggered only in normal op so it doesn't trigger in any config mode
	//as if someone has access to config then the person also has access to the lock/unlock function itself
	if ( doorChanged && doorOpen && currentMenu == NORMAL_OP && !isAlarm && lockState == CLOSED)
	{
		setAlarm();

		if (drawAlarm() < 0)
			return -1;	
	}

	unsigned long now = getTimeMs();

	//close lock if is in mono mode and monoSwitchTime passed since opening
	if (lockState == OPEN && lockMode == MONO && now - unlockStartTime >= monoSwitchTime)
	{
		if (lock() < 0){
			return -1;
		}
	}

	//set display to off if it's in none mode
	if (!displayOff && displayBacklight == NONE)
	{
		displayOff = true;
		drawBlank();
	}

	//return to normal op after timeout
	//and set display to off if is in auto mode
	if (now - actionTimeout >= actionStartTime)
	{
		if (currentMenu != NORMAL_OP && currentMenu != CHANGE_PASSWORD)
		{
			SendTelemetry("ConfigEvent", "Config exited due to timeout.");
		}
		if (displayBacklight == AUTO && !isAlarm)//set display off after timeout
		{
			drawBlank();
			displayOff = true;
		}
		if (currentMenu != NORMAL_OP)//return to normal op menu and draw normal op if display is set to constant
		{
			currentMenu = NORMAL_OP;
			if (displayBacklight == CONSTANT)
			{
				drawNormalOp();
			}
		}

		clearBuffer();
	}

	//disable block lock after blockLockTimeout passed
	if (blockLock && now - blockLockStartTime >= blockLockTimeout)
	{
		invalidTries = 0;
		blockLock = false;
		if (displayBacklight == CONSTANT)
			drawNormalOp();
	}

	//reset failed attempts counter to 0 after some time
	if (now - failedAttemptsResetStartTime >= failedAttemptsResetTimeout)
	{
		invalidTries = 0;
	}

	char key = 0;
	if (checkForKeyPress(&key) < 0) {
		return -1;
	}
	if (key)
	{
		Log_Debug("key pressed: %c\n", key);

		actionStartTime = now;//action, keypress happened

		if (displayOff)
		{
			displayOff = false;
			if (blockLock)
			{
				drawBlockLock();
				return 0;
			}
			drawNormalOp();
		}

		switch (key)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			addToBuffer(key);
			break;
		case '*'://star works only in normal op
			if (currentMenu == NORMAL_OP)
			{
				if (doStarAction())
					return -1;
				clearBuffer();
			}
			break;
		case '#':
			if (doHashAction() < 0)
				return -1;
			clearBuffer();
			break;
		case 'B':
			goBack();
			clearBuffer();
			break;
		case 'C':
			clearBuffer();
			break;
		}
	}
	return 0;
}

//does action on "*" pressed depending on content of charr buffer
static int doStarAction()
{
	switch (currentMenu)
	{
	case NORMAL_OP:
		//char buffer equals to user password
		//so trigger change password for user
		if (!strcmp(charBuffer, userPassword))
		{
			currentMenu = CHANGE_PASSWORD;
			if (isAlarm)
			{
				resetAlarm();
			}
			if (displayBacklight != NONE)
				drawChangePassword();
			Log_Debug("Change user password.\n");
		}
		//char buffer equals to admin password
		//so go to config
		else if (!strcmp(charBuffer, adminPassword))
		{
			currentMenu = CONFIG;
			if (isAlarm)
			{
				resetAlarm();
			}
			if (displayBacklight != NONE)
				drawConfig();
			SendTelemetry("ConfigEvent", "Config accessed.");
			Log_Debug("Config mode.\n");
		}
		else
		{
			invalidTries++;
			failedAttemptsResetStartTime = getTimeMs();//reset invalid tries after some time

			SendTelemetry("ConfigWarning", "Invalid credentials for star function.");

			Log_Debug("Invalid credentials.\n");
			if (invalidTries == 3)//after 3 invalid tries send warning to azure and lock access to the keyboard for some time
			{
				blockLock = true;
				blockLockStartTime = getTimeMs();//return keyboard access after some time
				if (displayBacklight != NONE)
					drawBlockLock();

				Log_Debug("Three invalid attempts\nLock functionality disabled for 30 seconds.\n");
				SendTelemetry("LockWarning", "Three invalid attempts, lock functionality disabled for 30 seconds.\n");
			}
		}
		break;
	}
	return 0;
}

//does action on "#" press depending on content of char buffer
static int doHashAction()
{
	switch (currentMenu)
	{
	case NORMAL_OP:
		//charr buffer equals to one of the passwords
		//so unlock or lock(if bistable) door and reset alarm if there is one
		if (!strcmp(charBuffer, userPassword) || !strcmp(charBuffer, adminPassword))
		{
			if (isAlarm)
			{
				resetAlarm();
				if (displayBacklight != NONE)
					drawNormalOp();
				return 0;
			}
			
			bool v;
			if (isLocked(&v) < 0)
				return -1;
			if (lockMode == BI && !v)//for bistable if door is unlocked then lock
			{
				lock();
			}
			else
			{
				unlock();
				unlockStartTime = getTimeMs();//for monostable only to close it after some time
			}
			invalidTries = 0;//reset invalid tries when correct credentials given
		}
		else
		{
			invalidTries++;
			failedAttemptsResetStartTime = getTimeMs();//reset invalid tries after some time

			Log_Debug("Invalid credentials.\n");

			SendTelemetry("ConfigWarning", "Invalid credentials.");

			if (invalidTries == 3)//after 3 invalid tries send warning to azure and lock access to the keyboard for some time
			{
				blockLock = true;
				blockLockStartTime = getTimeMs();//return keyboard access after some time
				if(displayBacklight != NONE)
					drawBlockLock();

				Log_Debug("Three invalid attempts\nLock functionality disabled for 30 seconds.\n");
				SendTelemetry("LockWarning", "Three invalid attempts, lock functionality disabled for 30 seconds.\n");
			}
		}
		break;
	case CHANGE_PASSWORD:
		if (strcmp(charBuffer, adminPassword))
		{
			strcpy(userPassword, charBuffer);

			char msg[20];
			snprintf(msg, 20, "\"%s\"", userPassword);
			TwinReportState("UserPassword", msg);

			Log_Debug("User password changed.\n");
			SendTelemetry("UserEvent", "User password changed.");
		}
		currentMenu = NORMAL_OP;
		if (displayBacklight != NONE)
			drawNormalOp();
		break;
	case CONFIG:
		if (!strcmp("1", charBuffer))
		{
			currentMenu = CHANGE_CONFIG_PASSWORD;
			if (displayBacklight != NONE)
				drawChangePassword();
			Log_Debug("Change config password.\n");
		}
		else if (!strcmp("2", charBuffer))
		{
			currentMenu = CHANGE_LOCK_MODE;
			if (displayBacklight != NONE)
				drawChangeLockMode();
			Log_Debug("Change lock mode.\n");
		}
		else if (!strcmp("3", charBuffer))
		{
			currentMenu = CHANGE_LOCK_CONTACT_MODE;
			if (displayBacklight != NONE)
				drawChangeContactMode();
			Log_Debug("Change lock contact mode.\n");
		}
		else if (!strcmp("4", charBuffer))
		{
			currentMenu = CHANGE_MONO_SWITCH_TIME;
			if (displayBacklight != NONE)
				drawChangeMonoSwitchTime();
			Log_Debug("Change mono switch time.\n");
		}
		else if (!strcmp("5", charBuffer))
		{
			currentMenu = CHANGE_DISPLAY_BACKLIGHT_MODE;
			if (displayBacklight != NONE)
				drawChangeDisplayMode();
			Log_Debug("Change display backlight.\n");
		}
		else if (!strcmp("6", charBuffer))
		{
			currentMenu = CHANGE_DISPLAY_BACKLIGHT_MODE;
			Log_Debug("Change display backlight.\n");
		}
		break;
	case CHANGE_CONFIG_PASSWORD:
		if (strcmp(charBuffer, userPassword))
		{
			strcpy(adminPassword, charBuffer);
			Log_Debug("Config password changed.\n");

			char msg[20];
			snprintf(msg, 20, "\"%s\"", adminPassword);
			TwinReportState("ConfigPassword", msg);
			SendTelemetry("ConfigEvent", "Config password changed.");
		}
		currentMenu = CONFIG;
		if (displayBacklight != NONE)
			drawConfig();
		break;
	case CHANGE_LOCK_MODE:
		if (!strcmp("1", charBuffer))
		{
			lockMode = MONO;
			
			Log_Debug("Lock mode changed to monostable.\n");
			TwinReportState("LockMode", "\"Monostable\"");
			SendTelemetry("ConfigEvent", "Lock mode changed to monostable.");
		}
		else if (!strcmp("2", charBuffer))
		{
			lockMode = BI;
			Log_Debug("Lock mode changed to bistable.\n");
			TwinReportState("LockMode", "\"Bistable\"");
			SendTelemetry("ConfigEvent", "Lock mode changed to bistable.");
		}
		currentMenu = CONFIG;
		if (displayBacklight != NONE)
			drawConfig();
		break;
	case CHANGE_LOCK_CONTACT_MODE:
		if (!strcmp("1", charBuffer))
		{
			contactMode = NORMAL_OPEN;
			lock();
			Log_Debug("Lock contact mode changed to normal open.\n");
			TwinReportState("ContactMode", "\"Normal open\"");
			SendTelemetry("ConfigEvent", "Lock contact mode changed to normal open.");
		}
		else if (!strcmp("2", charBuffer))
		{
			contactMode = NORMAL_CLOSED;
			lock();
			Log_Debug("Lock contact mode changed to normal closed.\n");
			TwinReportState("ContactMode", "\"Normal closed\"");
			SendTelemetry("ConfigEvent", "Lock contact mode changed to normal closed.");
		}
		currentMenu = CONFIG;
		if (displayBacklight != NONE)
			drawConfig();
		break;
	case CHANGE_MONO_SWITCH_TIME:
		if (strlen(charBuffer) != 0)
		{
			int val;
			sscanf(charBuffer, "%d", &val);
			if (val < 999 || val != 0)
			{
				monoSwitchTime = val * 1000;
				char valbuf[10];
				snprintf(valbuf, 10, "\"%d\"", val);
				TwinReportState("MonoSwitchTime", valbuf);

				char buf[50];
				snprintf(buf, 50, "Changed mono switch time to %d seconds.", val);
				SendTelemetry("ConfigEvent", buf);
			}
		}
		currentMenu = CONFIG;
		if (displayBacklight != NONE)
			drawConfig();
		break;
	case CHANGE_DISPLAY_BACKLIGHT_MODE:
		if (!strcmp("1", charBuffer))
		{
			displayBacklight = NONE;
			Log_Debug("Changed display backlight mode to none.\n");
			TwinReportState("DisplayBacklightMode", "\"None\"");
			SendTelemetry("ConfigEvent", "Changed display backlight mode to none.");
		}
		else if (!strcmp("2", charBuffer))
		{
			displayBacklight = AUTO;
			Log_Debug("Changed display backlight mode to auto.\n");
			TwinReportState("DisplayBacklightMode", "\"Auto\"");
			SendTelemetry("ConfigEvent", "Changed display backlight mode to auto.");
		}
		else if (!strcmp("3", charBuffer))
		{
			displayBacklight = CONSTANT;
			Log_Debug("Changed display backlight mode to constant.\n");
			TwinReportState("DisplayBacklightMode", "\"Constant\"");
			SendTelemetry("ConfigEvent", "Changed display backlight mode to constant.");
		}
		currentMenu = CONFIG;
		if (displayBacklight != NONE)
			drawConfig();
		break;
	case FACTORY_RESET:
		factoryReset();
		break;
	}
	return 0;
}

//goes to previous menu
//does nothing if current menu is normal op
static int goBack()
{
	switch (currentMenu)
	{
	case CHANGE_PASSWORD:
		currentMenu = NORMAL_OP;
		if (displayBacklight != NONE)
			drawNormalOp();
		Log_Debug("Change password canceled.\n");
		break;
	case CONFIG:
		currentMenu = NORMAL_OP;
		if (displayBacklight != NONE)
			drawNormalOp();
		Log_Debug("Left config menu.\n");
		SendTelemetry("ConfigEvent", "Config exited.");
		break;
	case CHANGE_CONFIG_PASSWORD:
	case CHANGE_LOCK_MODE:
	case CHANGE_LOCK_CONTACT_MODE:
	case CHANGE_MONO_SWITCH_TIME:
	case CHANGE_DISPLAY_BACKLIGHT_MODE:
		currentMenu = CONFIG;
		if (displayBacklight != NONE)
			drawConfig();
		Log_Debug("Config operation canceled.\n");
		break;
	}
	return 0;
}

//adds given char to char buffer
//returns true if char added
//false if buffer is full
static bool addToBuffer(char c)
{
	int len = strlen(charBuffer);
	if (len == 12)
		return false;
	charBuffer[len] = c;
	return true;
}

//clears charr buffer
static void clearBuffer()
{
	for (int i = 0; i < 12; i++)
	{
		charBuffer[i] = '\0';
	}
}

static unsigned long getTimeMs()
{
	struct timeval te;
	gettimeofday(&te, NULL); // get current time
	long long milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000; // calculate milliseconds
	// printf("milliseconds: %lld\n", milliseconds);
	return milliseconds;
}

//sets given bool to true if door relay is locked
//returns 0 or -1 if error
static int isLocked(bool* v)
{
	if (contactMode == NORMAL_OPEN)
	{
		GPIO_Value_Type val;
		if (GPIO_GetValue(doorLockFd, &val) < 0)
			return -1;
		if (val == GPIO_Value_Low)
			*v = true;
		else
			*v = false;
	}
	else
	{
		GPIO_Value_Type val;
		if (GPIO_GetValue(doorLockFd, &val) < 0)
			return -1;
		if (val == GPIO_Value_High)
			*v = true;
		else
			*v = false;
	}
	return 0;
}

//locks the door relay
//sets relay open or closed depending on contact mode
//returns -1 on error
static int lock()
{
	if (alwaysOpen)
		return 0;
	if (contactMode == NORMAL_OPEN)
	{
		GPIO_Value_Type val;
		if (GPIO_GetValue(doorLockFd, &val) < 0)
			return -1;
		if ( val != GPIO_Value_Low)
		{
			if (GPIO_SetValue(doorLockFd, GPIO_Value_Low))
				return -1;

			Log_Debug("Lock locked.\n");
			TwinReportState("IsLockOpen", "false");
			SendTelemetry("LockEvent", "Lock locked.");
		}
		else
			return 0;
		
	}
	else
	{
		GPIO_Value_Type val;
		if (GPIO_GetValue(doorLockFd, &val) < 0)
			return -1;
		if (val != GPIO_Value_High)
		{
			if (GPIO_SetValue(doorLockFd, GPIO_Value_High))
				return -1;

			Log_Debug("Lock locked.\n");
			TwinReportState("IsLockOpen", "false");
			SendTelemetry("LockEvent", "Lock locked.");
		}
		else
			return 0;
	}
	if (displayBacklight != NONE && currentMenu == NORMAL_OP)
		drawLocked();
	lockState = CLOSED;
	return 0;
}

//unlocks the door relay
//sets relay open or closed depending on contact mode
//returns -1 on error
static int unlock()
{
	if (alwaysClosed)
		return 0;
	if (contactMode == NORMAL_OPEN)
	{
		GPIO_Value_Type val;
		if (GPIO_GetValue(doorLockFd, &val) < 0)
			return -1;
		if (val != GPIO_Value_High)
		{
			if (GPIO_SetValue(doorLockFd, GPIO_Value_High))
				return -1;

			Log_Debug("Lock unlocked.\n");
			TwinReportState("IsLockOpen", "true");
			SendTelemetry("LockEvent", "Lock unlocked.");
		}
		else
			return 0;
	}
	else
	{
		GPIO_Value_Type val;
		if (GPIO_GetValue(doorLockFd, &val) < 0)
			return -1;
		if (val != GPIO_Value_Low)
		{
			if (GPIO_SetValue(doorLockFd, GPIO_Value_Low))
				return -1;

			Log_Debug("Lock unlocked.\n");
			TwinReportState("IsLockOpen", "true");
			SendTelemetry("LockEvent", "Lock unlocked.");
		}
		else
			return 0;
	}
	if(displayBacklight != NONE && currentMenu == NORMAL_OP)
		drawUnlocked();
	lockState = OPEN;
	return 0;
}

//set given bool to true if door is opened
//returns 0 or -1 if error
static int isDoorOpen(bool* v)
{
	GPIO_Value_Type val;
	if (GPIO_GetValue(doorSensorFd, &val) < 0)
		return -1;

	if (val == GPIO_Value_High)
	{
		*v = true;
		return 0;
	}
	*v = false;
	return 0;
}

//sets given bool to true if door state changed from closed to open or vice versa
//returns 0 or -1 if error
static int doorStateChanged(bool *v)
{
	static bool previousState = false;
	static bool fstRun = true;

	//for first time get current value of the door
	if (fstRun)
	{
		fstRun = false;
		bool r;
		if (isDoorOpen(&r) < 0)
			return -1;
		previousState = r;
		*v = false;
		return 0;
	}

	bool r;
	if (isDoorOpen(&r) < 0)
		return -1;
	if (previousState != r)
	{
		previousState = r;
		*v = true;
		return 0;
	}
	previousState = r;
	*v = false;
	return 0;
	
}

//draw normal operation
//locked/unlocked/alarm
//doesn't draw if display backlight mode is set to none
static int drawNormalOp()
{
	if (isAlarm)
	{
		if (drawAlarm() < 0)
			return -1;
		return 0;
	}

	if (displayBacklight == NONE)
		return 0;

	if (lockState == CLOSED)
	{
		if (drawLocked() < 0)
			return -1;
	}
	else
	{
		if (drawUnlocked() < 0)
			return -1;
	}
	return 0;
}

//open alarm relay and trigger alarm state in master
//returns 0 or -1 if error
static int setAlarm()
{
	if (isAlarm)
		return 0;
	isAlarm = true;
	TwinReportState("IsAlarm", "true");
	Log_Debug("Alarm!\n");
	SendTelemetry("LockCritical", "Intrusion!");

	if (GPIO_SetValue(alarmFd, GPIO_Value_High))
		return -1;
	return 0;
}

//close alarm relay
//returns 0 or -1 if error
static int resetAlarm()
{
	if (!isAlarm)
		return 0;
	isAlarm = false;
	TwinReportState("IsAlarm", "false");
	Log_Debug("Alarm cleared.\n");
	SendTelemetry("LockCritical", "Alarm cleared.");
	if (GPIO_SetValue(alarmFd, GPIO_Value_Low))
		return -1;
	if(displayBacklight != NONE)
	drawNormalOp();
	return 0;
}

static void factoryReset()
{
	strcpy(userPassword, defaultUserPassword);
	TwinReportState("UserPassword", "\"1234\"");

	strcpy(adminPassword, defaultAdminPassword);
	TwinReportState("ConfigPassword", "\"12345\"");

	lockMode = MONO;
	TwinReportState("LockMode", "\"Monostable\"");

	monoSwitchTime = 5000;
	TwinReportState("MonoSwitchTime", "\"5\"");

	contactMode = NORMAL_OPEN;
	TwinReportState("ContactMode", "\"Normal open\"");

	displayBacklight = AUTO;
	TwinReportState("DisplayBacklightMode", "\"Auto\"");

	currentMenu = NORMAL_OP;
	if (displayBacklight != NONE)
		drawNormalOp();
	SendTelemetry("ConfigEvent", "Factory reset performed.");
}

static EventData appEventData = { .eventHandler = &AppTimerEventHandler };

static void AppTimerEventHandler(EventData* eventData)
{
	if (ConsumeTimerFdEvent(appTimerFd)){
		terminationRequired = true;
		return;
	}

	if (runApp() < 0) {
		terminationRequired = true;
	}
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData azureEventData = {.eventHandler = &AzureTimerEventHandler};

//Azure timer event:  Check connection status and send telemetry
static void AzureTimerEventHandler(EventData* eventData)
{
	if (ConsumeTimerFdEvent(azureTimerFd) != 0) {
		terminationRequired = true;
		return;
	}

	bool isNetworkReady = false;
	if (Networking_IsNetworkingReady(&isNetworkReady) != -1) {
		if (isNetworkReady && !iothubAuthenticated) {
			int period = SetupAzureClient();
			struct timespec azureTelemetryPeriod = { period, 0 };
			SetTimerFdToPeriod(azureTimerFd, &azureTelemetryPeriod);
		}
	}
	else {
		Log_Debug("Failed to get Network state\n");
	}

	if (iothubAuthenticated) {
		IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
	}
}

static int InitPeripheralsAndHandlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

	doorSensorFd = GPIO_OpenAsInput(doorSensorPin);
	if (doorSensorFd < 0){
		return -1;
	}

	doorLockFd = GPIO_OpenAsOutput(doorLockPin, GPIO_OutputMode_OpenDrain, GPIO_Value_Low);
	if (doorSensorFd < 0) {
		return -1;
	}

	alarmFd = GPIO_OpenAsOutput(alarmPin, GPIO_OutputMode_OpenDrain, GPIO_Value_Low);
	if (doorSensorFd < 0) {
		return -1;
	}

	if (initDisplay() < 0) {
		return -1;
	}

	if (initKeyboard() < 0) {
		return -1;
	}

    epollFd = CreateEpollFd();
    if (epollFd < 0) {
        return -1;
    }

	struct timespec appTimerPeriod = { 0, 10000000 };
	appTimerFd = CreateTimerFdAndAddToEpoll(epollFd, &appTimerPeriod, &appEventData, EPOLLIN);
	if (appTimerFd < 0){
		return -1;
	}

    int azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
    struct timespec azureTelemetryPeriod = {azureIoTPollPeriodSeconds, 0};
    azureTimerFd = CreateTimerFdAndAddToEpoll(epollFd, &azureTelemetryPeriod, &azureEventData, EPOLLIN);
    if (azureTimerFd < 0) {
        return -1;
    }

    return 0;
}

static void ClosePeripheralsAndHandlers(void)
{
    Log_Debug("Closing file descriptors\n");

	cleanupDisplay();
	cleanupKeyboard();
	CloseFdAndPrintError(doorSensorFd, "DoorSensor");
	CloseFdAndPrintError(alarmFd, "Alarm");
	CloseFdAndPrintError(doorLockFd, "Lock");
	CloseFdAndPrintError(appTimerFd, "AppTimer");
    CloseFdAndPrintError(azureTimerFd, "AzureTimer");
    CloseFdAndPrintError(epollFd, "Epoll");
}

MethodCallback(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* response_size, void* userContextCallback)
{
	(void)userContextCallback;
	(void)payload;
	(void)size;

	int result;

	if (strcmp("ResetAlarm", method_name) == 0)
	{
		const char deviceMethodResponse[] = "{ \"Response\": \"Ok\" }";
		*response_size = sizeof(deviceMethodResponse) - 1;
		*response = malloc(*response_size);
		(void)memcpy(*response, deviceMethodResponse, *response_size);
		result = 200;
		resetAlarm();
		actionStartTime = getTimeMs();
	}
	if (strcmp("FactoryReset", method_name) == 0)
	{
		const char deviceMethodResponse[] = "{ \"Response\": \"Ok\" }";
		*response_size = sizeof(deviceMethodResponse) - 1;
		*response = malloc(*response_size);
		(void)memcpy(*response, deviceMethodResponse, *response_size);
		result = 200;
		factoryReset();
	}
	else
	{
		// All other entries are ignored.
		const char deviceMethodResponse[] = "{ }";
		*response_size = sizeof(deviceMethodResponse) - 1;
		*response = malloc(*response_size);
		(void)memcpy(*response, deviceMethodResponse, *response_size);
		result = -1;
	}

	return result;
}

void TwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload,
	size_t payloadSize, void* userContextCallback)
{
	if (!synced)
		drawNormalOp();

	synced = true;
	size_t nullTerminatedJsonSize = payloadSize + 1;
	char* nullTerminatedJsonString = (char*)malloc(nullTerminatedJsonSize);
	if (nullTerminatedJsonString == NULL) {
		Log_Debug("ERROR: Could not allocate buffer for twin update payload.\n");
		abort();
	}

	// Copy the provided buffer to a null terminated buffer.
	memcpy(nullTerminatedJsonString, payload, payloadSize);
	// Add the null terminator at the end.
	nullTerminatedJsonString[nullTerminatedJsonSize - 1] = 0;

	Log_Debug(nullTerminatedJsonString);

	JSON_Value* rootProperties = NULL;
	rootProperties = json_parse_string(nullTerminatedJsonString);
	if (rootProperties == NULL) {
		Log_Debug("WARNING: Cannot parse the string as JSON content.\n");
		goto cleanup;
	}

	JSON_Object* rootObject = json_value_get_object(rootProperties);
	JSON_Object* desiredProperties = json_object_dotget_object(rootObject, "desired");
	if (desiredProperties == NULL) {
		desiredProperties = rootObject;
	}

	JSON_Object* jsn = json_object_dotget_object(desiredProperties, "AlwaysOpen");
	if (jsn != NULL) {
		alwaysOpen = (bool)json_object_get_boolean(jsn, "value");
		actionStartTime = getTimeMs();
	}

	jsn = json_object_dotget_object(desiredProperties, "AlwaysClosed");
	if (jsn != NULL) {
		alwaysClosed = (bool)json_object_get_boolean(jsn, "value");
		actionStartTime = getTimeMs();
	}

	JSON_Object* reportedProperties = json_object_dotget_object(rootObject, "reported");
	if (reportedProperties == NULL) {
		reportedProperties = rootObject;
	}

	char *val = json_object_dotget_string(reportedProperties, "LockMode");
	if (val != NULL) {
		if (!strcmp(val, "Monostable"))
			lockMode = MONO;
		else
			lockMode = BI;
	}

	val = json_object_dotget_string(reportedProperties, "ContactMode");
	if (val != NULL) {
		if (!strcmp(val, "Normal open"))
			contactMode = NORMAL_OPEN;
		else
			contactMode = NORMAL_CLOSED;
		lock();
	}

	val = json_object_dotget_string(reportedProperties, "DisplayBacklightMode");
	if (val != NULL) {
		if (!strcmp(val, "None"))
			displayBacklight = NONE;
		else if (!strcmp(val, "Auto"))
			displayBacklight = AUTO;
		else
			displayBacklight = CONSTANT;
	}

	int intval = json_object_dotget_number(reportedProperties, "MonoSwitchTime");
	if (intval != NULL) {
		monoSwitchTime = intval * 1000;

		char valbuf[10];
		snprintf(valbuf, 10, "\"%d\"", intval);
	}

	val = json_object_dotget_string(reportedProperties, "UserPassword");
	if (val != NULL) {
		strcpy(userPassword, val);
	}

	val = json_object_dotget_string(reportedProperties, "ConfigPassword");
	if (val != NULL) {
		strcpy(adminPassword, val);
	}

cleanup:
	// Release the allocated memory.
	json_value_free(rootProperties);
	free(nullTerminatedJsonString);
}
