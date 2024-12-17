#include <windows.h>
#include <stdio.h>
#include <ctype.h>
#include <shlwapi.h>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shlwapi.lib")

// Maintaining our own key states bookkeeping is kinda cringe
// but we can't really use Get[Async]KeyState, see the first note at
// https://docs.microsoft.com/en-us/previous-versions/windows/desktop/legacy/ms644985(v=vs.85)
#define DIRECTION_LEFT 0
#define DIRECTION_RIGHT 1
#define DIRECTION_UP 2
#define DIRECTION_DOWN 3
#define IS_DOWN 1
#define IS_UP 0
#define whitelist_max_length 200
#define WASD_ID 100
#define ARROWS_ID 200
#define CUSTOM_ID 300
#define ESC_BIND_ID 400

// This is scuffed. ESC is a scan code, everything else is virtual key codes
#define KEY_ESC 1
#define KEY_W 0x4C
#define KEY_A 0xBC
#define KEY_S 0xBE
#define KEY_D 0xBF
#define KEY_E 0x45
#define KEY_UP VK_UP
#define KEY_LEFT VK_LEFT
#define KEY_DOWN VK_DOWN
#define KEY_RIGHT VK_RIGHT

HWND main_window;

const char* CONFIG_NAME = "socd_v008.conf";
const LPCWSTR CLASS_NAME = L"SOCD_CLASS";
char config_line[100];
char focused_program[MAX_PATH];
char programs_whitelist[whitelist_max_length][MAX_PATH] = {0};

HHOOK kbhook;
int hook_is_installed = 0;
int listening_for_esc_bind = 0;

int real[4]; // whether the key is pressed for real on keyboard
int virtual[4]; // whether the key is pressed on a software level
int DEFUALT_DISABLE_BIND = KEY_E;
int WASD[4] = {KEY_A, KEY_D, KEY_W, KEY_S};
int ARROWS[4] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
// left, right, up, down
int CUSTOM_BINDS[4];
int DISABLE_BIND;
int disableKeyPressed;
int ESC_BIND = 0;
int ESC_PRESSED;

int show_error_and_quit(char* text) {
    int error = GetLastError();
    // error is dword which is 32 bits, so 10 characters should be enough considering that we have an extra %d in the string
    char* error_buffer = malloc(strlen(text) + 10);
    sprintf(error_buffer, text, error);
    MessageBox(
        NULL,
        error_buffer,
        "RIP",
        MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

void paint_esc_label(lParam) {
    LPCWSTR label;
    if (listening_for_esc_bind) {
        label = L"Press button to bind ESC to...";
    } else {
        if (!ESC_BIND) {
            label = L"ESC isn't bound";
        } else {
            wchar_t label_buffer[100];
            wchar_t key_name_buf[15];
            GetKeyNameTextW(lParam, key_name_buf, 15);
            wprintf(L"key name is %s\nvirtual key is %X", key_name_buf, ESC_BIND);
            wsprintfW(label_buffer, L"ESC is bound to %s", key_name_buf);
            label = label_buffer;
        }
    }
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        label,
        WS_VISIBLE | WS_CHILD,
        120,
        167,
        400,
        30,
        main_window,
        (HMENU)500,
        (HINSTANCE)main_window,
        NULL);
    if (hwnd == NULL) {
        show_error_and_quit("Failed to create ESC bind label, error code is %d");
    }
    
}


void write_settings(int* bindings, int disableBind, int esc_bind) {
    FILE* config_file = fopen(CONFIG_NAME, "w");
    if (config_file == NULL) {
        // This writes to console that we're freeing sigh
        // Probably better to show MessageBox
        perror("Couldn't open the config file");
        return;
    }
    for (int i = 0; i < 4; i++) {
        fprintf(config_file, "%X\n", bindings[i]);
    }
    fprintf(config_file, "%X\n", disableBind);
    fprintf(config_file, "%X\n", esc_bind);
    for (int i = 0; i < whitelist_max_length; i++) {
        if (programs_whitelist[i][0] == '\0') {
            break;
        }
        fprintf(config_file, "%s\n", programs_whitelist[i]);
    }
    fclose(config_file);
}

void set_bindings(int* bindings, int disableBind, int esc_bind) {
    CUSTOM_BINDS[0] = bindings[0];
    CUSTOM_BINDS[1] = bindings[1];
    CUSTOM_BINDS[2] = bindings[2];
    CUSTOM_BINDS[3] = bindings[3];
    DISABLE_BIND = disableBind; 
    ESC_BIND = esc_bind;
}

void read_settings() {
    FILE* config_file = fopen(CONFIG_NAME, "r+");
    if (config_file == NULL) {
        set_bindings(WASD, DEFUALT_DISABLE_BIND, ESC_BIND);
        write_settings(WASD, DEFUALT_DISABLE_BIND, ESC_BIND);
        return;
    }
    
    // First 4 lines are key bindings
    for (int i=0; i < 4; i++) {
        char* result = fgets(config_line, 100, config_file);
        int button = (int)strtol(result, NULL, 16);
        CUSTOM_BINDS[i] = button;
    }

    // 5th line is disable key bind
    char* result = fgets(config_line,100,config_file);
    DISABLE_BIND = (int)strtol(result,NULL,16);

    // 6th line is ESC key bind
    result = fgets(config_line,100,config_file);
    ESC_BIND = (int)strtol(result,NULL,16);

    // Then there are programs SOCD cleaner should track
    int i = 0;
    while (fgets(programs_whitelist[i], MAX_PATH, config_file) != NULL) {
        // Remove line ends from the line we just read.
        // Works for LF, CR, CRLF, LFCR etc
        programs_whitelist[i][strcspn(programs_whitelist[i], "\r\n")] = 0;
        i++;
    }
    for (int i = 0; i < whitelist_max_length; i++) {
        if (programs_whitelist[i][0] == '\0') {
            break;
        }
    }
    fclose(config_file);
}

int find_opposing_key(int key) {
    if (key == CUSTOM_BINDS[DIRECTION_LEFT]) {
        return CUSTOM_BINDS[DIRECTION_RIGHT];
    }
    if (key == CUSTOM_BINDS[DIRECTION_RIGHT]) {
        return CUSTOM_BINDS[DIRECTION_LEFT];
    }
    if (key == CUSTOM_BINDS[DIRECTION_UP]) {
        return CUSTOM_BINDS[DIRECTION_DOWN];
    }
    if (key == CUSTOM_BINDS[DIRECTION_DOWN]) {
        return CUSTOM_BINDS[DIRECTION_UP];
    }
    return -1;
}

int find_index_by_key(int key) {
    if (key == CUSTOM_BINDS[DIRECTION_LEFT]) {
        return DIRECTION_LEFT;
    }
    if (key == CUSTOM_BINDS[DIRECTION_RIGHT]) {
        return DIRECTION_RIGHT;
    }
    if (key == CUSTOM_BINDS[DIRECTION_UP]) {
        return DIRECTION_UP;
    }
    if (key == CUSTOM_BINDS[DIRECTION_DOWN]) {
        return DIRECTION_DOWN;
    }
    return -1;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    KBDLLHOOKSTRUCT* kbInput = (KBDLLHOOKSTRUCT*)lParam;
    
    // We ignore injected events so we don't mess with the inputs
    // we inject ourselves with SendInput.
    // TODO maybe it's better to set our own custom extended info and check it
    // instead of LLKHF_INJECTED so we know FOR SURE it's ours
    if (nCode != HC_ACTION || kbInput->flags & LLKHF_INJECTED) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }
    
    INPUT input;
    int key = kbInput->vkCode;
    if(key == DISABLE_BIND){
        if(wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            disableKeyPressed = IS_DOWN;
        }
        else if(wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
        {
            disableKeyPressed = IS_UP;
        }
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    if(key == ESC_BIND) {
        if(wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            ESC_PRESSED = IS_DOWN;
            input.type = INPUT_KEYBOARD;
            input.ki = (KEYBDINPUT){0, KEY_ESC, KEYEVENTF_SCANCODE, 0, 0};
            SendInput(1, &input, sizeof(INPUT));
        }
        else if(wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
        {
            ESC_PRESSED = IS_UP;
            input.type = INPUT_KEYBOARD;
            input.ki = (KEYBDINPUT){0, KEY_ESC, KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE, 0, 0};
            SendInput(1, &input, sizeof(INPUT));
        }
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    int opposing = find_opposing_key(key);
    if (opposing < 0) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }
    int index = find_index_by_key(key);
    int opposing_index = find_index_by_key(opposing);

    // Translate opposing virtual key code to the scancode before sending them.
    // The reason for this is that a lot of games (mostly old ones and some new ones)
    // Are using DirectInput even loops which only respect scancodes events and fully
    // ignores virtual key codes. Source games (Half-Life2, Portal1/2, VTMB)
    // are among notable examples.
    opposing = MapVirtualKeyW(opposing, MAPVK_VK_TO_VSC_EX);
    
    // Holding Alt sends WM_SYSKEYDOWN/WM_SYSKEYUP
    // instead of WM_KEYDOWN/WM_KEYUP, check it as well
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        real[index] = IS_DOWN;
        virtual[index] = IS_DOWN;
        if (real[opposing_index] == IS_DOWN && virtual[opposing_index] == IS_DOWN && disableKeyPressed == IS_UP) {
            input.type = INPUT_KEYBOARD;
            input.ki = (KEYBDINPUT){0, opposing, KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE, 0, 0};
            SendInput(1, &input, sizeof(INPUT));
            /* printf("Send %d", opposing); */
            virtual[opposing_index] = IS_UP;
        }
    }
    else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        real[index] = IS_UP;
        virtual[index] = IS_UP;
        if (real[opposing_index] == IS_DOWN && disableKeyPressed == IS_UP) {
            input.type = INPUT_KEYBOARD;
            input.ki = (KEYBDINPUT){0, opposing, KEYEVENTF_SCANCODE, 0, 0};
            SendInput(1, &input, sizeof(INPUT));
            /* printf("Send %d", opposing); */
            virtual[opposing_index] = IS_DOWN;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void set_kb_hook(HINSTANCE instance) {
    if (!hook_is_installed) {
        printf("hooking this shit\n");
        kbhook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, instance, 0);
        if (kbhook != NULL) {
            hook_is_installed = 1;
        } else {
            printf("hook failed oopsie\n");
        }
    }
}

void unset_kb_hook() {
    if (hook_is_installed) {
        printf("unhooking this shit\n");
        UnhookWindowsHookEx(kbhook);
        // Forget buttons that are pressed before unhooking from the keyboard
        // So that when we hook again we don't end up in a dirty state where one
        // of the buttons is being stuck from the previous hook
        real[DIRECTION_LEFT] = IS_UP;
        real[DIRECTION_RIGHT] = IS_UP;
        real[DIRECTION_UP] = IS_UP;
        real[DIRECTION_DOWN] = IS_UP;
        virtual[DIRECTION_LEFT] = IS_UP;
        virtual[DIRECTION_RIGHT] = IS_UP;
        virtual[DIRECTION_UP] = IS_UP;
        virtual[DIRECTION_DOWN] = IS_UP;
        hook_is_installed = 0;
    }
}

void get_focused_program() {
    HWND inspected_window = GetForegroundWindow();
    DWORD process_id = 0;
    GetWindowThreadProcessId(inspected_window, &process_id);
    if (process_id== 0) {
        // Sometimes when you minimize a window nothing is focused for a brief moment,
        // in this case windows sends "System Idle Process" as currently focused window
        // for some reason. Just ignore it
        return;
    }
    HANDLE hproc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, 0, process_id);
    if (hproc == NULL) {
        // Sometimes we can't open a process (#5 "access denied"). Ignore it I guess.
        if (GetLastError() == 5) {
            return;
        }
        show_error_and_quit("Couldn't open active process, error code is: %d");
    }
    DWORD filename_size = MAX_PATH;
    // This function API is so fucking weird. Read its docs extremely carefully
    QueryFullProcessImageName(hproc, 0, focused_program, &filename_size);
    CloseHandle(hproc);
    PathStripPath(focused_program);
    printf("Window activated: %s\n", focused_program);
}

void detect_focused_program(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND window,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime
) {
    // We're ignoring window here, sometimes it points to the program that
    // is not actually being focused for some reason. Instead, we treat the event
    // as a trigger to check for the current foreground program ourselves.
    get_focused_program();
    
    HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(NULL);
    // Linear scan let's fucking go, don't look here CS degree people
    for (int i = 0; i < whitelist_max_length; i++) {
        if (strcmp(focused_program, programs_whitelist[i]) == 0) {
            set_kb_hook(hInstance);
            return;
        } else if (programs_whitelist[i][0] == '\0') {
            break;
        }
    }
    unset_kb_hook();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_COMMAND:
        // We reset focus to the main window so that it's possible to bind the buttons
        // to SPACE without it repressing the button and asking for the bind infinitely
        SetFocus(main_window);
        if (wParam == WASD_ID) {
            set_bindings(WASD, DEFUALT_DISABLE_BIND, ESC_BIND);
            write_settings(WASD, DEFUALT_DISABLE_BIND, ESC_BIND);
            return 0;
        } else if (wParam == ARROWS_ID) {
            set_bindings(ARROWS,DEFUALT_DISABLE_BIND, ESC_BIND);
            write_settings(ARROWS,DEFUALT_DISABLE_BIND, ESC_BIND);
            return 0;
        } else if (wParam == ESC_BIND_ID) {
            listening_for_esc_bind = !listening_for_esc_bind;
            paint_esc_label(lParam);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int main() {
    // You can compile with these flags instead of calling to FreeConsole
    // to get rid of a terminal window
    // cl socd_cleaner.c /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
    FreeConsole();

    ESC_PRESSED = IS_UP;
    disableKeyPressed = IS_UP;
    real[DIRECTION_LEFT] = IS_UP;
    real[DIRECTION_RIGHT] = IS_UP;
    real[DIRECTION_UP] = IS_UP;
    real[DIRECTION_DOWN] = IS_UP;
    virtual[DIRECTION_LEFT] = IS_UP;
    virtual[DIRECTION_RIGHT] = IS_UP;
    virtual[DIRECTION_UP] = IS_UP;
    virtual[DIRECTION_DOWN] = IS_UP;

    HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(NULL);
    
    read_settings();
    // Means no allowed programes were set up, just globally set the hook.
    if (programs_whitelist[0][0] == '\0') {
        set_kb_hook(hInstance);
        
        // At least one allowed program is specified,
        // handle hooking/unhooking when program gets focused/unfocused
    } else {
        SetWinEventHook(
            EVENT_OBJECT_FOCUS,
            EVENT_OBJECT_FOCUS,
            hInstance,
            (WINEVENTPROC)detect_focused_program,
            0,
            0,
            WINEVENT_OUTOFCONTEXT
            );
    }
    
    WNDCLASSEXW wc;
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = 0;
    wc.lpfnWndProc = WindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)COLOR_WINDOW,
    wc.lpszMenuName = NULL;
    wc.lpszClassName = CLASS_NAME;
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    
    if (RegisterClassExW(&wc) == 0) {
        show_error_and_quit("Failed to register window class, error code is %d");
    };
    
    main_window = CreateWindowExW(
        0,
        CLASS_NAME,
        L"SOCD helper for Epic Gamers! v008",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        460,
        250,
        NULL,
        NULL,
        hInstance,
        NULL);
    if (main_window == NULL) {
        show_error_and_quit("Failed to create a window, error code is %d");
    }
    
    HWND wasd_hwnd = CreateWindowExW(
        0,
        L"BUTTON",
        L"WASD",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON,
        10,
        90,
        100,
        30,
        main_window,
        (HMENU)WASD_ID,
        hInstance,
        NULL);
    if (wasd_hwnd == NULL) {
        show_error_and_quit("Failed to create WASD radiobutton, error code is %d");
    }
    
    HWND arrows_hwnd = CreateWindowExW(
        0,
        L"BUTTON",
        L"Arrows",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON,
        10,
        110,
        100,
        30,
        main_window,
        (HMENU)ARROWS_ID,
        hInstance,
        NULL);
    if (arrows_hwnd == NULL) {
        show_error_and_quit("Failed to create Arrows radiobutton, error code is %d");
    }
    
    HWND custom_hwnd = CreateWindowExW(
        0,
        L"BUTTON",
        L"Custom",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON,
        10,
        130,
        100,
        30,
        main_window,
        (HMENU)CUSTOM_ID,
        hInstance,
        NULL);
    if (custom_hwnd == NULL) {
        show_error_and_quit("Failed to create Custom radiobutton, error code is %d");
    }

    HWND esc_hwnd = CreateWindowExW(
        0,
        L"BUTTON",
        L"Bind ESC",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        10,
        160,
        100,
        30,
        main_window,
        (HMENU)ESC_BIND_ID,
        hInstance,
        NULL);
    if (esc_hwnd == NULL) {
        show_error_and_quit("Failed to create ESC bind button, error code is %d");
    }
    int code = MapVirtualKeyW(ESC_BIND, MAPVK_VK_TO_VSC_EX);

    /* We transform the scan code of the button to an lParam-style bit sequence that
       paint_esc_label expects.
       MapVirtualKey sets 0xE0 (or 0xE1, but we don't care for now) prefix to the scan code
       if it's for the right handside button (so right ctrl or right alt).
       We test for that and set 24th extended bit to 1 if that's the case */

    int code_lparam = code << 16;
    if (0xE000 & code) {
        code_lparam =  (1 << 24) | code_lparam;
    }
    paint_esc_label(code_lparam);
    
    int check_id;
    if (memcmp(CUSTOM_BINDS, WASD, sizeof(WASD)) == 0) {
        check_id = WASD_ID;
    } else if (memcmp(CUSTOM_BINDS, ARROWS, sizeof(ARROWS)) == 0) {
        check_id = ARROWS_ID;
    } else {
        check_id = CUSTOM_ID;
    }
    if (CheckRadioButton(main_window, WASD_ID, CUSTOM_ID, check_id) == 0) {
        show_error_and_quit("Failed to select default keybindings, error code is %d");
    }
    
    HWND text_hwnd = CreateWindowExW(
        0,
        L"STATIC",
        (L"\"Last Wins\" is the only mode available as of now.\n"
         L"I'll add custom binding interface later. "
         L"For now, you can set them in socd.conf in the first 4 rows. "
         L"The order is left, right, up, down."),
        WS_VISIBLE | WS_CHILD,
        10,
        10,
        400,
        80,
        main_window,
        (HMENU)100,
        hInstance,
        NULL);
    if (text_hwnd == NULL) {
        show_error_and_quit("Failed to create Text, error code is %d");
    }
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        switch (msg.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (listening_for_esc_bind) {
                UINT real_virtual_key_code = msg.wParam;
                listening_for_esc_bind = 0;
                ESC_BIND = 0;

                // key scancode is 16-23 bits of lParam
                UINT scancode = (msg.lParam & 0x00ff0000) >> 16;
                // extended is 24th bit of lParam. Only works for ctrl and alt
                int extended = msg.lParam >> 24 & 1;

                // Windows is weird when it comes to distinguishing between left and right counterparts of
                // shift, ctrl and alt keys. Extended bit of lParam only works for alt and control but doesn't work for shift.
                // MapVirtualKeyW only works for shift but doesn't work for alt and ctrl. So we should use both
                // Funny enough, left/right windows key work just fine out of the box without any special treatment.
                switch (real_virtual_key_code) {
                case VK_SHIFT:
                    real_virtual_key_code = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
                    break;
                case VK_CONTROL:
                    real_virtual_key_code = extended ? VK_RCONTROL : VK_LCONTROL;
                    break;
                case VK_MENU:
                    real_virtual_key_code = extended ? VK_RMENU : VK_LMENU;
                    break;
                }
                
                if (real_virtual_key_code != VK_ESCAPE) {
                    ESC_BIND = real_virtual_key_code;
                    write_settings(CUSTOM_BINDS, DISABLE_BIND, ESC_BIND);
                }
                paint_esc_label(msg.lParam);
            }
        }
    }
    return 0;
}
