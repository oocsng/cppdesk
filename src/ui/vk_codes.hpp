#pragma once

// Windows virtual-key codes. The screens and the wire protocol both use these values for keys, so every
// platform's shell translates its native key codes into them. Defined only where <windows.h> has not.

#ifndef VK_LBUTTON
#define VK_LBUTTON 0x01
#endif
#ifndef VK_TAB
#define VK_TAB 0x09
#endif
#ifndef VK_RETURN
#define VK_RETURN 0x0D
#endif
#ifndef VK_SHIFT
#define VK_SHIFT 0x10
#endif
#ifndef VK_CONTROL
#define VK_CONTROL 0x11
#endif
#ifndef VK_MENU
#define VK_MENU 0x12
#endif
#ifndef VK_ESCAPE
#define VK_ESCAPE 0x1B
#endif
#ifndef VK_PRIOR
#define VK_PRIOR 0x21
#endif
#ifndef VK_NEXT
#define VK_NEXT 0x22
#endif
#ifndef VK_END
#define VK_END 0x23
#endif
#ifndef VK_HOME
#define VK_HOME 0x24
#endif
#ifndef VK_UP
#define VK_UP 0x26
#endif
#ifndef VK_DOWN
#define VK_DOWN 0x28
#endif
#ifndef VK_DELETE
#define VK_DELETE 0x2E
#endif
#ifndef VK_F1
#define VK_F1 0x70
#endif
#ifndef VK_F8
#define VK_F8 0x77
#endif
#ifndef VK_F11
#define VK_F11 0x7A
#endif
