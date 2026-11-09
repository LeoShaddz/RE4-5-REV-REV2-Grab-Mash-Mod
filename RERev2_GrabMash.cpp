// RERev2_GrabMash - Resident Evil Revelations 2 (rerev2.exe, Steam, PC, 32 bits)
// Segurar o botao A do controle (ou uma tecla do teclado) simula, de forma repetida:
//   * o balancar do analogico esquerdo (e do mouse para os lados) -> escapar de agarroes
//   * apertar A varias vezes (e a tecla de acao do teclado)        -> girar valvulas e QTEs de apertar rapido
//
// NAO COMPILADO nem TESTADO (nao consigo rodar o jogo aqui).
// v3 acrescenta sensores por vtable (agarroes de varios inimigos); v2: ainda SIMULA a entrada (XInput + SendInput), pois NAO achei o contador de escape no rerev2.exe. Mas agora
// usa dois "sensores" achados na analise estatica do exe: so observa (nao altera) duas funcoes do jogo que rodam
// todo frame enquanto a situacao existe, e so simula quando uma delas rodou ha pouco (GateMs):
//   * 0x541D90 = cEmActionGrap::update (agarrao de inimigo)  -> libera analogico/mouse balancando e A/tecla pulsando
//   * 0x785AC0 = cSmDoorValveAct_ValveRoll::update (valvula de porta) -> libera A/tecla pulsando
// Se a assinatura de bytes nao conferir, o sensor nao e instalado e esse sensor passa a ser ignorado (como na v1).
// Limites: outros inimigos/valvulas podem usar outras classes que nao cobri (veja o log).
//
//  1) CONTROLE: desvio dentro do XInputGetState da DLL do XInput (encadeia com outros desvios).
//  2) TECLADO/MOUSE: SendInput (scancode para teclas, MOUSEEVENTF_MOVE relativo para o mouse).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef unsigned char u8;
typedef unsigned int  u32;

static int gGate = 1, gGateMs = 250;
static volatile DWORD gGrabTick = 0, gValveTick = 0, gGrabStart = 0;
static int gGrabMaxMs = 3000, gPulseAnywhere = 0;
static bool gGrabSensor = false, gValveSensor = false;
static int gEnable = 1, gLog = 1, gHoldMs = 150, gAmp = 30000, gDead = 9000;
static int gPadMash = 1, gPadWiggle = 1, gPadMashMs = 60, gPadWiggleMs = 60;
static int gVk = 70, gMashVk = 69, gKeyMash = 1, gKeyMouse = 1, gKeyMashMs = 60, gMouseMs = 60, gMouseStep = 60;

static char  gDir[MAX_PATH] = "";
static FILE* gF = NULL;
static volatile LONG gLines = 0;
static void Log(const char* f, ...) {
    if (!gLog) return;
    if (InterlockedIncrement(&gLines) > 5000) return;
    if (!gF) { char p[MAX_PATH]; strcpy(p, gDir); strcat(p, "RERev2_GrabMash.log"); gF = fopen(p, "a"); }
    if (!gF) return;
    va_list a; va_start(a, f); vfprintf(gF, f, a); va_end(a); fputc('\n', gF); fflush(gF);
}

// ---------------- controle (XInput) ----------------
struct XPAD { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XSTATE { DWORD packet; XPAD pad; };
typedef DWORD (WINAPI *XGet_t)(DWORD, XSTATE*);
static const int NX = 3;
static XGet_t gTramp[NX] = { NULL, NULL, NULL };
static bool   gHooked[NX] = { false, false, false };
static volatile LONG gCalls = 0;

static DWORD gHoldStart = 0;       // quando o A fisico comecou a ser segurado (0 = solto)
static bool  gActive = false;

static bool GrabOn(DWORD now)  { return !gGate || !gGrabSensor  || (gGrabTick  && now - gGrabTick  <= (DWORD)gGateMs); }
static bool ValveOn(DWORD now) { return !gGate || !gValveSensor || (gValveTick && now - gValveTick <= (DWORD)gGateMs); }
// com sensor instalado: so age se o sensor de agarrao ou de valvula estiver ativo. Sem sensor: como na v1.
static bool AnyOn(DWORD now) {
    if (gPulseAnywhere) return true;                                   // opcao: A/tecla pulsam sempre que segurados (so o balancar continua so no agarrao)
    if (!gGate || (!gGrabSensor && !gValveSensor)) return true;
    return (gGrabSensor && GrabOn(now)) || (gValveSensor && ValveOn(now));
}
static bool WiggleOn(DWORD now) {
    if (!gGate || !gGrabSensor) return true;
    if (!GrabOn(now)) return false;
    // o balancar so vale nos primeiros GrabMaxMs do agarrao: depois de se soltar, o jogo ainda roda a funcao do
    // agarrao por alguns segundos (animacao de recuperacao) e o personagem andaria sozinho.
    if (gGrabMaxMs > 0 && gGrabStart && now - gGrabStart > (DWORD)gGrabMaxMs) return false;
    return true;
}

static void Apply(DWORD idx, XSTATE* s) {
    InterlockedIncrement(&gCalls);
    if (!s || !gEnable) return;
    DWORD now = GetTickCount();
    bool a = (s->pad.wButtons & 0x1000) != 0;                          // A fisico
    if (!a) { if (gActive) { gActive = false; Log("controle: A solto, parou de simular"); } gHoldStart = 0; return; }
    if (!gHoldStart) gHoldStart = now;
    if (now - gHoldStart < (DWORD)gHoldMs) return;                     // toque curto: nao interfere
    if (!AnyOn(now)) return;                                           // nem agarrado nem na valvula
    if (!gActive) { gActive = true; Log("controle: simulando (A segurado %u ms; pad %u; stick real L=(%d,%d))", now - gHoldStart, idx, s->pad.lx, s->pad.ly); }
    if (gPadMash) {                                                    // A pulsando
        if (((now / (DWORD)gPadMashMs) & 1) != 0) s->pad.wButtons |= 0x1000; else s->pad.wButtons &= ~0x1000;
    }
    if (gPadWiggle && WiggleOn(now)) {                                 // analogico esquerdo balancando, so se voce NAO esta mexendo nele
        int ax = s->pad.lx < 0 ? -s->pad.lx : s->pad.lx, ay = s->pad.ly < 0 ? -s->pad.ly : s->pad.ly;
        if (ax <= gDead && ay <= gDead) {
            s->pad.lx = ((now / (DWORD)gPadWiggleMs) & 1) ? (SHORT)gAmp : (SHORT)-gAmp;
            s->pad.ly = 0;
        }
    }
}
#define DEFHOOK(i) static DWORD WINAPI HookX##i(DWORD idx, XSTATE* s) { DWORD r = gTramp[i](idx, s); if (r == ERROR_SUCCESS) Apply(idx, s); return r; }
DEFHOOK(0) DEFHOOK(1) DEFHOOK(2)
static void* const gHooks[NX] = { (void*)&HookX0, (void*)&HookX1, (void*)&HookX2 };

static bool HookFunc(int i, u32 target) {
    static const u8 pro[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    if (IsBadReadPtr((void*)target, 16)) return false;
    const u8* c = (const u8*)target;
    u8* tr = (u8*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    if (memcmp(c, pro, 5) == 0) {
        memcpy(tr, c, 5);
        tr[5] = 0xE9; *(u32*)(tr + 6) = (target + 5) - (u32)(tr + 10);
    } else if (c[0] == 0xE9) {
        u32 dest = target + 5 + *(const u32*)(c + 1);
        Log("xinput ja desviado por outro programa: %08X -> %08X (encadeando)", target, dest);
        tr[0] = 0xE9; *(u32*)(tr + 1) = dest - (u32)(tr + 5);
    } else {
        Log("prologo inesperado em %08X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", target,
            c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), tr, 32);
    DWORD old;
    if (!VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    gTramp[i] = (XGet_t)tr;
    u8* f = (u8*)target;
    f[0] = 0xE9; *(u32*)(f + 1) = (u32)gHooks[i] - (target + 5);
    VirtualProtect((void*)target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 5);
    return true;
}

static int HookXInput() {
    const char* dlls[NX] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
    int n = 0;
    for (int i = 0; i < NX; ++i) {
        if (gHooked[i]) { ++n; continue; }
        HMODULE h = GetModuleHandleA(dlls[i]);
        if (!h) continue;
        u32 fn = (u32)GetProcAddress(h, "XInputGetState");
        if (!fn) continue;
        gHooked[i] = true;
        if (HookFunc(i, fn)) { ++n; Log("%s: XInputGetState (%08X) desviado", dlls[i], fn); }
        else Log("%s: nao consegui desviar %08X", dlls[i], fn);
    }
    return n;
}

// ---------------- sensores (so observam) ----------------
// v3: sensores extras por tabela de vtable (slot 7 = update) das classes de agarrao de cada inimigo e da valvula.
// {endereco da vtable, update esperado, nome, tipo 1=agarrao 2=valvula}
struct VSens { u32 vt, fn; const char* name; int kind; };
static const VSens kVS[] = {
    { 0x1269e0c, 0x4e72d0, "cEm3500ActAttackGrap", 1 },
    { 0x12762c8, 0x532ad0, "cEm3400ActGrapLoop", 1 },
    { 0x1276330, 0x533070, "cEm3400ActGrapPlEscape", 1 },
    { 0x127889c, 0x541d90, "cEmActionGrap", 1 },
    { 0x127e40c, 0x559850, "cEm1200ActGrap", 1 },
    { 0x1280f24, 0x5737e0, "cEm0100ActGrap", 1 },
    { 0x128b5a0, 0x5ac3b0, "cEm1100ActGrap", 1 },
    { 0x1293a60, 0x621c80, "cEm1500ActGrapHit", 1 },
    { 0x1334980, 0x7a1950, "cSmGinaAct_Grap", 1 },
    { 0x130f2e8, 0x785ac0, "cSmDoorValveAct_ValveRoll", 2 },
    { 0x13815ec, 0x81cd70, "cSmValveAct_Broken", 2 },
    { 0x13815bc, 0x81cd90, "cSmValveAct_Search", 2 },
    { 0x138158c, 0x81ce80, "cSmValveAct_Wait", 2 },
    // v5: "manivelas/alavancas" em movimento (podem ser a valvula que voce gira apertando varias vezes)
    { 0x1358cf0, 0x7c4320, "cSmLeverAct_OnMove", 2 },
    { 0x13598ec, 0x7c5de0, "cSmLeverCraneAct_OffMove", 2 },
    { 0x135988c, 0x7c5e80, "cSmLeverCraneAct_OnMove", 2 },
    { 0x135a048, 0x7c6c40, "cSmLeverLongPressAct_OffMove", 2 },
    { 0x1359fe8, 0x7c6d60, "cSmLeverLongPressAct_OnMove", 2 },
    { 0x135ac28, 0x7c8b40, "cSmLeverOnOffAct_OnMove", 2 },
    { 0x135b514, 0x7c6c40, "cSmLeverRollAct_OffMove", 2 },
    { 0x135b4b4, 0x7ca520, "cSmLeverRollAct_OnMove", 2 },
};
static const int NVS = (int)(sizeof(kVS) / sizeof(kVS[0]));
static volatile LONG gLastVS = -1;
static void __cdecl MarkVS(int id) {
    DWORD n = GetTickCount(); if (!n) n = 1;
    if (kVS[id].kind == 1) { if (!gGrabTick  || n - gGrabTick  > 1000 || gLastVS != id) { Log("sensor: AGARRAO ativo (%s)", kVS[id].name); if (!gGrabTick || n - gGrabTick > 1000) gGrabStart = n; }  gGrabTick  = n; }
    else                   { if (!gValveTick || n - gValveTick > 1000 || gLastVS != id) Log("sensor: VALVULA ativa (%s)", kVS[id].name); gValveTick = n; }
    gLastVS = id;
}
static int InstallVtableSensors() {
    int ok = 0;
    for (int i = 0; i < NVS; ++i) {
        u32* slot = (u32*)(kVS[i].vt + 28);
        if (IsBadReadPtr(slot, 4) || *slot != kVS[i].fn) { Log("vtable %s: slot 7 = %08X (esperado %08X), ignorado", kVS[i].name, IsBadReadPtr(slot, 4) ? 0 : *slot, kVS[i].fn); continue; }
        u8* tr = (u8*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tr) continue;
        u8* p = tr;
        *p++ = 0x9C; *p++ = 0x60;                                      // pushfd; pushad
        *p++ = 0x68; *(u32*)p = (u32)i; p += 4;                        // push id
        *p++ = 0xE8; *(u32*)p = (u32)&MarkVS - (u32)(p + 4); p += 4;   // call MarkVS
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                         // add esp,4
        *p++ = 0x61; *p++ = 0x9D;                                      // popad; popfd
        *p++ = 0xE9; *(u32*)p = kVS[i].fn - (u32)(p + 4); p += 4;      // jmp update original (this/pilha intactos)
        FlushInstructionCache(GetCurrentProcess(), tr, 32);
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) continue;
        *slot = (u32)tr;
        VirtualProtect(slot, 4, old, &old);
        ++ok;
    }
    Log("sensores por vtable: %d de %d instalados", ok, NVS);
    return ok;
}

static void __cdecl MarkGrab()  { DWORD n = GetTickCount(); if (!gGrabTick  || n - gGrabTick  > 1000) { Log("sensor: AGARRAO ativo"); gGrabStart = n ? n : 1; }  gGrabTick  = n ? n : 1; }
static void __cdecl MarkValve() { DWORD n = GetTickCount(); if (!gValveTick || n - gValveTick > 1000) Log("sensor: VALVULA ativa"); gValveTick = n ? n : 1; }

static bool InstallSensor(u32 fn, const u8* sig, int len, void (__cdecl *cb)(), const char* name) {
    if (IsBadReadPtr((void*)fn, 16)) { Log("%s: %08X ilegivel", name, fn); return false; }
    if (memcmp((void*)fn, sig, len) != 0) {
        const u8* c = (const u8*)fn;
        Log("%s: assinatura nao confere em %08X: %02X %02X %02X %02X %02X %02X %02X %02X (sensor desligado)", name, fn, c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7]);
        return false;
    }
    u8* tr = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    u8* p = tr;
    *p++ = 0x9C; *p++ = 0x60;                                          // pushfd; pushad
    *p++ = 0xE8; *(u32*)p = (u32)cb - (u32)(p + 4); p += 4;            // call cb
    *p++ = 0x61; *p++ = 0x9D;                                          // popad; popfd
    memcpy(p, (void*)fn, len); p += len;                               // instrucoes originais (sem referencias relativas)
    *p++ = 0xE9; *(u32*)p = (fn + len) - (u32)(p + 4); p += 4;
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    DWORD old;
    if (!VirtualProtect((void*)fn, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    u8* f = (u8*)fn;
    f[0] = 0xE9; *(u32*)(f + 1) = (u32)tr - (fn + 5);
    for (int i = 5; i < len; ++i) f[i] = 0x90;
    VirtualProtect((void*)fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)fn, len);
    Log("%s: sensor instalado em %08X", name, fn);
    return true;
}
static void InstallSensors() {
    // cEmActionGrap::update: push ebp; mov ebp,esp; and esp,-16   (6 bytes)
    static const u8 sg[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
    // cSmDoorValveAct_ValveRoll::update: mov eax,[0x15DE6F8]       (5 bytes)
    static const u8 sv[5] = { 0xA1, 0xF8, 0xE6, 0x5D, 0x01 };
    for (int i = 0; i < 240; ++i) {                                    // espera o exe estar pronto
        if (!gGrabSensor)  gGrabSensor  = InstallSensor(0x541D90, sg, 6, MarkGrab,  "agarrao");
        if (!gValveSensor) gValveSensor = InstallSensor(0x785AC0, sv, 5, MarkValve, "valvula");
        if (gGrabSensor && gValveSensor) break;
        if (i == 3) break;                                             // assinatura errada nao melhora esperando
        Sleep(500);
    }
    if (InstallVtableSensors() > 0) { gGrabSensor = true; gValveSensor = true; }
}

// ---------------- teclado e mouse ----------------
static void SetKey(int vk, bool down) {
    if (!vk) return;
    INPUT in; memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}
static void MouseMove(int dx) {
    INPUT in; memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx; in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}
static bool GameIsForeground() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI KeyLoop(LPVOID) {
    typedef UINT (WINAPI *TBP)(UINT);
    HMODULE mm = LoadLibraryA("winmm.dll");
    if (mm) { TBP f = (TBP)GetProcAddress(mm, "timeBeginPeriod"); if (f) f(1); }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD holdStart = 0; bool active = false, keyDown = false;
    DWORD lastMouse = 0; int dir = 1;
    for (;;) {
        Sleep(1);
        bool held = (GetAsyncKeyState(gVk) & 0x8000) != 0 && GameIsForeground();
        DWORD now = GetTickCount();
        if (!held) {
            if (keyDown) { SetKey(gMashVk, false); keyDown = false; }
            if (active) { active = false; Log("teclado: parou de simular"); }
            holdStart = 0; continue;
        }
        if (!holdStart) holdStart = now;
        if (now - holdStart < (DWORD)gHoldMs) continue;
        if (!AnyOn(now)) {
            if (keyDown) { SetKey(gMashVk, false); keyDown = false; }
            continue;
        }
        if (!active) { active = true; Log("teclado: simulando (tecla %d segurada)", gVk); }
        if (gKeyMash && gMashVk) {                                     // tecla de acao pulsando
            bool want = ((now / (DWORD)gKeyMashMs) & 1) != 0;
            if (want != keyDown) { SetKey(gMashVk, want); keyDown = want; }
        }
        if (gKeyMouse && WiggleOn(now) && now - lastMouse >= (DWORD)gMouseMs) {         // mouse para os lados
            lastMouse = now; MouseMove(dir * gMouseStep); dir = -dir;
        }
    }
    return 0;
}

static DWORD WINAPI Init(LPVOID) {
    char ini[MAX_PATH]; HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Init, &self);
    GetModuleFileNameA(self, ini, MAX_PATH);
    char* s = strrchr(ini, '\\'); if (s) *(s + 1) = 0;
    strcpy(gDir, ini); strcat(ini, "RERev2_GrabMash.ini");
    gEnable     = GetPrivateProfileIntA("Main", "Enable", 1, ini);
    gLog        = GetPrivateProfileIntA("Main", "Log", 1, ini);
    gGate       = GetPrivateProfileIntA("Main", "UseGameSensors", 1, ini);
    gGateMs     = GetPrivateProfileIntA("Main", "GateMs", 250, ini);
    gGrabMaxMs  = GetPrivateProfileIntA("Main", "GrabMaxMs", 3000, ini);
    gPulseAnywhere = GetPrivateProfileIntA("Main", "PulseAnywhere", 0, ini);
    gHoldMs     = GetPrivateProfileIntA("Main", "HoldDelayMs", 150, ini);
    gAmp        = GetPrivateProfileIntA("Main", "Amplitude", 30000, ini);
    gDead       = GetPrivateProfileIntA("Main", "StickIdleZone", 9000, ini);
    gPadMash    = GetPrivateProfileIntA("Main", "PadMashA", 1, ini);
    gPadWiggle  = GetPrivateProfileIntA("Main", "PadWiggleStick", 1, ini);
    gPadMashMs  = GetPrivateProfileIntA("Main", "PadMashPeriodMs", 60, ini);
    gPadWiggleMs= GetPrivateProfileIntA("Main", "PadWigglePeriodMs", 60, ini);
    gVk         = GetPrivateProfileIntA("Main", "ExtraKeyVK", 70, ini);
    gMashVk     = GetPrivateProfileIntA("Main", "MashKeyVK", 69, ini);
    gKeyMash    = GetPrivateProfileIntA("Main", "KeyMash", 1, ini);
    gKeyMouse   = GetPrivateProfileIntA("Main", "KeyMouseWiggle", 1, ini);
    gKeyMashMs  = GetPrivateProfileIntA("Main", "KeyMashPeriodMs", 60, ini);
    gMouseMs    = GetPrivateProfileIntA("Main", "MouseWigglePeriodMs", 60, ini);
    gMouseStep  = GetPrivateProfileIntA("Main", "MouseStepPx", 60, ini);
    if (gHoldMs < 0) gHoldMs = 0;
    if (gPadMashMs < 10) gPadMashMs = 10;
    if (gPadWiggleMs < 10) gPadWiggleMs = 10;
    if (gKeyMashMs < 10) gKeyMashMs = 10;
    if (gMouseMs < 10) gMouseMs = 10;
    if (gAmp > 32767) gAmp = 32767;
    if (gAmp < 1) gAmp = 1;
    Log("RERev2_GrabMash v5 carregado (HoldDelayMs=%d ExtraKeyVK=%d MashKeyVK=%d)", gHoldMs, gVk, gMashVk);
    if (!gEnable) return 0;
    u32 base = (u32)GetModuleHandleA(NULL);
    if (base != 0x400000) Log("AVISO: exe carregado em %08X (esperado 00400000)", base);
    if (gGate && base == 0x400000) InstallSensors();
    Log("sensores: agarrao=%d valvula=%d", (int)gGrabSensor, (int)gValveSensor);
    if (gVk) CreateThread(NULL, 0, KeyLoop, NULL, 0, NULL);
    for (int i = 0; i < 240; ++i) {                                    // o XInput pode ser carregado depois: tenta ~2 min
        if (HookXInput() == NX) break;
        Sleep(500);
    }
    Log("XInput: %d DLL(s) desviada(s)", HookXInput());
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(NULL, 0, Init, NULL, 0, NULL); }
    return TRUE;
}
