#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "src/PluginAPI.h"
#include "src/Entity.h"
#include "src/Window.h"
#include "src/Game.h"
#include "src/Event.h"
#include "src/String_.h"
#include "src/Commands.h"
#include "src/Graphics.h"
#include "src/Drawer2D.h"
#include "src/PackedCol.h"
#include "src/Chat.h"
#include "src/ExtMath.h"
#include "src/Gui.h"
#include "src/Core.h"
#include <string.h>
#include <stdint.h>

#define MAX_TEXT_ITEMS 16
#define HOOK_SIZE 12

struct DynamicText {
    cc_bool active;
    int id;
    char textBuffer[128];
    cc_string text;
    float x, y, scale, angle;
};

static struct DynamicText textList[MAX_TEXT_ITEMS];
static GfxResourceID text_vb = 0;

typedef void (*Chat_AddOf_Fn)(const cc_string* text, int msgType);
static Chat_AddOf_Fn Original_Chat_AddOf = NULL;
static unsigned char OriginalBytes[HOOK_SIZE];
static void* Trampoline = NULL;
static volatile LONG HookInstalled = 0;

static void Chat_AddStatic(const char* text) {
    cc_string str = String_FromReadonly(text);
    Chat_Add(&str);
}

static struct DynamicText* FindOrCreateText(int id) {
    int i;
    for (i = 0; i < MAX_TEXT_ITEMS; i++) {
        if (textList[i].active && textList[i].id == id) return &textList[i];
    }
    for (i = 0; i < MAX_TEXT_ITEMS; i++) {
        if (!textList[i].active) {
            struct DynamicText* item = &textList[i];
            item->active = true;
            item->id = id;
            item->x = 0.5f;
            item->y = 0.5f;
            item->scale = 1.0f;
            item->angle = 0.0f;
            String_InitArray(item->text, item->textBuffer);
            item->text.length = 0;
            return item;
        }
    }
    return NULL;
}

static void RemoveText(int id) {
    int i;
    for (i = 0; i < MAX_TEXT_ITEMS; i++) {
        if (textList[i].active && textList[i].id == id) {
            textList[i].active = false;
            textList[i].text.length = 0;
            return;
        }
    }
}

static void ClearTexts(void) {
    int i;
    for (i = 0; i < MAX_TEXT_ITEMS; i++) {
        textList[i].active = false;
        textList[i].text.length = 0;
    }
}

static cc_bool ProcessTextArgs(const cc_string* args, int argsCount, cc_bool showErrors) {
    cc_string sub;
    if (argsCount < 1) return false;
    sub = args[0];

    if (String_CaselessEqualsConst(&sub, "add")) {
        int id, i;
        struct DynamicText* item;

        if (argsCount < 7) {
            if (showErrors) Chat_AddStatic("&eUsage: text add [id] [x] [y] [scale] [angle] [message]");
            return false;
        }

        if (!Convert_ParseInt(&args[1], &id)) {
            if (showErrors) Chat_AddStatic("&eInvalid text ID.");
            return false;
        }

        item = FindOrCreateText(id);
        if (!item) {
            if (showErrors) Chat_AddStatic("&eMaximum text overlays reached.");
            return false;
        }

        if (!Convert_ParseFloat(&args[2], &item->x) ||
            !Convert_ParseFloat(&args[3], &item->y) ||
            !Convert_ParseFloat(&args[4], &item->scale) ||
            !Convert_ParseFloat(&args[5], &item->angle)) {
            item->active = false;
            if (showErrors) Chat_AddStatic("&eInvalid numbers in text arguments.");
            return false;
        }

        item->text.length = 0;
        for (i = 6; i < argsCount; i++) {
            if (i > 6) String_Append(&item->text, ' ');
            String_AppendString(&item->text, &args[i]);
        }
        return true;
    }

    if (String_CaselessEqualsConst(&sub, "remove")) {
        int id;

        if (argsCount < 2) {
            if (showErrors) Chat_AddStatic("&eUsage: text remove [id]");
            return false;
        }

        if (!Convert_ParseInt(&args[1], &id)) {
            if (showErrors) Chat_AddStatic("&eInvalid text ID.");
            return false;
        }

        RemoveText(id);
        return true;
    }

    if (String_CaselessEqualsConst(&sub, "clear")) {
        ClearTexts();
        return true;
    }

    return false;
}

static cc_bool ParseServerText(const cc_string* message) {
    cc_string str, command, args[16];
    int argsCount;

    if (!message || !message->buffer || message->length <= 0) return false;

    str = *message;

    while (str.length >= 2 && (str.buffer[0] == '&' || str.buffer[0] == '%')) {
        str.buffer += 2;
        str.length -= 2;
    }

    while (str.length > 0 && (str.buffer[0] == ' ' || str.buffer[0] == '\t')) {
        str.buffer++;
        str.length--;
    }

    if (str.length < 5 ||
        str.buffer[0] != 't' ||
        str.buffer[1] != 'e' ||
        str.buffer[2] != 'x' ||
        str.buffer[3] != 't' ||
        str.buffer[4] != ' ') return false;

    command.buffer = str.buffer + 5;
    command.length = str.length - 5;
    command.capacity = command.length;

    argsCount = String_UNSAFE_Split(&command, ' ', args, 16);
    return ProcessTextArgs(args, argsCount, false);
}

static void Hooked_Chat_AddOf(const cc_string* text, int msgType) {
    if (msgType == MSG_TYPE_NORMAL && text && ParseServerText(text)) return;
    Original_Chat_AddOf(text, msgType);
}

static void WriteAbsoluteJump(unsigned char* at, void* destination) {
    uintptr_t address = (uintptr_t)destination;
    at[0] = 0x48;
    at[1] = 0xB8;
    memcpy(at + 2, &address, sizeof(address));
    at[10] = 0xFF;
    at[11] = 0xE0;
}

static cc_bool InstallChatHook(void) {
    HMODULE module;
    void* target;
    DWORD oldProtect, tempProtect;
    unsigned char* trampoline;

    if (InterlockedCompareExchange(&HookInstalled, 1, 0) != 0) return true;

    module = GetModuleHandleA("ClassiCube.dll");
    if (!module) module = GetModuleHandleA(NULL);
    if (!module) {
        InterlockedExchange(&HookInstalled, 0);
        return false;
    }

    target = (void*)GetProcAddress(module, "Chat_AddOf");
    if (!target) {
        InterlockedExchange(&HookInstalled, 0);
        return false;
    }

    memcpy(OriginalBytes, target, HOOK_SIZE);

    trampoline = (unsigned char*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        InterlockedExchange(&HookInstalled, 0);
        return false;
    }

    memcpy(trampoline, OriginalBytes, HOOK_SIZE);
    WriteAbsoluteJump(trampoline + HOOK_SIZE, (unsigned char*)target + HOOK_SIZE);

    Trampoline = trampoline;
    Original_Chat_AddOf = (Chat_AddOf_Fn)Trampoline;

    if (!VirtualProtect(target, HOOK_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Trampoline = NULL;
        Original_Chat_AddOf = NULL;
        InterlockedExchange(&HookInstalled, 0);
        return false;
    }

    WriteAbsoluteJump((unsigned char*)target, (void*)Hooked_Chat_AddOf);
    FlushInstructionCache(GetCurrentProcess(), target, HOOK_SIZE);
    VirtualProtect(target, HOOK_SIZE, oldProtect, &tempProtect);
    return true;
}

static void RemoveChatHook(void) {
    HMODULE module;
    void* target;
    DWORD oldProtect, tempProtect;

    if (InterlockedCompareExchange(&HookInstalled, 0, 1) != 1) return;

    module = GetModuleHandleA("ClassiCube.dll");
    if (!module) module = GetModuleHandleA(NULL);
    if (!module) return;

    target = (void*)GetProcAddress(module, "Chat_AddOf");
    if (!target) return;

    if (!VirtualProtect(target, HOOK_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect)) return;

    memcpy(target, OriginalBytes, HOOK_SIZE);
    FlushInstructionCache(GetCurrentProcess(), target, HOOK_SIZE);
    VirtualProtect(target, HOOK_SIZE, oldProtect, &tempProtect);

    if (Trampoline) VirtualFree(Trampoline, 0, MEM_RELEASE);
    Trampoline = NULL;
    Original_Chat_AddOf = NULL;
}

static void OnContextLost(void* obj) {
    (void)obj;
    if (text_vb) {
        Gfx_DeleteVb(&text_vb);
        text_vb = 0;
    }
}

static void OnContextRecreated(void* obj) {
    (void)obj;
    if (!text_vb) text_vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4);
}

static void CalcOrthoMatrix(struct Matrix* m, float width, float height, float zNear, float zFar) {
    memset(m, 0, sizeof(struct Matrix));
    m->row1.x = 2.0f / width;
    m->row2.y = -2.0f / height;
    m->row3.z = 1.0f / (zNear - zFar);
    m->row4.x = -1.0f;
    m->row4.y = 1.0f;
    m->row4.z = zNear / (zNear - zFar);
    m->row4.w = 1.0f;
}

static void MakeIdentityMatrix(struct Matrix* m) {
    memset(m, 0, sizeof(struct Matrix));
    m->row1.x = 1.0f;
    m->row2.y = 1.0f;
    m->row3.z = 1.0f;
    m->row4.w = 1.0f;
}

static void DrawTextOverlay(struct DynamicText* item) {
    struct FontDesc font;
    struct DrawTextArgs args;
    struct Texture tex;
    struct Matrix mat, scaleMat, rotMat, transMat;
    struct VertexTextured v[4];
    struct VertexTextured* data;
    float px, py, hw, hh;
    PackedCol col;

    if (!item->active || !item->text.length || !text_vb) return;

    memset(&font, 0, sizeof(font));
    Font_Make(&font, 16, FONT_FLAGS_NONE);

    memset(&args, 0, sizeof(args));
    args.text = item->text;
    args.font = &font;
    args.useShadow = true;

    memset(&tex, 0, sizeof(tex));
    Drawer2D_MakeTextTexture(&tex, &args);

    if (!tex.ID) {
        Font_Free(&font);
        return;
    }

    px = (item->x >= 0.0f && item->x <= 1.0f) ? item->x * Window_Main.Width : item->x;
    py = (item->y >= 0.0f && item->y <= 1.0f) ? item->y * Window_Main.Height : item->y;

    Matrix_Scale(&scaleMat, item->scale, item->scale, 1.0f);
    Matrix_RotateZ(&rotMat, item->angle * MATH_DEG2RAD);
    Matrix_Translate(&transMat, px, py, 0.0f);
    Matrix_Mul(&mat, &scaleMat, &rotMat);
    Matrix_Mul(&mat, &mat, &transMat);

    Gfx_LoadMatrix(MATRIX_VIEW, &mat);

    hw = tex.width / 2.0f;
    hh = tex.height / 2.0f;
    col = PACKEDCOL_WHITE;

    v[0].x = -hw; v[0].y = -hh; v[0].z = 0.0f; v[0].Col = col; v[0].U = tex.uv.u1; v[0].V = tex.uv.v1;
    v[1].x =  hw; v[1].y = -hh; v[1].z = 0.0f; v[1].Col = col; v[1].U = tex.uv.u2; v[1].V = tex.uv.v1;
    v[2].x =  hw; v[2].y =  hh; v[2].z = 0.0f; v[2].Col = col; v[2].U = tex.uv.u2; v[2].V = tex.uv.v2;
    v[3].x = -hw; v[3].y =  hh; v[3].z = 0.0f; v[3].Col = col; v[3].U = tex.uv.u1; v[3].V = tex.uv.v2;

    data = (struct VertexTextured*)Gfx_LockDynamicVb(text_vb, VERTEX_FORMAT_TEXTURED, 4);
    if (data) {
        memcpy(data, v, sizeof(v));
        Gfx_UnlockDynamicVb(text_vb);
        Gfx_BindTexture(tex.ID);
        Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
        Gfx_BindVb(text_vb);
        Gfx_DrawVb_IndexedTris(4);
    }

    Gfx_DeleteTexture(&tex.ID);
    Font_Free(&font);
}

static void OnRender2D(float delta) {
    struct Matrix proj, identity;
    int i;
    (void)delta;

    if (Gui.InputGrab) return;

    if (Window_Main.Width <= 0 || Window_Main.Height <= 0) return;
    if (!text_vb) text_vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4);
    if (!text_vb) return;

    CalcOrthoMatrix(&proj, (float)Window_Main.Width, (float)Window_Main.Height, -10000.0f, 10000.0f);
    Gfx_LoadMatrix(MATRIX_PROJ, &proj);
    Gfx_SetDepthTest(false);
    Gfx_SetAlphaBlending(true);

    for (i = 0; i < MAX_TEXT_ITEMS; i++) {
        if (textList[i].active) DrawTextOverlay(&textList[i]);
    }

    MakeIdentityMatrix(&identity);
    Gfx_LoadMatrix(MATRIX_VIEW, &identity);
    Gfx_SetDepthTest(true);
}

static void TextCommand_Execute(const cc_string* args, int argsCount) {
    ProcessTextArgs(args, argsCount, true);
}

static struct ChatCommand TextCommand = {
    "text",
    TextCommand_Execute,
    false,
    {
        "&a/text add [id] [x] [y] [scale] [angle] [text]",
        "&a/text remove [id]",
        "&a/text clear",
    }
};

static void Register2DDrawHook(void (*hook)(float delta)) {
    int i;
    for (i = 0; i < 16; i++) {
        if (!Game.Draw2DHooks[i]) {
            Game.Draw2DHooks[i] = hook;
            return;
        }
    }
}

static void Plugin_Init(void) {
    int i;

    for (i = 0; i < MAX_TEXT_ITEMS; i++) {
        textList[i].active = false;
        textList[i].id = 0;
        textList[i].x = 0.5f;
        textList[i].y = 0.5f;
        textList[i].scale = 1.0f;
        textList[i].angle = 0.0f;
        String_InitArray(textList[i].text, textList[i].textBuffer);
        textList[i].text.length = 0;
    }

    Commands_Register(&TextCommand);
    Register2DDrawHook(OnRender2D);
    Event_Register_(&GfxEvents.ContextLost, NULL, OnContextLost);
    Event_Register_(&GfxEvents.ContextRecreated, NULL, OnContextRecreated);

    if (!InstallChatHook()) Chat_AddStatic("&c[text] Failed to hook Chat_AddOf");
}

static BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) RemoveChatHook();
    return TRUE;
}

PLUGIN_EXPORT int Plugin_ApiVersion = 1;
PLUGIN_EXPORT struct IGameComponent Plugin_Component = { Plugin_Init };