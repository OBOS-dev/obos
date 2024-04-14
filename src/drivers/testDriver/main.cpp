/*
	drivers/testDriver/main.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <driver_interface/header.h>

#include <scheduler/thread.h>

using namespace obos;
[[maybe_unused]] volatile
driverInterface::driverHeader OBOS_DEFINE_IN_SECTION(OBOS_DRIVER_HEADER_SECTION) g_driverHeader = {
	.magic = driverInterface::g_driverHeaderMagic,
	.type  = driverInterface::driverType::KernelExtension,
	.friendlyName = "Test driver",
	.requestedLoader = {0},
	.loaderPacket = nullptr
};

extern "C"
void _start()
{
	logger::log("In test driver!\n");
	scheduler::ExitCurrentThread();
}