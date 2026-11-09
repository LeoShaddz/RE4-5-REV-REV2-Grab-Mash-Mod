// RERev_GrabMash - Resident Evil Revelations (rerev.exe, Steam, PC, 32 bits)
// Segurar o botao A do controle (ou uma tecla do teclado, ExtraKeyVK no .ini) durante o agarrao
// ("balancar o analogico / mover o mouse para os lados") faz o jogo contar o escape sozinho.
// Fora do agarrao segurar A/tecla nao faz nada.
//
// NAO COMPILADO nem TESTADO (nao consigo rodar o jogo aqui). Feito so por analise estatica do rerev.exe
// (base fixa 0x400000, sem ASLR). Se o exe do seu jogo for diferente, o mod confere a assinatura e NAO altera nada.
//
// Como funciona (o que achei no exe)
//  * O jogo tem uma classe cActionCommand (prompt de acao) com tipos: 1 Trigger, 2 Press, 3 Repeat,
//    4 StickDirection, 5 StickRoll, 6 StickGacha (o balancar do agarrao), 7 outro.
//  * O tipo 6 e tratado pela funcao 0x40E760 (thiscall, ecx = cActionCommand, 1 float na pilha).
//    Cada frame ela mede o quanto o analogico/mouse andou e SUBTRAI isso do campo float [this+0x28]
//    ("distancia que falta"). Quando [this+0x28] chega a 0 ou menos, o escape e concluido.
//  * O mod coloca um desvio no inicio dessa funcao. Com A (ou a tecla) segurado, ele reduz [this+0x28]
//    proporcionalmente ao tempo, de modo que o agarrao termina em EscapeMs milissegundos.
//  * Eu NAO tenho como confirmar sem rodar o jogo que +0x28 e mesmo a "distancia que falta"; a leitura do
//    codigo indica que sim. Ligue Log=1 e me mande o RERev_GrabMash.log se nao funcionar.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned int  u32;

static const u32 FN_GACHA    = 0x40E760;    // handler do tipo 6 (StickGacha)
static const int CMD_TYPE_OFF = 0x04;       // tipo do comando
static const int CMD_TYPE_GACHA = 6;
static const int CMD_REMAIN_OFF = 0x28;     // float: distancia que falta
static const int PATCH_LEN   = 9;           // push ebp; mov ebp,esp; and esp,-8; sub esp,0x1C

static int  gEnable = 1, gEscapeMs = 800, gLog = 0, gVk = 0, gPadA = 1;
static FILE* gF = NULL;
static char gDir[MAX_PATH] = "";
static void Log(const char* f, ...) {
    if (!gLog) return;
    if (!gF) { char p[MAX_PATH]; strcpy(p, gDir); strcat(p, "RERev_GrabMash.log"); gF = fopen(p, "a"); }
    if (!gF) return;
    va_list a; va_start(a, f); vfprintf(gF, f, a); va_end(a); fputc('\n', gF); fflush(gF);
}

// ---- XInput carregado dinamicamente ----
struct XPAD { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XSTATE { DWORD packet; XPAD pad; };
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XSTATE*);
static XInputGetState_t pXGet = NULL;
static void LoadXInput() {
    const char* dlls[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
    for (const char* n : dlls) {
        HMODULE h = LoadLibraryA(n);
        if (h) { pXGet = (XInputGetState_t)GetProcAddress(h, "XInputGetState"); if (pXGet) { Log("XInput: %s", n); return; } }
    }
    Log("XInput nao encontrado");
}
static bool Held() {
    if (gVk && (GetAsyncKeyState(gVk) & 0x8000)) return true;
    if (gPadA && pXGet) {
        for (DWORD i = 0; i < 4; ++i) {
            XSTATE s; memset(&s, 0, sizeof(s));
            if (pXGet(i, &s) == ERROR_SUCCESS && (s.pad.wButtons & 0x1000)) return true;   // A
        }
    }
    return false;
}

// ---- chamado no inicio do handler do tipo 6 (todo frame) ----
static u32   gThis = 0;
static float gStart = 0, gLast = 0;
static DWORD gTick = 0;
static int   gLogged = 0;

static void __cdecl OnGacha(u32 self) {
    if (!gEnable || !self) return;
    if (IsBadReadPtr((void*)self, 0x30)) return;
    if (*(volatile int*)(self + CMD_TYPE_OFF) != CMD_TYPE_GACHA) return;
    volatile float* rem = (volatile float*)(self + CMD_REMAIN_OFF);
    float v = *rem;
    DWORD now = GetTickCount();
    if (self != gThis || v > gLast + 0.0001f || now - gTick > 500) {          // prompt novo (ou reiniciado)
        gThis = self; gStart = v; gTick = now; gLast = v;
        if (gLogged < 40) { ++gLogged; Log("agarrao: cmd=%08X restante inicial=%g", self, v); }
    }
    if (!(v > 0.0f)) { gTick = now; gLast = v; return; }
    if (!Held()) { gTick = now; gLast = v; return; }
    DWORD dt = now - gTick; gTick = now;
    if (dt > 100) dt = 100;                                                     // nao pula tudo apos um travamento
    float dec = gStart * (float)dt / (float)gEscapeMs;
    float nv = v - dec;
    if (nv < 0.0f) nv = 0.0f;
    *rem = nv; gLast = nv;
}

static bool Install() {
    u32 fn = FN_GACHA;
    static bool logged = false;
    if (IsBadReadPtr((void*)fn, 24)) { if (!logged) Log("fn %08X ilegivel", fn); logged = true; return false; }
    const u8* c = (const u8*)fn;
    static const u8 sig[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x1C, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x83, 0x7F, 0x38, 0x00 };
    if (memcmp(c, sig, sizeof(sig)) != 0) {
        if (!logged) {
            Log("assinatura nao confere em %08X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                fn, c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7],c[8],c[9],c[10],c[11],c[12],c[13],c[14],c[15],c[16],c[17]);
            logged = true;
        }
        return false;
    }
    // trampolim: pushfd; pushad; push ecx; call OnGacha; add esp,4; popad; popfd; [9 bytes originais]; jmp fn+9
    u8* tr = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    u8* p = tr;
    *p++ = 0x9C;                                        // pushfd
    *p++ = 0x60;                                        // pushad
    *p++ = 0x51;                                        // push ecx   (this)
    *p++ = 0xE8; *(u32*)p = (u32)&OnGacha - (u32)(p + 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;              // add esp,4
    *p++ = 0x61;                                        // popad
    *p++ = 0x9D;                                        // popfd
    memcpy(p, (void*)fn, PATCH_LEN); p += PATCH_LEN;
    *p++ = 0xE9; *(u32*)p = (fn + PATCH_LEN) - (u32)(p + 4); p += 4;
    FlushInstructionCache(GetCurrentProcess(), tr, 64);

    DWORD old;
    if (!VirtualProtect((void*)fn, PATCH_LEN, PAGE_EXECUTE_READWRITE, &old)) return false;
    u8* f = (u8*)fn;
    f[0] = 0xE9; *(u32*)(f + 1) = (u32)tr - (fn + 5);
    for (int i = 5; i < PATCH_LEN; ++i) f[i] = 0x90;
    VirtualProtect((void*)fn, PATCH_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)fn, PATCH_LEN);
    return true;
}

static DWORD WINAPI Init(LPVOID) {
    char ini[MAX_PATH]; HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Init, &self);
    GetModuleFileNameA(self, ini, MAX_PATH);
    char* s = strrchr(ini, '\\'); if (s) *(s + 1) = 0;
    strcpy(gDir, ini); strcat(ini, "RERev_GrabMash.ini");
    gEnable   = GetPrivateProfileIntA("Main", "Enable", 1, ini);
    gEscapeMs = GetPrivateProfileIntA("Main", "EscapeMs", 800, ini);
    gPadA     = GetPrivateProfileIntA("Main", "UseGamepadA", 1, ini);
    gVk       = GetPrivateProfileIntA("Main", "ExtraKeyVK", 0, ini);
    gLog      = GetPrivateProfileIntA("Main", "Log", 0, ini);
    if (gEscapeMs < 50) gEscapeMs = 50;
    Log("RERev_GrabMash v1 carregado (Enable=%d EscapeMs=%d UseGamepadA=%d ExtraKeyVK=%d)", gEnable, gEscapeMs, gPadA, gVk);
    if (!gEnable) return 0;
    LoadXInput();
    for (int i = 0; i < 120; ++i) {                     // o codigo pode ainda nao estar liberado ao carregar: tenta ~60 s
        if (Install()) { Log("desvio instalado em 0x%X", FN_GACHA); return 0; }
        Sleep(500);
    }
    Log("assinatura nao confere: exe diferente, nada foi alterado");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(NULL, 0, Init, NULL, 0, NULL); }
    return TRUE;
}
