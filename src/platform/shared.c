/*****************************************************************************
 * Copyright (c) 2014 Ted John
 * OpenRCT2, an open source clone of Roller Coaster Tycoon 2.
 * 
 * This file is part of OpenRCT2.
 * 
 * OpenRCT2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <math.h>
#include <SDL.h>
#include "../addresses.h"
#include "../config.h"
#include "../cursors.h"
#include "../drawing/drawing.h"
#include "../interface/keyboard_shortcut.h"
#include "../interface/window.h"
#include "../input.h"
#include "platform.h"

typedef void(*update_palette_func)(char*, int, int);

openrct2_cursor gCursorState;
const unsigned char *gKeysState;
unsigned char *gKeysPressed;
unsigned int gLastKeyPressed;
char* gTextInput;
int gTextInputLength;
int gTextInputMaxLength;
int gTextInputCursorPosition = 0;

int gNumResolutions = 0;
resolution *gResolutions = NULL;
int gResolutionsAllowAnyAspectRatio = 0;

SDL_Window *gWindow;

static SDL_Surface *_surface;
static SDL_Palette *_palette;
static int _screenBufferSize;
static void *_screenBuffer;
static SDL_Cursor* _cursors[CURSOR_COUNT];
static const int _fullscreen_modes[] = { 0, SDL_WINDOW_FULLSCREEN, SDL_WINDOW_FULLSCREEN_DESKTOP };
static unsigned int _lastGestureTimestamp;
static float _gestureRadius;

static void platform_create_window();
static void platform_load_cursors();
static void platform_unload_cursors();

int resolution_sort_func(const void *pa, const void *pb)
{
	const resolution *a = (resolution*)pa;
	const resolution *b = (resolution*)pb;

	int areaA = a->width * a->height;
	int areaB = b->width * b->height;

	if (areaA == areaB) return 0;
	if (areaA < areaB) return -1;
	return 1;
}

void platform_update_fullscreen_resolutions()
{
	int i, displayIndex, numDisplayModes;
	SDL_DisplayMode mode;
	resolution *resLook, *resPlace;
	float desktopAspectRatio, aspectRatio;

	// Query number of display modes
	displayIndex = SDL_GetWindowDisplayIndex(gWindow);
	numDisplayModes = SDL_GetNumDisplayModes(displayIndex);

	// Get desktop aspect ratio
	SDL_GetDesktopDisplayMode(displayIndex, &mode);
	desktopAspectRatio = (float)mode.w / mode.h;

	if (gResolutions != NULL)
		free(gResolutions);

	// Get resolutions
	gNumResolutions = numDisplayModes;
	gResolutions = malloc(gNumResolutions * sizeof(resolution));

	gNumResolutions = 0;
	for (i = 0; i < numDisplayModes; i++) {
		SDL_GetDisplayMode(displayIndex, i, &mode);
		
		aspectRatio = (float)mode.w / mode.h;
		if (gResolutionsAllowAnyAspectRatio || fabs(desktopAspectRatio - aspectRatio) < 0.0001f) {
			gResolutions[gNumResolutions].width = mode.w;
			gResolutions[gNumResolutions].height = mode.h;
			gNumResolutions++;
		}
	}

	// Sort by area
	qsort(gResolutions, gNumResolutions, sizeof(resolution), resolution_sort_func);

	// Remove duplicates
	resPlace = &gResolutions[0];
	for (int i = 1; i < gNumResolutions; i++) {
		resLook = &gResolutions[i];
		if (resLook->width != resPlace->width || resLook->height != resPlace->height)
			*++resPlace = *resLook;
	}

	gNumResolutions = (int)(resPlace - &gResolutions[0]) + 1;

	// Update config fullscreen resolution if not set
	if (gConfigGeneral.fullscreen_width == -1 || gConfigGeneral.fullscreen_height == -1) {
		gConfigGeneral.fullscreen_width = gResolutions[gNumResolutions - 1].width;
		gConfigGeneral.fullscreen_height = gResolutions[gNumResolutions - 1].height;
	}
}

void platform_get_closest_resolution(int inWidth, int inHeight, int *outWidth, int *outHeight)
{
	int i, destinationArea, areaDiff, closestAreaDiff, closestWidth, closestHeight;

	closestAreaDiff = -1;
	destinationArea = inWidth * inHeight;
	for (i = 0; i < gNumResolutions; i++) {
		// Check if exact match
		if (gResolutions[i].width == inWidth && gResolutions[i].height == inHeight) {
			closestWidth = gResolutions[i].width;
			closestHeight = gResolutions[i].height;
			closestAreaDiff = 0;
			break;
		}

		// Check if area is closer to best match
		areaDiff = abs((gResolutions[i].width * gResolutions[i].height) - destinationArea);
		if (closestAreaDiff == -1 || areaDiff < closestAreaDiff) {
			closestAreaDiff = areaDiff;
			closestWidth = gResolutions[i].width;
			closestHeight = gResolutions[i].height;
		}
	}

	if (closestAreaDiff != -1) {
		*outWidth = closestWidth;
		*outHeight = closestHeight;
	} else {
		*outWidth = 640;
		*outHeight = 480;
	}
}

void platform_draw()
{
	// Lock the surface before setting its pixels
	if (SDL_MUSTLOCK(_surface))
		if (SDL_LockSurface(_surface) < 0) {
			RCT2_ERROR("locking failed %s", SDL_GetError());
			return;
		}

	// Copy pixels from the virtual screen buffer to the surface
	memcpy(_surface->pixels, _screenBuffer, _surface->pitch * _surface->h);

	// Unlock the surface
	if (SDL_MUSTLOCK(_surface))
		SDL_UnlockSurface(_surface);

	// Copy the surface to the window
	if (SDL_BlitSurface(_surface, NULL, SDL_GetWindowSurface(gWindow), NULL)) {
		RCT2_ERROR("SDL_BlitSurface %s", SDL_GetError());
		exit(1);
	}
	if (SDL_UpdateWindowSurface(gWindow)) {
		RCT2_ERROR("SDL_UpdateWindowSurface %s", SDL_GetError());
		exit(1);
	}
}

static void platform_resize(int width, int height)
{
	rct_drawpixelinfo *screenDPI;
	int newScreenBufferSize;
	void *newScreenBuffer;
	uint32 flags;

	if (_surface != NULL)
		SDL_FreeSurface(_surface);
	if (_palette != NULL)
		SDL_FreePalette(_palette);

	_surface = SDL_CreateRGBSurface(0, width, height, 8, 0, 0, 0, 0);
	_palette = SDL_AllocPalette(256);

	if (!_surface || !_palette) {
		RCT2_ERROR("%p || %p == NULL %s", _surface, _palette, SDL_GetError());
		exit(-1);
	}

	if (SDL_SetSurfacePalette(_surface, _palette)) {
		RCT2_ERROR("SDL_SetSurfacePalette failed %s", SDL_GetError());
		exit(-1);
	}

	newScreenBufferSize = _surface->pitch * _surface->h;
	newScreenBuffer = malloc(newScreenBufferSize);
	if (_screenBuffer == NULL) {
		memset(newScreenBuffer, 0, newScreenBufferSize);
	} else {
		memcpy(newScreenBuffer, _screenBuffer, min(_screenBufferSize, newScreenBufferSize));
		if (newScreenBufferSize - _screenBufferSize > 0)
			memset((uint8*)newScreenBuffer + _screenBufferSize, 0, newScreenBufferSize - _screenBufferSize);
		free(_screenBuffer);
	}

	_screenBuffer = newScreenBuffer;
	_screenBufferSize = newScreenBufferSize;

	RCT2_GLOBAL(RCT2_ADDRESS_SCREEN_WIDTH, sint16) = width;
	RCT2_GLOBAL(RCT2_ADDRESS_SCREEN_HEIGHT, sint16) = height;

	screenDPI = RCT2_ADDRESS(RCT2_ADDRESS_SCREEN_DPI, rct_drawpixelinfo);
	screenDPI->bits = _screenBuffer;
	screenDPI->x = 0;
	screenDPI->y = 0;
	screenDPI->width = width;
	screenDPI->height = height;
	screenDPI->pitch = _surface->pitch - _surface->w;

	RCT2_GLOBAL(0x009ABDF0, uint8) = 6;
	RCT2_GLOBAL(0x009ABDF1, uint8) = 3;
	RCT2_GLOBAL(0x009ABDF2, uint8) = 1;
	RCT2_GLOBAL(RCT2_ADDRESS_DIRTY_BLOCK_WIDTH, sint16) = 64;
	RCT2_GLOBAL(RCT2_ADDRESS_DIRTY_BLOCK_HEIGHT, sint16) = 8;
	RCT2_GLOBAL(RCT2_ADDRESS_DIRTY_BLOCK_COLUMNS, sint32) = (width >> 6) + 1;
	RCT2_GLOBAL(RCT2_ADDRESS_DIRTY_BLOCK_ROWS, sint32) = (height >> 3) + 1;

	window_resize_gui(width, height);
	window_relocate_windows(width, height);

	gfx_invalidate_screen();

	// Check if the window has been resized in windowed mode and update the config file accordingly
	// This is called in rct2_update_2 and is only called after resizing a window has finished
	flags = SDL_GetWindowFlags(gWindow);
	if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED |
		SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) == 0) {
		if (width != gConfigGeneral.window_width || height != gConfigGeneral.window_height) {
			gConfigGeneral.window_width = width;
			gConfigGeneral.window_height = height;
			config_save_default();
		}
	}
}

void platform_update_palette(char* colours, int start_index, int num_colours)
{
	SDL_Color base[256];
	SDL_Surface *surface;
	int i;

	surface = SDL_GetWindowSurface(gWindow);
	if (!surface) {
		RCT2_ERROR("SDL_GetWindowSurface failed %s", SDL_GetError());
		exit(1);
	}

	for (i = 0; i < 256; i++) {
		base[i].r = colours[2];
		base[i].g = colours[1];
		base[i].b = colours[0];
		base[i].a = 0;
		colours += 4;
	}

	if (SDL_SetPaletteColors(_palette, base, 0, 256)) {
		RCT2_ERROR("SDL_SetPaletteColors failed %s", SDL_GetError());
		exit(1);
	}
}

void platform_process_messages()
{
	SDL_Event e;

	gLastKeyPressed = 0;
	// gCursorState.wheel = 0;
	gCursorState.left &= ~CURSOR_CHANGED;
	gCursorState.middle &= ~CURSOR_CHANGED;
	gCursorState.right &= ~CURSOR_CHANGED;
	gCursorState.old = 0;

	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
// 			rct2_finish();
			rct2_quit();
			break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
				platform_resize(e.window.data1, e.window.data2);
			break;
		case SDL_MOUSEMOTION:
			RCT2_GLOBAL(0x0142406C, int) = e.motion.x;
			RCT2_GLOBAL(0x01424070, int) = e.motion.y;

			gCursorState.x = e.motion.x;
			gCursorState.y = e.motion.y;
			break;
		case SDL_MOUSEWHEEL:
			gCursorState.wheel += e.wheel.y * 128;
			break;
		case SDL_MOUSEBUTTONDOWN:
			RCT2_GLOBAL(0x01424318, int) = e.button.x;
			RCT2_GLOBAL(0x0142431C, int) = e.button.y;
			switch (e.button.button) {
			case SDL_BUTTON_LEFT:
				store_mouse_input(1);
				gCursorState.left = CURSOR_PRESSED;
				gCursorState.old = 1;
				break;
			case SDL_BUTTON_MIDDLE:
				gCursorState.middle = CURSOR_PRESSED;
				break;
			case SDL_BUTTON_RIGHT:
				store_mouse_input(3);
				gCursorState.right = CURSOR_PRESSED;
				gCursorState.old = 2;
				break;
			}
			break;
		case SDL_MOUSEBUTTONUP:
			RCT2_GLOBAL(0x01424318, int) = e.button.x;
			RCT2_GLOBAL(0x0142431C, int) = e.button.y;
			switch (e.button.button) {
			case SDL_BUTTON_LEFT:
				store_mouse_input(2);
				gCursorState.left = CURSOR_RELEASED;
				gCursorState.old = 3;
				break;
			case SDL_BUTTON_MIDDLE:
				gCursorState.middle = CURSOR_RELEASED;
				break;
			case SDL_BUTTON_RIGHT:
				store_mouse_input(4);
				gCursorState.right = CURSOR_RELEASED;
				gCursorState.old = 4;
				break;
			}
			break;
		case SDL_KEYDOWN:
			if (e.key.keysym.sym == SDLK_KP_ENTER){
				// Map Keypad enter to regular enter.
				e.key.keysym.scancode = SDL_SCANCODE_RETURN;
			}

			gLastKeyPressed = e.key.keysym.sym;
			gKeysPressed[e.key.keysym.scancode] = 1;
			if (e.key.keysym.sym == SDLK_RETURN && e.key.keysym.mod & KMOD_ALT) {
				int targetMode = gConfigGeneral.fullscreen_mode == 0 ? 2 : 0;
				platform_set_fullscreen_mode(targetMode);
				gConfigGeneral.fullscreen_mode = targetMode;
				config_save_default();
				break;
			}

			// Text input

			// If backspace and we have input text with a cursor position none zero
			if (e.key.keysym.sym == SDLK_BACKSPACE && gTextInputLength > 0 && gTextInput && gTextInputCursorPosition){
				// When at max length don't shift the data left
				// as it would buffer overflow.
				if (gTextInputCursorPosition != gTextInputMaxLength)
					memmove(gTextInput + gTextInputCursorPosition - 1, gTextInput + gTextInputCursorPosition, gTextInputMaxLength - gTextInputCursorPosition - 1);
				gTextInput[gTextInputLength - 1] = '\0';
				gTextInputCursorPosition--;
				gTextInputLength--;
			}
			if (e.key.keysym.sym == SDLK_END){
				gTextInputCursorPosition = gTextInputLength;
			}
			if (e.key.keysym.sym == SDLK_HOME){
				gTextInputCursorPosition = 0;
			}
			if (e.key.keysym.sym == SDLK_DELETE && gTextInputLength > 0 && gTextInput && gTextInputCursorPosition != gTextInputLength){
				memmove(gTextInput + gTextInputCursorPosition, gTextInput + gTextInputCursorPosition + 1, gTextInputMaxLength - gTextInputCursorPosition - 1);
				gTextInput[gTextInputMaxLength - 1] = '\0';
				gTextInputLength--;
			}
			if (e.key.keysym.sym == SDLK_LEFT && gTextInput){
				if (gTextInputCursorPosition) gTextInputCursorPosition--;
			}
			else if (e.key.keysym.sym == SDLK_RIGHT && gTextInput){
				if (gTextInputCursorPosition < gTextInputLength) gTextInputCursorPosition++;
			}
			break;
		case SDL_MULTIGESTURE:
			if (e.mgesture.numFingers == 2) {
				if (e.mgesture.timestamp > _lastGestureTimestamp + 1000)
					_gestureRadius = 0;
				_lastGestureTimestamp = e.mgesture.timestamp;
				_gestureRadius += e.mgesture.dDist;

				// Zoom gesture
				const int tolerance = 128;
				int gesturePixels = (int)(_gestureRadius * RCT2_GLOBAL(RCT2_ADDRESS_SCREEN_WIDTH, sint16));
				if (gesturePixels > tolerance) {
					_gestureRadius = 0;
					keyboard_shortcut_handle_command(SHORTCUT_ZOOM_VIEW_IN);
				} else if (gesturePixels < -tolerance) {
					_gestureRadius = 0;
					keyboard_shortcut_handle_command(SHORTCUT_ZOOM_VIEW_OUT);
				}
			}
			break;

		case SDL_TEXTINPUT:
			if (gTextInputLength < gTextInputMaxLength && gTextInput){
				// Convert the utf-8 code into rct ascii
				char new_char;
				if (!(e.text.text[0] & 0x80))
					new_char = *e.text.text;
				else if (!(e.text.text[0] & 0x20))
					new_char = ((e.text.text[0] & 0x1F) << 6) | (e.text.text[1] & 0x3F);

				// If inserting in center of string make space for new letter
				if (gTextInputLength > gTextInputCursorPosition){
					memmove(gTextInput + gTextInputCursorPosition + 1, gTextInput + gTextInputCursorPosition, gTextInputMaxLength - gTextInputCursorPosition - 1);
					gTextInput[gTextInputCursorPosition] = new_char;
					gTextInputLength++;
				}
				else gTextInput[gTextInputLength++] = new_char;

				gTextInputCursorPosition++;
			}
			break;
		default:
			break;
		}
	}

	gCursorState.any = gCursorState.left | gCursorState.middle | gCursorState.right;

	// Updates the state of the keys
	int numKeys = 256;
	gKeysState = SDL_GetKeyboardState(&numKeys);
}

static void platform_close_window()
{
	if (gWindow != NULL)
		SDL_DestroyWindow(gWindow);
	if (_surface != NULL)
		SDL_FreeSurface(_surface);
	if (_palette != NULL)
		SDL_FreePalette(_palette);
	platform_unload_cursors();
}

void platform_init()
{
	platform_create_window();
	gKeysPressed = malloc(sizeof(unsigned char) * 256);
	memset(gKeysPressed, 0, sizeof(unsigned char) * 256);
	// RCT2_CALLPROC(0x00404584); // dinput_init()
}

static void platform_create_window()
{
	int width, height;

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		RCT2_ERROR("SDL_Init %s", SDL_GetError());
		exit(-1);
	}

	platform_load_cursors();
	RCT2_CALLPROC_EBPSAFE(0x0068371D);

	// Get window size
	width = gConfigGeneral.window_width;
	height = gConfigGeneral.window_height;
	if (width == -1) width = 640;
	if (height == -1) height = 480;

	RCT2_GLOBAL(0x009E2D8C, sint32) = 0;

	// Create window in window first rather than fullscreen so we have the display the window is on first
	gWindow = SDL_CreateWindow(
		"OpenRCT2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_RESIZABLE
	);
	if (!gWindow) {
		log_fatal("SDL_CreateWindow failed %s", SDL_GetError());
		exit(-1);
	}

	// Set the update palette function pointer
	RCT2_GLOBAL(0x009E2BE4, update_palette_func) = platform_update_palette;

	// Initialise the surface, palette and draw buffer
	platform_resize(width, height);

	platform_update_fullscreen_resolutions();
	platform_set_fullscreen_mode(gConfigGeneral.fullscreen_mode);
}

int platform_scancode_to_rct_keycode(int sdl_key)
{
	char keycode = (char)SDL_GetKeyFromScancode((SDL_Scancode)sdl_key);

	// Until we reshufle the text files to use the new positions 
	// this will suffice to move the majority to the correct positions.
	// Note any special buttons PgUp PgDwn are mapped wrong.
	if (keycode >= 'a' && keycode <= 'z')
		keycode = toupper(keycode);

	return keycode;
}

void platform_free()
{
	free(gKeysPressed);

	platform_close_window();
	SDL_Quit();
}

void platform_start_text_input(char* buffer, int max_length)
{
	SDL_StartTextInput();
	gTextInputMaxLength = max_length - 1;
	gTextInput = buffer;
	gTextInputCursorPosition = strnlen(gTextInput, max_length);
	gTextInputLength = gTextInputCursorPosition;
}

void platform_stop_text_input()
{
	SDL_StopTextInput();
	gTextInput = NULL;
}

static void platform_unload_cursors()
{
	for (int i = 0; i < CURSOR_COUNT; i++)
		if (_cursors[i] != NULL)
			SDL_FreeCursor(_cursors[i]);
}

void platform_set_fullscreen_mode(int mode)
{
	int width, height;

	mode = _fullscreen_modes[mode];

	// HACK Changing window size when in fullscreen usually has no effect
	if (mode == SDL_WINDOW_FULLSCREEN)
		SDL_SetWindowFullscreen(gWindow, 0);

	// Set window size
	if (mode == SDL_WINDOW_FULLSCREEN) {
		platform_update_fullscreen_resolutions();
		platform_get_closest_resolution(gConfigGeneral.fullscreen_width, gConfigGeneral.fullscreen_height, &width, &height);
		SDL_SetWindowSize(gWindow, width, height);
	} else if (mode == 0) {
		SDL_SetWindowSize(gWindow, gConfigGeneral.window_width, gConfigGeneral.window_height);
	}

	if (SDL_SetWindowFullscreen(gWindow, mode)) {
		log_fatal("SDL_SetWindowFullscreen %s", SDL_GetError());
		exit(1);

		// TODO try another display mode rather than just exiting the game
	}
}

/**
 *  This is not quite the same as the below function as we don't want to
 *  derfererence the cursor before the function.
 *  rct2: 0x0407956
 */
void platform_set_cursor(char cursor)
{
	SDL_SetCursor(_cursors[cursor]);
}
/**
 *
 * rct2: 0x0068352C
 */
static void platform_load_cursors()
{
	RCT2_GLOBAL(0x14241BC, uint32) = 2;
	HINSTANCE hInst = RCT2_GLOBAL(RCT2_ADDRESS_HINSTANCE, HINSTANCE);
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_ARROW,				HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x74));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_BLANK,				HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0xA1));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_UP_ARROW,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x6D));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_UP_DOWN_ARROW,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x6E));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_HAND_POINT,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x70));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_ZZZ,				HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x78));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_DIAGONAL_ARROWS,	HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x77));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_PICKER,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x7C));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_TREE_DOWN,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x83));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_FOUNTAIN_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x7F));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_STATUE_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x80));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_BENCH_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x81));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_CROSS_HAIR,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x82));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_BIN_DOWN,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x84));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_LAMPPOST_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x85));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_FENCE_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x8A));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_FLOWER_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x89));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_PATH_DOWN,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x8B));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_DIG_DOWN,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x8D));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_WATER_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x8E));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_HOUSE_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x8F));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_VOLCANO_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x90));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_WALK_DOWN,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x91));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_PAINT_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x9E));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_ENTRANCE_DOWN,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0x9F));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_HAND_OPEN,			HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0xA6));
	RCT2_GLOBAL(RCT2_ADDRESS_HCURSOR_HAND_CLOSED,		HCURSOR) = LoadCursor(hInst, MAKEINTRESOURCE(0xA5));

	_cursors[0] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
	_cursors[1] = SDL_CreateCursor(blank_cursor_data, blank_cursor_mask, 32, 32, BLANK_CURSOR_HOTX, BLANK_CURSOR_HOTY);
	_cursors[2] = SDL_CreateCursor(up_arrow_cursor_data, up_arrow_cursor_mask, 32, 32, UP_ARROW_CURSOR_HOTX, UP_ARROW_CURSOR_HOTY);
	_cursors[3] = SDL_CreateCursor(up_down_arrow_cursor_data, up_down_arrow_cursor_mask, 32, 32, UP_DOWN_ARROW_CURSOR_HOTX, UP_DOWN_ARROW_CURSOR_HOTY);
	_cursors[4] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
	_cursors[5] = SDL_CreateCursor(zzz_cursor_data, zzz_cursor_mask, 32, 32, ZZZ_CURSOR_HOTX, ZZZ_CURSOR_HOTY);
	_cursors[6] = SDL_CreateCursor(diagonal_arrow_cursor_data, diagonal_arrow_cursor_mask, 32, 32, DIAGONAL_ARROW_CURSOR_HOTX, DIAGONAL_ARROW_CURSOR_HOTY);
	_cursors[7] = SDL_CreateCursor(picker_cursor_data, picker_cursor_mask, 32, 32, PICKER_CURSOR_HOTX, PICKER_CURSOR_HOTY);
	_cursors[8] = SDL_CreateCursor(tree_down_cursor_data, tree_down_cursor_mask, 32, 32, TREE_DOWN_CURSOR_HOTX, TREE_DOWN_CURSOR_HOTY);
	_cursors[9] = SDL_CreateCursor(fountain_down_cursor_data, fountain_down_cursor_mask, 32, 32, FOUNTAIN_DOWN_CURSOR_HOTX, FOUNTAIN_DOWN_CURSOR_HOTY);
	_cursors[10] = SDL_CreateCursor(statue_down_cursor_data, statue_down_cursor_mask, 32, 32, STATUE_DOWN_CURSOR_HOTX, STATUE_DOWN_CURSOR_HOTY);
	_cursors[11] = SDL_CreateCursor(bench_down_cursor_data, bench_down_cursor_mask, 32, 32, BENCH_DOWN_CURSOR_HOTX, BENCH_DOWN_CURSOR_HOTY);
	_cursors[12] = SDL_CreateCursor(cross_hair_cursor_data, cross_hair_cursor_mask, 32, 32, CROSS_HAIR_CURSOR_HOTX, CROSS_HAIR_CURSOR_HOTY);
	_cursors[13] = SDL_CreateCursor(bin_down_cursor_data, bin_down_cursor_mask, 32, 32, BIN_DOWN_CURSOR_HOTX, BIN_DOWN_CURSOR_HOTY);
	_cursors[14] = SDL_CreateCursor(lamppost_down_cursor_data, lamppost_down_cursor_mask, 32, 32, LAMPPOST_DOWN_CURSOR_HOTX, LAMPPOST_DOWN_CURSOR_HOTY);
	_cursors[15] = SDL_CreateCursor(fence_down_cursor_data, fence_down_cursor_mask, 32, 32, FENCE_DOWN_CURSOR_HOTX, FENCE_DOWN_CURSOR_HOTY);
	_cursors[16] = SDL_CreateCursor(flower_down_cursor_data, flower_down_cursor_mask, 32, 32, FLOWER_DOWN_CURSOR_HOTX, FLOWER_DOWN_CURSOR_HOTY);
	_cursors[17] = SDL_CreateCursor(path_down_cursor_data, path_down_cursor_mask, 32, 32, PATH_DOWN_CURSOR_HOTX, PATH_DOWN_CURSOR_HOTY);
	_cursors[18] = SDL_CreateCursor(dig_down_cursor_data, dig_down_cursor_mask, 32, 32, DIG_DOWN_CURSOR_HOTX, DIG_DOWN_CURSOR_HOTY);
	_cursors[19] = SDL_CreateCursor(water_down_cursor_data, water_down_cursor_mask, 32, 32, WATER_DOWN_CURSOR_HOTX, WATER_DOWN_CURSOR_HOTY);
	_cursors[20] = SDL_CreateCursor(house_down_cursor_data, house_down_cursor_mask, 32, 32, HOUSE_DOWN_CURSOR_HOTX, HOUSE_DOWN_CURSOR_HOTY);
	_cursors[21] = SDL_CreateCursor(volcano_down_cursor_data, volcano_down_cursor_mask, 32, 32, VOLCANO_DOWN_CURSOR_HOTX, VOLCANO_DOWN_CURSOR_HOTY);
	_cursors[22] = SDL_CreateCursor(walk_down_cursor_data, walk_down_cursor_mask, 32, 32, WALK_DOWN_CURSOR_HOTX, WALK_DOWN_CURSOR_HOTY);
	_cursors[23] = SDL_CreateCursor(paint_down_cursor_data, paint_down_cursor_mask, 32, 32, PAINT_DOWN_CURSOR_HOTX, PAINT_DOWN_CURSOR_HOTY);
	_cursors[24] = SDL_CreateCursor(entrance_down_cursor_data, entrance_down_cursor_mask, 32, 32, ENTRANCE_DOWN_CURSOR_HOTX, ENTRANCE_DOWN_CURSOR_HOTY);
	_cursors[25] = SDL_CreateCursor(hand_open_cursor_data, hand_open_cursor_mask, 32, 32, HAND_OPEN_CURSOR_HOTX, HAND_OPEN_CURSOR_HOTY);
	_cursors[26] = SDL_CreateCursor(hand_closed_cursor_data, hand_closed_cursor_mask, 32, 32, HAND_CLOSED_CURSOR_HOTX, HAND_CLOSED_CURSOR_HOTY);
	platform_set_cursor(CURSOR_ARROW);
	RCT2_GLOBAL(0x14241BC, uint32) = 0;
}

/**
 * 
 *  rct2: 0x00407D80
 */
int platform_get_cursor_pos(int* x, int* y)
{
	POINT point;
	GetCursorPos(&point);
	*x = point.x;
	*y = point.y;
}