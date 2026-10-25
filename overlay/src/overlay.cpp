// =====================================================================================
//  MEC Coop - overlay en jeu (DLL injectée dans MirrorsEdgeCatalyst.exe)
//
//  - s'accroche à IDXGISwapChain::Present (DirectX 11) avec MinHook
//  - dessine le radar avec Dear ImGui, directement dans l'image du jeu
//  - les données (positions, cap, réglages) viennent de coop.exe par mémoire partagée
// =====================================================================================
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "MinHook.h"
#include "game.h"

// ---------------------------------------------------------------- mémoire partagée
#pragma pack(push, 1)
struct SharedPlayer {
    char     name[32];
    float    x, y, z;
    int32_t  valid;
    uint32_t color;          // 0xAABBGGRR (format ImGui)
};
struct Shared {
    uint32_t magic;          // 'MECO'
    uint32_t version;        // 1
    uint32_t seq;            // pair = stable, impair = écriture en cours
    int32_t  enabled;        // dessiner le radar ?
    float    me_x, me_y, me_z;
    float    heading;        // radians, 0 = +Z
    int32_t  me_valid;
    float    range;          // mètres
    float    scale;          // taille du radar
    int32_t  corner;         // 0 haut-droite, 1 haut-gauche, 2 bas-gauche, 3 bas-droite
    int32_t  mirror;
    int32_t  count;
    SharedPlayer players[8];
    char     status[96];
    uint32_t dll_frames;     // écrit par la DLL : preuve qu'elle tourne
    // v2 : où lire la caméra et ma position directement dans le jeu (chemins depuis Faith)
    uint64_t faith;
    int32_t  cam_n;
    int32_t  cam_offs[6];
    int32_t  cam_stride;     // octets entre x, y et z
    float    cam_sign;       // cap = sign * atan2(x, z) + off
    float    cam_off;
    int32_t  cam_ok;
    int32_t  pos_n;
    int32_t  pos_offs[4];
    uint32_t flags;          // v5 : bit 0 = radar, bit 1 = pseudos au-dessus des têtes
};
#pragma pack(pop)

static const uint32_t MAGIC = 0x4F43454D;
static_assert(sizeof(Shared) == 648, "taille Shared différente de coop.py");
// GUID définis à la main (pas de bibliothèque dxguid)
static const GUID kIID_Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
static const GUID kIID_Device    = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
static HANDLE   g_map = NULL;
static Shared*  g_shared = NULL;

static bool ReadShared(Shared& out) {
    if (!g_shared) {
        g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "Local\\MECCoopOverlay_v12");
        if (!g_map) return false;
        g_shared = (Shared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
        if (!g_shared) { CloseHandle(g_map); g_map = NULL; return false; }
    }
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s1 = *(volatile uint32_t*)&g_shared->seq;
        if (s1 & 1) { YieldProcessor(); continue; }
        memcpy(&out, (const void*)g_shared, sizeof(Shared));
        MemoryBarrier();
        uint32_t s2 = *(volatile uint32_t*)&g_shared->seq;
        if (s1 == s2) return out.magic == MAGIC;
    }
    return false;
}

// ---------------------------------------------------------------- état DirectX / ImGui
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn oPresent = NULL;
static ResizeFn  oResize = NULL;

static ID3D11Device*           g_dev = NULL;
static ID3D11DeviceContext*    g_ctx = NULL;
static ID3D11RenderTargetView* g_rtv = NULL;
static HWND                    g_hwnd = NULL;
static bool                    g_ready = false;
static LARGE_INTEGER           g_freq, g_last;

// valeurs lissées (pour une animation parfaitement fluide à chaque image du jeu)
// suivi d'un joueur : position affichée + vitesse estimée (prédiction entre deux positions reçues)
struct Smooth {
    char name[32]; float x, y, z; bool init;
    float sx, sy, sz;          // dernière position reçue
    float vx, vy, vz;          // vitesse estimée (m/s)
    LONGLONG st;               // quand elle est arrivée
    float iv;                  // intervalle moyen entre deux positions reçues
};
static Smooth g_sm[8];
static float  g_me[3];
static bool   g_me_init = false;
static float  g_heading = 0.f;

static void CreateRTV(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = NULL;
    if (SUCCEEDED(sc->GetBuffer(0, kIID_Texture2D, (void**)&back)) && back) {
        g_dev->CreateRenderTargetView(back, NULL, &g_rtv);
        back->Release();
    }
}

static void LoadFont(ImGuiIO& io) {
    // Segoe UI Gras si disponible (sinon police ImGui par défaut, agrandie)
    char path[MAX_PATH];
    GetWindowsDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\Fonts\\segoeuib.ttf");
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(f, NULL), got = 0;
        void* data = IM_ALLOC(size);
        if (data && ReadFile(f, data, size, &got, NULL) && got == size) {
            CloseHandle(f);
            io.Fonts->AddFontFromMemoryTTF(data, (int)size, 18.0f);   // ImGui libère data
            return;
        }
        if (data) IM_FREE(data);
        CloseHandle(f);
    }
    ImFontConfig cfg;
    cfg.SizePixels = 18.0f;
    io.Fonts->AddFontDefault(&cfg);
}

static bool InitImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(kIID_Device, (void**)&g_dev)) || !g_dev) return false;
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC desc;
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    LoadFont(io);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    CreateRTV(sc);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    return true;
}

static inline float Alpha(float dt, float tau) { return 1.0f - expf(-dt / tau); }

// lecture sûre de la mémoire du jeu (ne plante jamais, même si l'adresse n'est plus valide)
static bool SafeRead(uint64_t addr, void* out, SIZE_T n) {
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n, &got) && got == n;
}

static uint64_t Resolve(uint64_t ent, const int32_t* offs, int n) {
    if (!ent || n <= 0) return 0;
    uint64_t addr = ent + (uint32_t)offs[0];
    for (int i = 1; i < n; i++) {
        uint64_t p = 0;
        if (!SafeRead(addr, &p, 8) || !p) return 0;
        addr = p + (uint32_t)offs[i];
    }
    return addr;
}

// cap exact lu dans la caméra du jeu
static bool DirectHeading(const Shared& s, float* out) {
    if (!s.cam_ok || (s.cam_stride != 4 && s.cam_stride != 16)) return false;
    uint64_t a;
    if (s.cam_n == -1)                       // adresse directe (64 bits en deux moitiés)
        a = (uint64_t)(uint32_t)s.cam_offs[0] | ((uint64_t)(uint32_t)s.cam_offs[1] << 32);
    else if (s.cam_n >= 1 && s.cam_n <= 6)
        a = Resolve(s.faith, s.cam_offs, s.cam_n);
    else
        return false;
    float v[3];
    for (int i = 0; i < 3; i++)
        if (!SafeRead(a + (uint64_t)(i * s.cam_stride), &v[i], 4)) return false;
    // vecteur unitaire (la ligne "forward" de la caméra peut pencher : on ne garde que x et z)
    if (!(v[0] == v[0]) || !(v[1] == v[1]) || !(v[2] == v[2]) ||
        fabsf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - 1.f) > 0.02f || v[0] * v[0] + v[2] * v[2] < 0.01f)
        return false;
    *out = s.cam_sign * atan2f(v[0], v[2]) + s.cam_off;
    return true;
}

// ma position exacte lue dans le jeu
static bool DirectPos(const Shared& s, float* out) {
    if (s.pos_n < 1 || s.pos_n > 4) return false;
    uint64_t a = Resolve(s.faith, s.pos_offs, s.pos_n);
    if (!SafeRead(a, out, 12)) return false;
    for (int i = 0; i < 3; i++) if (!(out[i] == out[i]) || fabsf(out[i]) > 100000.f) return false;
    // doit rester proche de ce que coop.exe voit (sinon c'est une mauvaise adresse)
    return fabsf(out[0] - s.me_x) + fabsf(out[2] - s.me_z) < 20.f;
}

// ---------------------------------------------------------------- caméra du jeu
// Caméra finale du jeu, d'après le mod MECMP (licence MIT, (c) 2026 MECMP contributors,
// github.com/CodeNameMeteor/mecmp) : [jeu+0x2415e50] +0x08 -> liste -> [0] = manager ;
// manager +0x90 = matrice (right, up, forward, position), +0x80 = champ de vision vertical (degrés).
// La caméra regarde vers MOINS forward.
struct Cam { float right[3], up[3], fwd[3], pos[3]; float fovY; bool ok; };
static Cam g_cam;

static bool ReadCamera(Cam& c) {
    c.ok = false;
    uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL), p = 0, list = 0, mgr = 0;
    if (!SafeRead(base + 0x2415e50, &p, 8) || !p) return false;
    if (!SafeRead(p + 0x08, &list, 8) || !list) return false;
    if (!SafeRead(list, &mgr, 8) || !mgr) return false;
    float m[16];
    if (!SafeRead(mgr + 0x90, m, sizeof(m))) return false;
    for (int i = 0; i < 16; i++) if (!(m[i] == m[i]) || fabsf(m[i]) > 1e6f) return false;
    for (int r = 0; r < 3; r++) {
        float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (l < 0.95f || l > 1.05f) return false;
    }
    for (int i = 0; i < 3; i++) { c.right[i] = m[i]; c.up[i] = m[4 + i]; c.fwd[i] = m[8 + i]; c.pos[i] = m[12 + i]; }
    float fov = 0.f;
    SafeRead(mgr + 0x80, &fov, 4);
    c.fovY = (fov == fov && fov > 20.f && fov < 170.f) ? fov : 55.f;
    c.ok = true;
    return true;
}

// point du monde -> pixel à l'écran (faux s'il est derrière la caméra)
static bool Project(const Cam& c, const float p[3], ImVec2 disp, ImVec2* out, float* depth) {
    float d[3] = {p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2]};
    float x = d[0] * c.right[0] + d[1] * c.right[1] + d[2] * c.right[2];
    float y = d[0] * c.up[0] + d[1] * c.up[1] + d[2] * c.up[2];
    float z = -(d[0] * c.fwd[0] + d[1] * c.fwd[1] + d[2] * c.fwd[2]);
    if (!(z > 0.1f) || disp.x <= 0 || disp.y <= 0) return false;
    float tanY = tanf(c.fovY * 0.5f * 3.14159265f / 180.f), tanX = tanY * disp.x / disp.y;
    out->x = (0.5f + 0.5f * x / (z * tanX)) * disp.x;
    out->y = (0.5f - 0.5f * y / (z * tanY)) * disp.y;
    *depth = z;
    return out->x == out->x && out->y == out->y;
}

// ---------------------------------------------------------------- caméra fluide
// Le jeu met à jour sa caméra à son rythme de simulation, puis INTERPOLE l'image entre la
// mise à jour précédente et la dernière (d'où une image fluide à 180 i/s). On fait pareil :
// sinon les pseudos avancent par à-coups pendant que l'image glisse.
static Cam  g_camPrev, g_camCur;
static LONGLONG g_camT = 0;
static float g_tick = 1.f / 60.f;          // durée mesurée entre deux mises à jour de la caméra
static bool g_camHave = false;
static int  g_camMode = 1;                  // 0 = brut, 1 = interpolé (comme le jeu), 2 = anticipé
static float g_camRate = 0.f;               // mises à jour de caméra par seconde (mesuré)
static LONGLONG g_modeShownUntil = 0;

static void Norm(float* v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void SmoothCamera(LONGLONG now, LONGLONG freq) {
    Cam raw;
    if (!ReadCamera(raw)) { g_cam.ok = false; g_camHave = false; return; }
    bool changed = !g_camHave || memcmp(raw.right, g_camCur.right, sizeof(float) * 12) != 0;
    static int changes = 0;
    static LONGLONG rateT = 0;
    if (changed) changes++;
    if (now - rateT > freq) { g_camRate = (float)changes * (float)freq / (float)(now - rateT + 1); changes = 0; rateT = now; }
    // Alt+6 : changer de mode (pour comparer à l'œil)
    static bool down = false;
    bool k = false;   // (Alt+6 : outil de réglage, retiré)
    if (k && !down) { g_camMode = (g_camMode + 1) % 3; g_modeShownUntil = now + freq * 4; }
    down = k;
    if (changed) {
        if (g_camHave) {
            float dt = (float)(now - g_camT) / (float)freq;
            if (dt > 0.002f && dt < 0.1f) g_tick += (dt - g_tick) * 0.1f;
            g_camPrev = g_camCur;
        } else {
            g_camPrev = raw;
        }
        g_camCur = raw;
        g_camT = now;
        g_camHave = true;
    }
    // grand saut (téléportation, cinématique) : pas d'interpolation
    float jump = fabsf(g_camCur.pos[0] - g_camPrev.pos[0]) + fabsf(g_camCur.pos[1] - g_camPrev.pos[1]) +
                 fabsf(g_camCur.pos[2] - g_camPrev.pos[2]);
    float a = (float)(now - g_camT) / (float)freq / (g_tick > 0.001f ? g_tick : 0.016f);
    if (g_camMode == 0) a = 1.f;                          // brut : la dernière caméra lue
    else if (g_camMode == 2) a = a + 1.f;                 // anticipé : on prolonge le mouvement
    float amax = g_camMode == 2 ? 2.f : 1.f;
    if (a > amax || jump > 10.f) a = jump > 10.f ? 1.f : amax;
    if (a < 0.f) a = 0.f;
    Cam& c = g_cam;
    for (int i = 0; i < 3; i++) {
        c.right[i] = g_camPrev.right[i] + (g_camCur.right[i] - g_camPrev.right[i]) * a;
        c.fwd[i]   = g_camPrev.fwd[i]   + (g_camCur.fwd[i]   - g_camPrev.fwd[i])   * a;
        c.pos[i]   = g_camPrev.pos[i]   + (g_camCur.pos[i]   - g_camPrev.pos[i])   * a;
    }
    c.fovY = g_camPrev.fovY + (g_camCur.fovY - g_camPrev.fovY) * a;
    // ré-orthonormaliser : forward, puis right perpendiculaire, puis up = forward x right
    Norm(c.fwd);
    float d = c.right[0] * c.fwd[0] + c.right[1] * c.fwd[1] + c.right[2] * c.fwd[2];
    for (int i = 0; i < 3; i++) c.right[i] -= d * c.fwd[i];
    Norm(c.right);
    c.up[0] = c.fwd[1] * c.right[2] - c.fwd[2] * c.right[1];
    c.up[1] = c.fwd[2] * c.right[0] - c.fwd[0] * c.right[2];
    c.up[2] = c.fwd[0] * c.right[1] - c.fwd[1] * c.right[0];
    // même sens que le "up" du jeu (au cas où la convention serait inversée)
    if (c.up[0] * g_camCur.up[0] + c.up[1] * g_camCur.up[1] + c.up[2] * g_camCur.up[2] < 0) {
        c.up[0] = -c.up[0]; c.up[1] = -c.up[1]; c.up[2] = -c.up[2];
    }
    c.ok = true;
}

// ---------------------------------------------------------------- état de l'image
static float g_px[8], g_py[8], g_pz[8];   // positions lissées des joueurs (même ordre que s.players)
static float g_headOff = 0.6f;              // de "ma position lue" jusqu'au dessus de la tête
static bool  g_camHeading = false;

static void Update(const Shared& s, float dt) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    SmoothCamera(qpc.QuadPart, g_freq.QuadPart);
    if (s.me_valid) {
        float p[3];
        bool direct = s.version >= 2 && DirectPos(s, p);
        if (direct) {
            g_me[0] = p[0]; g_me[1] = p[1]; g_me[2] = p[2]; g_me_init = true;
        } else {
            float a = Alpha(dt, 0.04f);
            if (!g_me_init || fabsf(s.me_x - g_me[0]) + fabsf(s.me_z - g_me[2]) > 60.f) {
                g_me[0] = s.me_x; g_me[1] = s.me_y; g_me[2] = s.me_z; g_me_init = true;
            } else {
                g_me[0] += (s.me_x - g_me[0]) * a; g_me[1] += (s.me_y - g_me[1]) * a; g_me[2] += (s.me_z - g_me[2]) * a;
            }
        }
        // cap : 1) caméra du jeu, 2) adresse donnée par coop.exe, 3) valeur de coop.exe lissée
        float h;
        g_camHeading = g_cam.ok && g_cam.fwd[0] * g_cam.fwd[0] + g_cam.fwd[2] * g_cam.fwd[2] > 0.01f;
        if (g_camHeading) {
            g_heading = atan2f(-g_cam.fwd[0], -g_cam.fwd[2]);
        } else if (s.version >= 2 && DirectHeading(s, &h)) {
            g_heading = h;
        } else {
            float d = fmodf(s.heading - g_heading + 3.14159265f, 6.2831853f);
            if (d < 0) d += 6.2831853f;
            g_heading += (d - 3.14159265f) * Alpha(dt, 0.03f);
        }
        // hauteur de la tête : la caméra est à hauteur des yeux de Faith ; on mesure l'écart
        // entre la caméra et "ma position" pour placer les pseudos au même endroit chez les autres
        static float lastMe[3] = {0, 0, 0};
        float mv = (fabsf(p[0] - lastMe[0]) + fabsf(p[2] - lastMe[2])) / (dt > 0.001f ? dt : 0.001f);
        if (direct) { lastMe[0] = p[0]; lastMe[1] = p[1]; lastMe[2] = p[2]; }
        if (g_cam.ok && direct && mv < 0.5f) {             // seulement à l'arrêt (pas de secousses de course)
            float horiz = fabsf(g_cam.pos[0] - p[0]) + fabsf(g_cam.pos[2] - p[2]);
            float off = g_cam.pos[1] - p[1] + 0.35f;
            if (horiz < 1.5f && off > -1.5f && off < 2.5f) g_headOff += (off - g_headOff) * Alpha(dt, 1.0f);
        }
    }
    // suivi de chaque joueur (repéré par son nom) : on avance à sa vitesse à CHAQUE image,
    // et on se recale doucement sur chaque nouvelle position reçue -> mouvement fluide
    LONGLONG now = qpc.QuadPart;
    float fq = (float)g_freq.QuadPart;
    for (int i = 0; i < s.count && i < 8; i++) {
        const SharedPlayer& p = s.players[i];
        g_px[i] = p.x; g_py[i] = p.y; g_pz[i] = p.z;
        if (!p.valid) continue;
        Smooth* sm = NULL;
        for (int j = 0; j < 8; j++) if (g_sm[j].init && strcmp(g_sm[j].name, p.name) == 0) { sm = &g_sm[j]; break; }
        if (!sm) for (int j = 0; j < 8; j++) if (!g_sm[j].init) { sm = &g_sm[j]; break; }
        if (!sm) continue;
        bool fresh = !sm->init || strcmp(sm->name, p.name) != 0 ||
                     fabsf(p.x - sm->x) + fabsf(p.y - sm->y) + fabsf(p.z - sm->z) > 15.f;   // téléportation
        if (fresh) {
            lstrcpynA(sm->name, p.name, 32);
            sm->x = sm->sx = p.x; sm->y = sm->sy = p.y; sm->z = sm->sz = p.z;
            sm->vx = sm->vy = sm->vz = 0.f; sm->st = now; sm->iv = 1.f / 30.f; sm->init = true;
        } else if (p.x != sm->sx || p.y != sm->sy || p.z != sm->sz) {        // nouvelle position reçue
            float ds = (float)(now - sm->st) / fq;
            if (ds < 0.004f) ds = 0.004f;
            if (ds < 0.25f) {
                // vitesse calculée sur l'intervalle MOYEN : l'heure d'arrivée irrégulière ne la fait pas trembler
                sm->iv += (ds - sm->iv) * 0.1f;
                float k = 0.35f, d = sm->iv > 0.004f ? sm->iv : 0.004f;
                sm->vx += ((p.x - sm->sx) / d - sm->vx) * k;
                sm->vy += ((p.y - sm->sy) / d - sm->vy) * k;
                sm->vz += ((p.z - sm->sz) / d - sm->vz) * k;
            } else {
                sm->vx = sm->vy = sm->vz = 0.f;                              // reprise après une pause
            }
            sm->sx = p.x; sm->sy = p.y; sm->sz = p.z; sm->st = now;
        }
        // position visée : la dernière reçue, prolongée à la vitesse estimée (au plus 0,15 s)
        float age = (float)(now - sm->st) / fq;
        if (age > 0.15f) age = 0.15f;
        float tx = sm->sx + sm->vx * age, ty = sm->sy + sm->vy * age, tz = sm->sz + sm->vz * age;
        if ((float)(now - sm->st) / fq > 0.4f) { sm->vx *= 0.9f; sm->vy *= 0.9f; sm->vz *= 0.9f; }  // arrêté
        sm->x += sm->vx * dt; sm->y += sm->vy * dt; sm->z += sm->vz * dt;
        float a = Alpha(dt, 0.25f);
        sm->x += (tx - sm->x) * a; sm->y += (ty - sm->y) * a; sm->z += (tz - sm->z) * a;
        g_px[i] = sm->x; g_py[i] = sm->y; g_pz[i] = sm->z;
    }
}

static void DrawRadar(const Shared& s) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float k = s.scale > 0.3f ? s.scale : 1.0f;
    float size = 260.f * k, R = 115.f * k, margin = 24.f;
    float listH = 26.f * k * (s.count + 1);
    float x0 = (s.corner == 0 || s.corner == 3) ? disp.x - size - margin : margin;
    float y0 = (s.corner == 0 || s.corner == 1) ? margin : disp.y - size - listH - margin;
    ImVec2 c(x0 + size * 0.5f, y0 + size * 0.5f);

    dl->AddCircleFilled(c, R, IM_COL32(17, 20, 24, 200), 64);
    dl->AddCircle(c, R, IM_COL32(232, 232, 232, 230), 64, 2.0f * k);
    dl->AddCircle(c, R * 0.5f, IM_COL32(70, 76, 86, 200), 48, 1.0f);
    char buf[128];
    wsprintfA(buf, "%d m", (int)s.range);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - R + 4 * k), IM_COL32(150, 156, 165, 255), buf);
    float t = 9.f * k;
    dl->AddTriangleFilled(ImVec2(c.x, c.y - t), ImVec2(c.x - t * 0.66f, c.y + t * 0.66f),
                          ImVec2(c.x + t * 0.66f, c.y + t * 0.66f), IM_COL32(255, 255, 255, 255));

    float ly = y0 + size + 4.f;
    if (s.status[0]) {
        dl->AddText(ImVec2(x0 + 8, ly), IM_COL32(255, 179, 71, 255), s.status);
        ly += 22.f * k;
    }
    if (!s.me_valid) return;

    float sa = sinf(g_heading), ca = cosf(g_heading);
    float pxPerM = R / (s.range > 1 ? s.range : 150.f);
    for (int i = 0; i < s.count && i < 8; i++) {
        const SharedPlayer& p = s.players[i];
        if (!p.valid) {
            wsprintfA(buf, "%s : pas en jeu", p.name);
            dl->AddText(ImVec2(x0 + 8, ly), p.color, buf);
            ly += 22.f * k;
            continue;
        }
        float rx = g_px[i] - g_me[0], rz = g_pz[i] - g_me[2];
        float fwd = rx * sa + rz * ca;
        float right = rx * ca - rz * sa;
        if (s.mirror) right = -right;
        float dist = sqrtf(fwd * fwd + right * right);
        float lim = s.range > 1 ? s.range : 150.f;
        if (dist > lim && dist > 0.001f) { fwd *= lim / dist; right *= lim / dist; }
        ImVec2 dot(c.x + right * pxPerM, c.y - fwd * pxPerM);
        dl->AddCircleFilled(dot, 6.f * k, p.color, 20);
        dl->AddCircle(dot, 6.f * k, IM_COL32(0, 0, 0, 255), 20, 1.5f);
        ImVec2 ns = ImGui::CalcTextSize(p.name);
        dl->AddText(ImVec2(dot.x - ns.x * 0.5f + 1, dot.y - 12.f * k - ns.y + 1), IM_COL32(0, 0, 0, 200), p.name);
        dl->AddText(ImVec2(dot.x - ns.x * 0.5f, dot.y - 12.f * k - ns.y), p.color, p.name);
        float dh = g_py[i] - g_me[1];
        if (dh > 2.f)       wsprintfA(buf, "%s : %d m   +%d m", p.name, (int)(dist + 0.5f), (int)(dh + 0.5f));
        else if (dh < -2.f) wsprintfA(buf, "%s : %d m   -%d m", p.name, (int)(dist + 0.5f), (int)(-dh + 0.5f));
        else                wsprintfA(buf, "%s : %d m", p.name, (int)(dist + 0.5f));
        dl->AddText(ImVec2(x0 + 9, ly + 1), IM_COL32(0, 0, 0, 200), buf);
        dl->AddText(ImVec2(x0 + 8, ly), p.color, buf);
        ly += 22.f * k;
    }
}

static uint32_t NameId(const char* name) {
    uint32_t h = 2166136261u;
    for (const unsigned char* c = (const unsigned char*)name; *c; c++) { h ^= *c; h *= 16777619u; }
    return h | 1u;
}

// pseudos au-dessus de la tête des autres joueurs, dans le monde 3D
static void DrawNametags(const Shared& s) {
    if (!g_cam.ok || !s.me_valid) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    char buf[64];
    for (int i = 0; i < s.count && i < 8; i++) {
        const SharedPlayer& p = s.players[i];
        if (!p.valid) continue;
        float head[3] = {g_px[i], g_py[i] + g_headOff, g_pz[i]};
        float feet[3];
        if (game::active() && game::ghosts::shownPosition(NameId(p.name), feet)) {   // son personnage 3D est là : au-dessus de sa tête
            head[0] = feet[0]; head[1] = feet[1] + 2.05f; head[2] = feet[2];
        }
        ImVec2 sp;
        float depth;
        if (!Project(g_cam, head, disp, &sp, &depth)) continue;
        if (sp.x < -200 || sp.x > disp.x + 200 || sp.y < -200 || sp.y > disp.y + 200) continue;
        float dx = head[0] - g_cam.pos[0], dy = head[1] - g_cam.pos[1], dz = head[2] - g_cam.pos[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        // bien visible jusqu'à 60 m, puis s'efface doucement (mais reste lisible de loin)
        float fade = dist < 60.f ? 1.f : (dist > 400.f ? 0.45f : 1.f - 0.55f * (dist - 60.f) / 340.f);
        int alpha = (int)(255 * fade);
        ImU32 col = (p.color & 0x00FFFFFF) | ((ImU32)alpha << 24);
        ImU32 shadow = IM_COL32(0, 0, 0, (int)(200 * fade));
        // losange juste au-dessus de la tête
        float r = dist < 15.f ? 7.f : 5.f;
        ImVec2 q(sp.x, sp.y);
        dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), col);
        dl->AddQuad(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), shadow, 1.5f);
        // pseudo + distance
        ImVec2 ns = ImGui::CalcTextSize(p.name);
        ImVec2 np(q.x - ns.x * 0.5f, q.y - r - 4.f - ns.y);
        dl->AddText(ImVec2(np.x + 1, np.y + 1), shadow, p.name);
        dl->AddText(np, col, p.name);
        wsprintfA(buf, "%d m", (int)(dist + 0.5f));
        ImVec2 ds = ImGui::CalcTextSize(buf);
        float sc = 0.8f;
        ImVec2 dp(q.x - ds.x * sc * 0.5f, q.y + r + 2.f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * sc, ImVec2(dp.x + 1, dp.y + 1), shadow, buf);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * sc, dp, IM_COL32(230, 230, 230, alpha), buf);
    }
}

// Message de coop (progression de l'hôte : « retourne au menu », « clique sur Continuer »...)
// Alt+R : retour au menu principal (pour changer de sauvegarde sans quitter le jeu)
[[maybe_unused]] static void CheckMenuKey() {
    static bool down = false, first = true;
    if (first) { first = false; game::logf("menu : touche Alt+R prête"); }
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0, r = (GetAsyncKeyState('R') & 0x8000) != 0;
    bool k = pid == GetCurrentProcessId() && alt && r;
    static DWORD lastDiag = 0;
    if ((alt || r) && GetTickCount() - lastDiag > 1000) {   // diagnostic : ce que le mod voit des touches
        lastDiag = GetTickCount();
        game::logf("menu : touches vues Alt=%d R=%d, fenêtre du jeu au premier plan=%d", alt, r, pid == GetCurrentProcessId());
    }
    if (k && !down) game::menu::request(0);
    down = k;
}

static void DrawNotice() {
    char text[128];
    if (!game::notice::text(text, sizeof(text))) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 d = ImGui::GetIO().DisplaySize;
    float k1 = d.y / 1080.f;
    float fs = ImGui::GetFontSize() * 1.3f * (k1 > 0.7f ? k1 : 0.7f);
    ImVec2 s1 = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, text);
    float w = s1.x + 40 * k1, h = s1.y + 24 * k1;
    ImVec2 a(d.x * 0.5f - w * 0.5f, d.y * 0.04f), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, IM_COL32(12, 14, 18, 225), 8 * k1);
    dl->AddRect(a, b, IM_COL32(227, 38, 28, 235), 8 * k1, 0, 2 * k1);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(d.x * 0.5f - s1.x * 0.5f, a.y + 12 * k1), IM_COL32(255, 255, 255, 255), text);
}

// Invitation : un autre joueur a lancé une mission. Alt+M = y aller (téléportation au départ).
static void DrawInvite() {
    char text[96];
    bool taken = false;
    static DWORD takenAt = 0;
    static char last[96] = "";
    if (!game::invite::text(text, sizeof(text), &taken)) { takenAt = 0; return; }
    if (strcmp(text, last) != 0) { lstrcpynA(last, text, sizeof(last)); takenAt = 0; }
    static bool down = false;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool k = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('M') & 0x8000);
    if (k && !down && !taken) game::invite::accept();
    down = k;
    const char* line2 = "Alt+M : y aller";
    if (taken) {
        if (!takenAt) takenAt = GetTickCount();
        if (GetTickCount() - takenAt > 20000) return;
        lstrcpynA(text, "Lance la mission ici : le départ est juste à côté de toi", sizeof(text));
        line2 = "";
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 d = ImGui::GetIO().DisplaySize;
    float k1 = d.y / 1080.f;
    float fs = ImGui::GetFontSize() * 1.35f * (k1 > 0.7f ? k1 : 0.7f);
    ImVec2 s1 = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, text);
    ImVec2 s2 = ImGui::GetFont()->CalcTextSizeA(fs * 0.8f, 1e9f, 0, line2);
    float w = (s1.x > s2.x ? s1.x : s2.x) + 40 * k1, h = s1.y + (line2[0] ? s2.y + 6 * k1 : 0) + 24 * k1;
    ImVec2 a(d.x * 0.5f - w * 0.5f, d.y * 0.12f), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, IM_COL32(12, 14, 18, 210), 8 * k1);
    dl->AddRect(a, b, IM_COL32(255, 210, 60, 230), 8 * k1, 0, 2 * k1);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(d.x * 0.5f - s1.x * 0.5f, a.y + 12 * k1), IM_COL32(255, 255, 255, 255), text);
    if (line2[0])
        dl->AddText(ImGui::GetFont(), fs * 0.8f, ImVec2(d.x * 0.5f - s2.x * 0.5f, a.y + 18 * k1 + s1.y),
                    IM_COL32(255, 210, 60, 255), line2);
}

static void Frame(const Shared& s, float dt) {
    Update(s, dt);
    if (game::active()) {        // téléportation : fondu au noir
        float fa = game::teleport::fadeAlpha();
        if (fa > 0.f) {
            ImVec2 d = ImGui::GetIO().DisplaySize;
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), d, IM_COL32(0, 0, 0, (int)(255 * fa)));
            return;
        }
    }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (game::active()) { DrawInvite(); DrawNotice(); }
    if (q.QuadPart < g_modeShownUntil) {
        static const char* names[3] = {"brut", "fluide (interpolé)", "anticipé"};
        char b[128];
        wsprintfA(b, "Caméra : mode %d - %s   (le jeu la met à jour %d fois/s)", g_camMode, names[g_camMode], (int)(g_camRate + 0.5f));
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddText(ImVec2(41, 41), IM_COL32(0, 0, 0, 220), b);
        dl->AddText(ImVec2(40, 40), IM_COL32(255, 210, 60, 255), b);
    }
    bool v5 = s.version >= 5;
    if (!v5 || (s.flags & 2)) DrawNametags(s);
    if (!v5 || (s.flags & 1)) DrawRadar(s);
}

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return oPresent(sc, sync, flags);   // simple essai, rien n'est affiché
    if (game::active() && !game::hooks::clientUpdateHooked()) game::ghosts::renderFrame();
    // (Alt+R retiré : le retour au menu remettait l histoire à zéro)
    if (!g_ready) {
        static bool once = false;
        if (!once) { once = true; game::logf("affichage : 1re image vue, préparation"); }
        g_ready = InitImGui(sc);
        if (g_ready) game::logf("affichage : prêt");
    }
    Shared s;
    if (g_ready && ReadShared(s)) {
        g_shared->dll_frames++;
        // si coop.exe ne met plus les données à jour (fermé, planté), on n'affiche rien
        static uint32_t lastSeq = 0;
        static LONGLONG lastSeqTime = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s.seq != lastSeq) { lastSeq = s.seq; lastSeqTime = now.QuadPart; }
        bool fresh = (now.QuadPart - lastSeqTime) < g_freq.QuadPart * 3 / 2;
        char nt[8];
        bool hasNotice = game::active() && game::notice::text(nt, sizeof(nt));   // affiché même hors partie coop
        if ((s.enabled && fresh) || hasNotice) {
            if (!g_rtv) CreateRTV(sc);
            float dt = (float)(now.QuadPart - g_last.QuadPart) / (float)g_freq.QuadPart;
            g_last = now;
            if (dt > 0.1f) dt = 0.1f;
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (s.enabled && fresh) Frame(s, dt);
            else DrawNotice();
            ImGui::Render();
            ID3D11RenderTargetView* oldRtv = NULL;
            ID3D11DepthStencilView* oldDsv = NULL;
            g_ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
            g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
            if (oldRtv) oldRtv->Release();
            if (oldDsv) oldDsv->Release();
        }
    }
    return oPresent(sc, sync, flags);
}

static HRESULT __stdcall hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_rtv) { g_rtv->Release(); g_rtv = NULL; }
    return oResize(sc, n, w, h, f, fl);
}

// ---------------------------------------------------------------- installation des crochets
static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

// GridNode (outil de test, chargé avant nous) remplace la case Present de la table de DXGI et appelle ensuite
// g_origPresent. Poser notre crochet par-dessus (fenêtre factice + MinHook) faisait planter le jeu avec l'overlay
// Steam (25/10, appel à 0 depuis gameoverlayrenderer64). Avec lui, on s'insère dans SA chaîne : GridNode -> nous ->
// ce qu'il appelait. Adresses de gridnode.dll 0.1.0 (reconnu à ses octets ; autre version : pas touché).
static const uintptr_t kGnPresentHook = 0x115b60, kGnCallOrig = 0x115be5, kGnOrigPresent = 0x4746f0, kGnOrigResize = 0x4746e8;
static bool HookThroughGridNode() {
    HMODULE g = GetModuleHandleA("gridnode.dll");
    if (!g) return false;
    uintptr_t b = (uintptr_t)g;
    static const uint8_t sig[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x55, 0x57, 0x56, 0x53};
    uint8_t got[sizeof(sig)], call[6];
    if (!SafeRead(b + kGnPresentHook, got, sizeof(got)) || memcmp(got, sig, sizeof(sig)) ||
        !SafeRead(b + kGnCallOrig, call, sizeof(call)) || call[0] != 0xff || call[1] != 0x15 ||
        kGnCallOrig + 6 + *(int32_t*)(call + 2) != kGnOrigPresent) {
        game::logf("affichage : GridNode présent mais version inconnue, crochet habituel");
        return false;
    }
    void* volatile* op = (void* volatile*)(b + kGnOrigPresent);
    void* volatile* orz = (void* volatile*)(b + kGnOrigResize);
    for (int i = 0; i < 300 && (!*op || !*orz); i++) Sleep(100);   // GridNode s'installe ~1,5 s après son chargement
    if (!*op || !*orz) { game::logf("affichage : GridNode pas encore prêt, crochet habituel"); return false; }
    Sleep(1500);
    for (int t = 0; t < 5; t++) {
        void* p = *op;
        if (p != (void*)hkPresent) {
            oPresent = (decltype(oPresent))p;
            InterlockedCompareExchangePointer((PVOID volatile*)op, (void*)hkPresent, p);
        }
        void* r = *orz;
        if (r != (void*)hkResize) {
            oResize = (decltype(oResize))r;
            InterlockedCompareExchangePointer((PVOID volatile*)orz, (void*)hkResize, r);
        }
        Sleep(1000);
        if (*op == (void*)hkPresent && *orz == (void*)hkResize) {
            game::logf("affichage : branché derrière GridNode (pas de fenêtre factice)");
            return true;
        }
    }
    // on remet ce qu'on a changé avant le crochet habituel (sinon on serait deux fois dans la chaîne)
    if (*op == (void*)hkPresent) InterlockedCompareExchangePointer((PVOID volatile*)op, (void*)oPresent, (void*)hkPresent);
    if (*orz == (void*)hkResize) InterlockedCompareExchangePointer((PVOID volatile*)orz, (void*)oResize, (void*)hkResize);
    game::logf("affichage : GridNode reprend sa case, crochet habituel");
    return false;
}

static DWORD WINAPI InitThread(LPVOID) {
    if (HookThroughGridNode()) return 0;
    WNDCLASSEXA wc = { sizeof(wc), CS_CLASSDC, DummyProc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL,
                       "MECCoopDummy", NULL };
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowA("MECCoopDummy", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = NULL;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                               &sd, &sc, &dev, &fl, &ctx);
    if (SUCCEEDED(hr) && sc) {
        void** vt = *(void***)sc;
        void* present = vt[8];
        void* resize = vt[13];
        MH_Initialize();
        MH_CreateHook(present, (void*)hkPresent, (void**)&oPresent);
        MH_CreateHook(resize, (void*)hkResize, (void**)&oResize);
        int st = MH_EnableHook(MH_ALL_HOOKS);
        game::logf("affichage : crochet de l'image %s (Present %p)", st == 0 ? "posé" : "REFUSÉ", present);
        sc->Release();
        dev->Release();
        ctx->Release();
    }
    DestroyWindow(hwnd);
    UnregisterClassA("MECCoopDummy", wc.hInstance);
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // une autre version de l'overlay est déjà dans ce jeu (mise à jour sans relancer le jeu) : deux
        // overlays se battraient pour les mêmes crochets. On ne fait rien ; la nouvelle servira au prochain lancement.
        CreateMutexA(NULL, FALSE, "Local\\MECCoopOverlayLoaded");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
        for (int v = 1; v < 64; v++) {
            char name[32];
            wsprintfA(name, "mec_overlay_v%d.dll", v);
            HMODULE h = GetModuleHandleA(name);
            if (h && h != (HMODULE)inst) return TRUE;
        }
        game::start();
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
