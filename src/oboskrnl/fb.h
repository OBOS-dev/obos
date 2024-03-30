/*
	oboskrnl/fb.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>


namespace obos
{
	enum class FramebufferFormat
	{
		FB_FORMAT_INVALID,
		FB_FORMAT_RGB888,
		FB_FORMAT_BGR888,
		FB_FORMAT_RGBX8888,
		FB_FORMAT_XRGB8888,
	};
	struct Framebuffer
	{
		void* address;
		// Width in bytes/bpp/8
		uint32_t width;
		// Height in bytes/bpp/8
		uint32_t height;
		// Pitch in bytes
		uint32_t pitch;
		// Bits Per Pixel.
		int bpp;
		// The format of the framebuffer.
		FramebufferFormat format;
	};
	union Pixel
	{
		Pixel() = default;
		Pixel(uint32_t rgbx)
		{
			raw.rgbx = rgbx;
			ele.x = 0;
		}
		Pixel(uint8_t r,
			  uint8_t g,
			  uint8_t b)
		{
			ele.r = r;
			ele.g = g;
			ele.b = b;
			ele.x = 0x00;
		}
		struct 
		{
			uint32_t rgbx;
		} raw;
		struct
		{
			uint8_t r;
			uint8_t g;
			uint8_t b;
			uint8_t x;
		} ele;
		inline uint32_t ToFormat(FramebufferFormat format)
		{
			switch (format)
			{
			case FramebufferFormat::FB_FORMAT_RGB888: return raw.rgbx >> 8;
			case FramebufferFormat::FB_FORMAT_BGR888: return ele.b | ((uint32_t)ele.g << 8) | ((uint32_t)ele.r << 16);
			case FramebufferFormat::FB_FORMAT_RGBX8888: return raw.rgbx;
			case FramebufferFormat::FB_FORMAT_XRGB8888: return ele.r | ((uint32_t)ele.g << 8) | ((uint32_t)ele.b << 16);
			default: break;
			}
			return 0;
		}
		bool operator==(const Pixel& other)
		{
			return raw.rgbx == other.raw.rgbx;
		}
		bool operator!=(const Pixel& other)
		{
			return raw.rgbx != other.raw.rgbx;
		}
	};
}