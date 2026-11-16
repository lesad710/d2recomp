// Shared between the window (user.c) and the renderer (glide.c).
#pragma once
typedef struct {
    int w, h;    // the game's resolution (640x480 / 800x600)
    int dw, dh;  // the picture's size in the window
    int ox, oy;  // its offset (letterboxing)
} GameView;
extern GameView g_view;
extern unsigned g_main_hwnd;
void post_msg(unsigned hwnd, unsigned msg, unsigned wp, unsigned lp);
void input_mouse(int gx, int gy, int button, int down);  // button 0 = move, 1 left, 2 right, 3 middle
void input_key(int vk, int down);
void input_char(unsigned short ch);
