# GifFace

GifFace is a lightweight Win32 + GDI+ desktop overlay that displays an animated GIF as a click-through, bouncing face on your screen.

- Click-through (does not block mouse input)
- No taskbar or Alt-Tab entry
- Always on top
- Bounces around the screen
- Loads GIFs from a HTTP URL
- Remote trigger through HTTP
- Basic registry Run key persistance

Built as a single `.cpp` file and compiled directly with `cl.exe`:
```
cl /EHsc /std:c++17 GifFace.cpp user32.lib gdi32.lib gdiplus.lib wininet.lib advapi32.lib
```

**Exit:** `Ctrl + Alt + Q`
