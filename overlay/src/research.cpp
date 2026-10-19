// =====================================================================================
//  MEC Coop - recherche (temporaire) : le câble du grappin (MagRope)
//  On cherche les objets PamClientMagropeRenderEntity (ce qui dessine le câble) et on note dans le
//  journal ce qui change dedans pendant que le joueur se sert du grappin, pour trouver où sont les
//  points du câble. Rien n'est modifié dans le jeu (lecture seule).
// =====================================================================================
#include "game.h"
#include <stdio.h>

namespace game {
namespace research {

static const uintptr_t Vtbl_MagropeRender = 0x141c410f8;     // PamClientMagropeRenderEntity
static const uintptr_t Vtbl_Magrope = 0x141c619e0;           // PamClientMagropeEntity (points d'accroche)
static const uintptr_t Vtbl_MagropeVisualizer = 0x141c46b18; // PamClientMagRopeVisualizerEntity
static const size_t kSpan = 0x300;

static uintptr_t g_render[8];
static int g_nRender = 0;
static uint8_t g_prev[8][kSpan];
static uint8_t g_prevSub[8][18][0x100];
static int g_lines = 0;

static void scan() {
    const uintptr_t vR = rva(Vtbl_MagropeRender), vM = rva(Vtbl_Magrope), vV = rva(Vtbl_MagropeVisualizer);
    const size_t kBuf = 4 << 20;
    uint8_t* buf = (uint8_t*)VirtualAlloc(NULL, kBuf, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    int nM = 0, nV = 0;
    uintptr_t firstM = 0, firstV = 0;
    g_nRender = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t addr = 0x10000; VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        if ((uint8_t*)base == buf) continue;
        for (size_t off = 0; off < mbi.RegionSize; off += kBuf) {
            size_t n = mbi.RegionSize - off < kBuf ? mbi.RegionSize - off : kBuf;
            if (!read(base + off, buf, n)) continue;
            const uintptr_t* q = (const uintptr_t*)buf;
            for (size_t i = 0; i < n / 8; i += 2) {
                uintptr_t v = q[i];
                if (v == vR) { if (g_nRender < 8) g_render[g_nRender++] = base + off + i * 8; }
                else if (v == vM) { if (!firstM) firstM = base + off + i * 8; nM++; }
                else if (v == vV) { if (!firstV) firstV = base + off + i * 8; nV++; }
            }
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    logf("recherche grappin : %d dessin(s) de câble, %d points d'accroche (1er %p), %d visualiseur(s) (1er %p)", g_nRender,
         nM, (void*)firstM, nV, (void*)firstV);
    for (int i = 0; i < g_nRender; i++) {
        read(g_render[i], g_prev[i], kSpan);
        for (int k = 0; k < 18; k++) {
            uintptr_t p = 0;
            memcpy(&p, g_prev[i] + 0xc8 + k * 8, 8);
            if (p > 0x10000) read(p, g_prevSub[i][k], 0x100);
        }
        logf("recherche grappin : dessin %d = %p", i, (void*)g_render[i]);
    }
}

static void logDiff(const char* what, int idx, int k, const uint8_t* a, const uint8_t* b, size_t n) {
    char line[900];
    size_t len = 0;
    int shown = 0;
    for (size_t o = 0; o + 4 <= n && shown < 40; o += 4) {
        if (memcmp(a + o, b + o, 4) == 0) continue;
        float f;
        memcpy(&f, b + o, 4);
        uint32_t u;
        memcpy(&u, b + o, 4);
        char item[48];
        if (finite(f) && fabsf(f) > 1e-4f && fabsf(f) < 1e6f) _snprintf(item, sizeof(item), " +%x=%.2f", (unsigned)o, f);
        else _snprintf(item, sizeof(item), " +%x=#%08x", (unsigned)o, u);
        size_t il = strlen(item);
        if (len + il >= sizeof(line) - 1) break;
        memcpy(line + len, item, il);
        len += il;
        shown++;
    }
    line[len] = 0;
    if (shown) {
        if (k >= 0) logf("grappin %s %d [+%x]:%s", what, idx, 0xc8 + k * 8, line);
        else logf("grappin %s %d:%s", what, idx, line);
        g_lines++;
    }
}

void tick() {
    static DWORD nextScan = 0, last = 0;
    static int scans = 0;
    DWORD now = GetTickCount();
    if (!g_nRender && scans < 6 && now >= nextScan) {
        scans++;
        scan();
        nextScan = now + 20000;
    }
    if (g_lines > 400 || now - last < 100) return;
    last = now;
    for (int i = 0; i < g_nRender; i++) {
        uint8_t cur[kSpan];
        if (!read(g_render[i], cur, kSpan) || readOr<uintptr_t>(g_render[i]) != rva(Vtbl_MagropeRender)) continue;
        logDiff("dessin", i, -1, g_prev[i], cur, kSpan);
        for (int k = 0; k < 18; k++) {
            uintptr_t p = 0;
            memcpy(&p, cur + 0xc8 + k * 8, 8);
            uint8_t sub[0x100];
            if (p > 0x10000 && read(p, sub, 0x100)) {
                logDiff("dessin", i, k, g_prevSub[i][k], sub, 0x100);
                memcpy(g_prevSub[i][k], sub, 0x100);
            }
        }
        memcpy(g_prev[i], cur, kSpan);
    }
}

void levelChanged() { g_nRender = 0; }

}  // namespace research
}  // namespace game
