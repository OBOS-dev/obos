/*
	drivers/testDriver/main.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
//#include <klog.h>

#include <driver_interface/header.h>

using namespace obos;
[[maybe_unused]] volatile
driverInterface::driverHeader __attribute__((section(OBOS_DRIVER_HEADER_SECTION))) g_driverHeader = {
	.magic = driverInterface::g_driverHeaderMagic,
	.type  = driverInterface::driverType::KernelExtension,
	.friendlyName = "Test driver",
	.requestedLoader = {0},
	.loaderPacket = nullptr
};

namespace obos
{
	namespace logger
	{
		__attribute__((weak)) size_t log(const char* format, ...);
	}
}

extern "C"
void _start()
{
	(g_driverHeader);
	logger::log("In test driver!\n");
	while (1);
}