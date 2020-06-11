#pragma once

#include "stdbool.h"

#include "epoll_timerfd_utilities.h"

// Azure IoT SDK
#include <iothub_client_core_common.h>
#include <iothub_device_client_ll.h>
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>
#include <iothub.h>
#include <azure_sphere_provisioning.h>

// Azure IoT Hub/Central defines.
#define SCOPEID_LENGTH 20
char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application, set in
									 // app_manifest.json, CmdArgs

extern IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle;
extern const int keepalivePeriodSeconds;
extern bool iothubAuthenticated;

void SendTelemetry(const unsigned char* key, const unsigned char* value);
int SetupAzureClient(void);
void TwinReportState(const char* propertyName, const char* propertyValue);

// Azure IoT poll periods
extern const int AzureIoTDefaultPollPeriodSeconds;