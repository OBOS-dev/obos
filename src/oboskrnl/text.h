/*
 * oboskrnl/text.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

// Makeshift text renderer.

#define OBOS_FB_FORMAT_RGB888 1
#define OBOS_FB_FORMAT_BGR888 2
#define OBOS_FB_FORMAT_RGBX8888 3
#define OBOS_FB_FORMAT_XRGB8888 4

typedef struct
{
	uint32_t column;
	uint32_t row;
	void* font; // Must be 8x16 font.
	struct
	{
		void* base;
		uint32_t pitch;
		uint32_t width;
		uint32_t height;
		uint16_t format;
		uint8_t bpp;
	} fb;
} text_renderer_state;
extern text_renderer_state OBOS_TextRendererState;
obos_status OBOS_WriteCharacter(text_renderer_state* state, char ch);
obos_status OBOS_WriteCharacterAt(text_renderer_state* state, char ch, uint32_t column, uint32_t row);