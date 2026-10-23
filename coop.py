"""
MEC Coop - mod coopératif pour Mirror's Edge Catalyst (PC)
Jusqu'à 4 joueurs dans la même ville : chacun voit les autres en 3D (animés), entend leurs sons,
suit leurs missions, et partage le même monde (progression de l'hôte).

Lance coop.py (ou coop_vX.exe) : une fenêtre s'ouvre avec ton pseudo,
les boutons Héberger / Rejoindre / Test solo, l'état et les réglages.
L'overlay (mec_overlay.dll) est injecté dans le jeu par coop.

Raccourcis (dans le jeu) :
  Alt+M           aller à la mission lancée par un autre joueur
  Alt+1 / Alt+2   zoom avant / arrière du radar
  Alt+3           taille du radar
  Alt+4           coin de l'écran
  Alt+5           masquer / afficher
"""
import base64
import ctypes
import ctypes.wintypes  # noqa: F401  (importés d'avance : un exe remplacé pendant qu'il tourne ne peut plus rien importer)
import hashlib  # noqa: F401
import mmap  # noqa: F401
import shutil  # noqa: F401
import traceback  # noqa: F401
import zlib  # noqa: F401
import json
import math
import os
import socket
import struct
import sys
import threading
import time

try:
    import numpy as np
    import psutil
except ImportError:
    import subprocess
    subprocess.call([sys.executable, "-m", "pip", "install", "numpy", "psutil"])
    import numpy as np
    import psutil

HERE = os.path.dirname(sys.executable if getattr(sys, "frozen", False) else os.path.abspath(__file__))
CONFIG = os.path.join(HERE, "coop_config.json")
PORT = 27777
MAX_PLAYERS = 4

# --- Ce que la rétro-ingénierie nous a appris -----------------------------------------
CHAR_VT = 0x1c3c760                        # vtable de PamClientCharacterEntity
HEALTH_OFF = 0x198                         # seul le joueur local a un composant de santé ici
HEALTH_NAME = "PamClientHumanHealthComponent"
POS_PATH = [0x248, 0x580, 0x270]           # personnage -> animation -> ... -> X, Y, Z
YAW_ROW = -0x20                            # ligne de rotation d'un os, juste avant la position
# Caméra (trouvée par camauto/camfix : 0 retard, 0° d'erreur). Le jeu se charge toujours à la
# même adresse, donc on part de variables globales du jeu (jeu + décalage) puis on suit les pointeurs.
# Plusieurs chemins indépendants : on garde celui qui marche et qui tourne avec Faith.
# Caméra finale du jeu, d'après le mod MECMP (licence MIT, © 2026 MECMP contributors,
# github.com/CodeNameMeteor/mecmp, src/game/engine.h) :
#   [jeu+0x2415e50] (ClientCameraViewManagers) +0x08 -> liste -> [0] -> manager +0x90
#   = matrice right, up, forward, position ; la caméra regarde vers -forward.
CAM_MGR_PATH = [0x2415e50, 0x08, 0x0, 0x90]
CAM_STATIC = [
    [0x23659b0, 0x550, 0x508],
    [0x21054c8, 0xa30, 0x550, 0x508], [0x2105508, 0xa30, 0x550, 0x508],
    [0x2105528, 0xa30, 0x550, 0x508], [0x2111080, 0xa30, 0x550, 0x508],
    [0x2142b30, 0xa30, 0x550, 0x508], [0x2364ff0, 0x9d0, 0x550, 0x508],
    [0x2365010, 0x9b0, 0x550, 0x508], [0x2365050, 0x970, 0x550, 0x508],
    [0x2106a68, 0x4b0, 0x70, 0x38], [0x2106b20, 0x358, 0x3d0, 0x38],
    [0x2106b28, 0xd8, 0x60, 0x38], [0x2106b30, 0x2e0, 0xe70, 0x38],
    [0x21095a0, 0x350, 0x998, 0x38], [0x21095b0, 0x38, 0x858, 0x38],
]
CAM_STRIDE = 16


def wrap_angle(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


# ======================================================================================
#  Lecture de la mémoire du jeu
# ======================================================================================
class Mem:
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    MEM_COMMIT, MEM_PRIVATE = 0x1000, 0x20000
    CHUNK = 64 * 1024 * 1024

    def __init__(self):
        import ctypes.wintypes as wt

        class MBI(ctypes.Structure):
            _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                        ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                        ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD),
                        ("Protect", wt.DWORD), ("Type", wt.DWORD)]

        class ME32(ctypes.Structure):
            _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD),
                        ("th32ProcessID", wt.DWORD), ("GlblcntUsage", wt.DWORD),
                        ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                        ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE),
                        ("szModule", ctypes.c_wchar * 256), ("szExePath", ctypes.c_wchar * 260)]

        self.MBI, self.ME32 = MBI, ME32
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
        k.OpenProcess.restype = wt.HANDLE
        k.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
        k.VirtualQueryEx.restype = ctypes.c_size_t
        k.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        k.ReadProcessMemory.restype = wt.BOOL
        k.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
        k.CreateToolhelp32Snapshot.restype = wt.HANDLE
        k.Module32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(ME32)]
        k.Module32FirstW.restype = wt.BOOL
        k.CloseHandle.argtypes = [wt.HANDLE]
        k.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
        self.k = k
        self.h = None
        self.pid = None
        self.base = self.size = 0
        self.game_dir = None

    def open(self):
        for p in psutil.process_iter(["name", "exe"]):
            name = (p.info["name"] or "").lower()
            if "mirrorsedgecatalyst" in name and "trial" not in name:
                h = self.k.OpenProcess(self.PROCESS_QUERY_INFORMATION | self.PROCESS_VM_READ, False, p.pid)
                if not h:
                    return "Accès refusé au jeu : lance coop.py en administrateur"
                snap = self.k.CreateToolhelp32Snapshot(0x18, p.pid)
                me = self.ME32()
                me.dwSize = ctypes.sizeof(me)
                self.k.Module32FirstW(snap, ctypes.byref(me))
                self.k.CloseHandle(snap)
                self.h, self.base, self.size = h, me.modBaseAddr, me.modBaseSize
                self.pid = p.pid
                self.game_dir = os.path.dirname(p.info["exe"] or "")
                return None
        return "Jeu non lancé"

    def alive(self):
        import ctypes.wintypes as wt
        code = wt.DWORD()
        return bool(self.h) and self.k.GetExitCodeProcess(self.h, ctypes.byref(code)) and code.value == 259

    def read(self, addr, n):
        if not addr:
            return None
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        ok = self.k.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
        return buf.raw if ok and got.value == n else None

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def u64_array(self, addr, nbytes):
        arr = np.empty(nbytes // 8, dtype=np.uint64)
        got = ctypes.c_size_t(0)
        ok = self.k.ReadProcessMemory(self.h, ctypes.c_void_p(addr), ctypes.c_void_p(arr.ctypes.data),
                                      arr.nbytes, ctypes.byref(got))
        return arr[: got.value // 8] if ok or got.value else None

    def heap_chunks(self):
        addr, mbi = 0, self.MBI()
        while addr < 0x7FFFFFFFFFFF:
            if not self.k.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            base, size = mbi.BaseAddress or 0, mbi.RegionSize
            if mbi.State == self.MEM_COMMIT and mbi.Type == self.MEM_PRIVATE and mbi.Protect & 0xCC \
                    and not mbi.Protect & 0x100:
                for off in range(0, size, self.CHUNK):
                    yield base + off, min(self.CHUNK, size - off)
            addr = base + size

    def in_module(self, v):
        return self.base <= v < self.base + self.size

    def type_name(self, obj):
        vt = self.u64(obj)
        if not vt or not self.in_module(vt):
            return None
        fn = self.u64(vt)
        code = self.read(fn, 8) if fn else None
        if not code or code[:3] != b"\x48\x8d\x05" or code[7] != 0xC3:
            return None
        ti = fn + 7 + struct.unpack("<i", code[3:7])[0]
        data = self.u64(ti)
        name_p = self.u64(data) if data else None
        raw = self.read(name_p, 64) if name_p else None
        return raw.split(b"\0")[0].decode("latin1") if raw else None


def plausible(p):
    return p is not None and all(v == v and abs(v) < 100000 for v in p) and sum(abs(v) for v in p) > 1


class Game(threading.Thread):
    """Trouve Faith dans la mémoire et lit sa position en continu."""

    def __init__(self, mem=None):
        super().__init__(daemon=True)
        self.mem = mem
        self.faith = None
        self.pos = None
        self.yaw_raw = None        # orientation brute du squelette de Faith (radians)
        self.status = "Démarrage..."
        self.lost_since = None
        # caméra
        self.cam_raw = None        # angle brut de la caméra (radians), seulement une fois vérifiée
        self.cam_key = None        # identifiant de la disposition en mémoire (pour la calibration)
        self.cam_addr = None       # adresse vérifiée du vecteur de cap
        self.cam_stride = CAM_STRIDE
        self.cam_cands = []        # candidats en cours de vérification
        self.cam_path = None       # chemin fixe vérifié
        self.cam_thread = None
        self.cam_exact = False
        self.cam_trusted = set()   # chemins déjà vérifiés et calibrés lors d'une partie précédente
        self.cam_next_scan = 0.0
        self.cam_fail = 0
        self.cam_next_check = 0.0
        self.cam_phase = "idle"    # idle / scan / verify / ok

    def find_faith(self):
        vt = np.uint64(self.mem.base + CHAR_VT)
        for base, size in self.mem.heap_chunks():
            arr = self.mem.u64_array(base, size)
            if arr is None or not arr.size:
                continue
            for i in np.nonzero(arr == vt)[0]:
                ent = base + int(i) * 8
                comp = self.mem.u64(ent + HEALTH_OFF)
                if comp and self.mem.type_name(comp) == HEALTH_NAME and plausible(self.read_pos(ent)):
                    return ent
        return None

    def resolve(self, ent, path):
        addr = ent + path[0]
        for off in path[1:]:
            p = self.mem.u64(addr)
            if not p:
                return None
            addr = p + off
        return addr

    def pos_addr(self, ent):
        return self.resolve(ent, POS_PATH)

    def cam_vec(self, addr, stride=CAM_STRIDE):
        """Direction horizontale de la caméra (x, 0, z) -> angle brut, ou None."""
        b = self.mem.read(addr, 2 * stride + 4) if addr else None
        if not b:
            return None
        x, y, z = (struct.unpack_from("<f", b, i * stride)[0] for i in range(3))
        if not all(v == v for v in (x, y, z)) or abs(y) > 0.002 or abs(x * x + z * z - 1) > 0.005:
            return None
        return math.atan2(x, z)

    def drop_cam(self):
        self.cam_addr = self.cam_key = self.cam_raw = self.cam_path = None
        self.cam_cands = []
        self.cam_phase = "idle"

    def scan_cam(self):
        """Essaie chaque chemin fixe : ceux qui donnent un vecteur de cap valide sont candidats."""
        found, seen = [], set()
        for path in CAM_STATIC:
            a = self.resolve(self.mem.base, path)
            for stride in (16, 4):
                if a and (a, stride) not in seen and self.cam_vec(a, stride) is not None:
                    seen.add((a, stride))
                    key = f"{path[0]:x}/{stride}"
                    if key in self.cam_trusted:            # déjà vérifié avant : direct
                        self.cam_path, self.cam_stride, self.cam_key = path, stride, key
                        self.cam_cands, self.cam_phase, self.cam_fail = [], "ok", 0
                        log(f"Caméra : jeu+{key} (déjà connue)")
                        return
                    found.append({"path": path, "s": stride, "k": f"{path[0]:x}/{stride}",
                                  "win": None, "big": 0, "good": 0})
        self.cam_cands = found
        self.cam_phase = "verify" if found else "idle"
        if found:
            log(f"Caméra : {len(found)} candidat(s), vérification pendant que tu tournes")

    def scan_heap(self, faith):
        """Cherche le bloc caméra du jeu par sa forme (relevée par camauto ET camfix) :
           ligne 0 : (x, 0, z, x)    direction horizontale
           ligne 1 : (0, 1, 0, 0)    vertical
           ligne 2 : (x', 0, z', x') direction horizontale
           ligne 3 : (px, py, pz, px) position de la caméra, sur Faith"""
        found = []
        try:
            for base, size in self.mem.heap_chunks():
                if self.faith != faith:
                    return
                p = self.pos
                if not p or size < 64:
                    continue
                arr = self.mem.u64_array(base, size - size % 16)
                if arr is None or arr.size < 8:
                    continue
                R = arr.view(np.float32).reshape(-1, 4)
                r0, r1, r2, r3 = R[:-3], R[1:-2], R[2:-1], R[3:]
                with np.errstate(invalid="ignore", over="ignore"):
                    ok = (r1[:, 1] == 1.0) & (r1[:, 0] == 0.0) & (r1[:, 2] == 0.0) & (r1[:, 3] == 0.0)
                    idx = np.nonzero(ok)[0]
                    if not idx.size:
                        continue
                    a, c, e = r0[idx], r2[idx], r3[idx]
                    ok = (np.abs(a[:, 1]) < 1e-4) & (np.abs(c[:, 1]) < 1e-4) \
                        & (np.abs(a[:, 3] - a[:, 0]) < 1e-5) & (np.abs(e[:, 3] - e[:, 0]) < 1e-5) \
                        & (np.abs(a[:, 0] ** 2 + a[:, 2] ** 2 - 1) < 0.002) \
                        & (np.abs(e[:, 0] - p[0]) < 4) & (np.abs(e[:, 1] - p[1]) < 4) & (np.abs(e[:, 2] - p[2]) < 4)
                rows = idx[ok]
                rs = set(int(j) for j in rows)
                for j in rows:
                    j = int(j)
                    k = 1 if (j - 4) in rs else 0          # 1er ou 2e bloc d'une même série
                    found.append({"addr": base + j * 16, "s": 4, "k": f"mat{k}", "win": None, "big": 0, "good": 0})
                if len(found) > 200:
                    break
        except Exception as e:
            log(f"Caméra : erreur de recherche {e}")
        if self.faith == faith and self.cam_path is None:
            found.sort(key=lambda c: c["k"])
            m0 = [c for c in found if c["k"] == "mat0"]
            if len(m0) == 1 and "mat0" in self.cam_trusted:     # déjà vérifiée et calibrée avant
                self.cam_path, self.cam_addr, self.cam_stride, self.cam_key = "direct", m0[0]["addr"], 4, "mat0"
                self.cam_cands, self.cam_phase, self.cam_fail = [], "ok", 0
                log(f"Caméra : bloc retrouvé directement ({m0[0]['addr']:#x})")
                return
            self.cam_cands = found[:200]
            self.cam_phase = "verify" if found else "idle"
            log(f"Caméra : {len(found)} bloc(s) caméra trouvé(s) par leur forme")

    def read_cam_mgr(self):
        """Caméra exacte (sans calibration) : renvoie le cap, ou None."""
        a = self.resolve(self.mem.base, CAM_MGR_PATH)
        b = self.mem.read(a, 48) if a else None
        if not b:
            return None
        m = struct.unpack("<12f", b)
        for r in range(3):
            n = m[r * 4] ** 2 + m[r * 4 + 1] ** 2 + m[r * 4 + 2] ** 2
            if not (n == n and 0.95 < n < 1.05):
                return None
        fx, fz = m[8], m[10]
        if fx * fx + fz * fz < 0.01:          # regard tout droit vers le haut/bas : cap indéfini
            return None
        self.cam_addr, self.cam_stride = a + 0x20, 4          # ligne "forward" (pour l'overlay)
        return math.atan2(-fx, -fz)

    def read_cam(self):
        now = time.time()
        raw = self.read_cam_mgr()
        self.cam_exact = raw is not None
        if self.cam_exact:
            if self.cam_key != "mgr":
                log("Caméra : caméra finale du jeu (exacte)")
            self.cam_raw, self.cam_key, self.cam_phase = raw, "mgr", "ok"
            return
        if self.cam_key == "mgr":
            self.cam_key, self.cam_raw = None, None
        # 1) caméra déjà vérifiée : lecture directe
        if self.cam_path is not None:
            if self.cam_path != "direct":
                self.cam_addr = self.resolve(self.mem.base, self.cam_path)
            raw = self.cam_vec(self.cam_addr, self.cam_stride)
            self.cam_fail = 0 if raw is not None else self.cam_fail + 1
            if self.cam_fail > 120:                       # plus valide : on recherche
                log("Caméra perdue, nouvelle recherche")
                self.drop_cam()
            self.cam_raw = raw
            return
        self.cam_raw = None
        # 2) lancer une recherche si besoin (en arrière-plan, le radar continue de marcher)
        if not self.cam_cands and now > self.cam_next_scan and self.pos:
            self.cam_next_scan = now + 20.0
            self.scan_cam()                      # 1) adresses fixes (instantané)
            if not self.cam_cands and self.cam_path is None and \
                    (self.cam_thread is None or not self.cam_thread.is_alive()):
                self.cam_phase = "scan"          # 2) recherche par la forme (quelques secondes)
                self.cam_thread = threading.Thread(target=self.scan_heap, args=(self.faith,), daemon=True)
                self.cam_thread.start()
            return
        # 3) vérifier les candidats : quand le corps de Faith tourne, la caméra doit tourner aussi
        if not self.cam_cands or self.yaw_raw is None or now < getattr(self, "cam_next_verify", 0):
            return
        self.cam_next_verify = now + 0.1
        keep = []
        for c in self.cam_cands:
            a = c["addr"] if "addr" in c else self.resolve(self.mem.base, c["path"])
            raw = self.cam_vec(a, c["s"])
            if raw is None:
                continue
            if c["win"] is None:
                c["win"] = (now, self.yaw_raw, raw)
            elif now - c["win"][0] > 2.0:
                ds = abs(wrap_angle(self.yaw_raw - c["win"][1]))
                dc = abs(wrap_angle(raw - c["win"][2]))
                if ds > math.radians(40):
                    c["big"] += 1
                    c["good"] += dc > math.radians(15)
                c["win"] = (now, self.yaw_raw, raw)
                if c["good"] >= 4 and c["good"] >= c["big"] - 1:
                    if "addr" in c:
                        self.cam_path, self.cam_addr = "direct", c["addr"]
                    else:
                        self.cam_path = c["path"]
                    self.cam_stride, self.cam_key = c["s"], c["k"]
                    self.cam_cands, self.cam_phase, self.cam_fail = [], "ok", 0
                    log(f"Caméra vérifiée : {c['k']} ({a:#x})")
                    return
                if c["big"] >= 4 and c["good"] <= c["big"] // 3:
                    continue                                # ne suit pas : écarté
            keep.append(c)
        self.cam_cands = keep
        if not keep:
            self.cam_phase = "idle"

    def read_pos(self, ent):
        addr = self.pos_addr(ent)
        b = self.mem.read(addr, 12) if addr else None
        return struct.unpack("<3f", b) if b else None

    def read_yaw(self, ent):
        """Une ligne de rotation d'un os du squelette (juste avant la position) -> angle brut."""
        addr = self.pos_addr(ent)
        b = self.mem.read(addr + YAW_ROW, 12) if addr else None
        if not b:
            return None
        x, y, z = struct.unpack("<3f", b)
        if not all(v == v for v in (x, y, z)) or math.hypot(x, z) < 0.3:
            return None
        return math.atan2(x, z)

    def run(self):
        while True:
            try:
                self.step()
            except Exception as e:      # le jeu peut se fermer à tout moment
                self.status = f"Erreur : {e}"
                self.faith = None
                time.sleep(1)

    def step(self):
        if self.mem is None:
            self.mem = Mem()
        if not self.mem.h or not self.mem.alive():
            self.faith, self.pos = None, None
            err = self.mem.open()
            if err:
                self.status = err
                time.sleep(2)
                return
            log_dir(self.mem.game_dir)
        if self.faith is None:
            self.status = "Recherche de Faith..."
            self.faith = self.find_faith()
            if self.faith is None:
                self.status = "Faith introuvable (es-tu en partie ?)"
                time.sleep(3)
                return
            log(f"Faith trouvée : {self.faith:#x}")
            self.drop_cam()
        p = self.read_pos(self.faith)
        if plausible(p):
            self.pos, self.lost_since, self.status = p, None, "OK"
            self.yaw_raw = self.read_yaw(self.faith)
            self.read_cam()
        else:
            self.lost_since = self.lost_since or time.time()
            if time.time() - self.lost_since > 2:   # chargement, mort... on la recherche
                log("Faith perdue, nouvelle recherche")
                self.faith, self.pos = None, None
        time.sleep(0.008)


# ======================================================================================
#  Poses du corps (personnages 3D) : échange avec l'overlay par une 2e mémoire partagée
#  Ma pose y est écrite par l'overlay ; les poses des autres y sont écrites par coop.
# ======================================================================================
POSE_NAME = "Local\\MECCoopPose_v12"
MAX_BONES = 32
POSE_LOCAL = struct.Struct("<IIIffffI")          # seq, valide, heure, x, y, z, cap, nb d'os (32 o)
POSE_REMOTE = struct.Struct("<I32sIIIffffI")     # id, nom, n° de paquet, apparence, heure, x, y, z, cap, nb d'os
BONES_SIZE = MAX_BONES * 32
OFF_LOCAL = 8
OFF_RSEQ = OFF_LOCAL + POSE_LOCAL.size + BONES_SIZE
OFF_R = OFF_RSEQ + 8
OFF_SSEQ = OFF_R + 8 * (POSE_REMOTE.size + BONES_SIZE)
OFF_STATUS = OFF_SSEQ + 4
OFF_CTRL = OFF_STATUS + 200
# contrôle : rôle, suivre l'heure, heure de l'hôte, mon heure, demande de téléportation (n°, joueur, position),
# demande traitée, résultat, état de l'heure
POSE_CTRL = struct.Struct("<IIffII3fII64s")
OFF_MS = OFF_CTRL + POSE_CTRL.size              # v4 : ma mission (écrite par l'overlay)
POSE_MS = struct.Struct("<III3f64s32s")
OFF_IV = OFF_MS + POSE_MS.size                   # v4 : invitation à suivre un autre en mission (écrite par coop)
POSE_IV = struct.Struct("<I3f96sI")
OFF_AUD = OFF_IV + POSE_IV.size                  # v5 : sons (aEnabled, aOutHead, aOut[64], aInHead, aIn[128])
AUD_SAMPLE = struct.Struct("<IIBBH16I16f")       # heure, événement, source, nb de valeurs, -, clés, valeurs
AUD_OUT = OFF_AUD + 8
AUD_IN_HEAD = AUD_OUT + 64 * AUD_SAMPLE.size
AUD_IN = AUD_IN_HEAD + 4
CB_EVENT = struct.Struct("<6fI")                 # v6 : un coup sur un ennemi (position, santé, santé max, dégâts, -)
CB_OUT_HEAD = AUD_IN + 128 * (4 + AUD_SAMPLE.size)
CB_OUT = CB_OUT_HEAD + 4
CB_IN_HEAD = CB_OUT + 32 * CB_EVENT.size
CB_IN = CB_IN_HEAD + 4
OFF_GS = CB_IN + 32 * CB_EVENT.size              # v7 : état du jeu (0 rien, 1 menu principal, 2 en partie)
OFF_NOTICE = OFF_GS + 4                           # v7 : n°, puis message affiché en haut de l'écran du jeu (128 o)
OFF_MENU = OFF_NOTICE + 4 + 128                   # v8 : +1 = demander au jeu de revenir au menu principal
PG_FLAG = struct.Struct("<Ii")                   # v10 : un drapeau de progression (empreinte, valeur)
PG_OUT_HEAD = OFF_MENU + 8                        # v9 : + genre de demande (0 menu, 1 recharger la sauvegarde)
PG_OUT = PG_OUT_HEAD + 4                          # v10 : mes changements de drapeaux (écrits par l'overlay)
PG_IN_HEAD = PG_OUT + 256 * PG_FLAG.size
PG_IN = PG_IN_HEAD + 4                            # v10 : drapeaux de l'hôte à appliquer (écrits par coop)
PG_STATS = PG_IN + 512 * PG_FLAG.size             # v10 : appliqués, inconnus (overlay)
PG_SNAP = PG_STATS + 8                            # v11 : +1 = l'overlay envoie toute ma table de drapeaux
PG_SNAP_DONE = PG_SNAP + 4                        # v12 : n° de la dernière table entièrement envoyée, puis son nombre
PG_IN_DONE = PG_SNAP + 12                         # v12 : drapeaux de l'hôte déjà traités par l'overlay
POSE_SIZE = PG_SNAP + 16
TRANS_SCALE = 2.05                               # les os bougent d'au plus 2 m par rapport à leur parent


def name_id(name):
    """Identifiant d'un joueur : FNV-1a de son nom (même calcul que l'overlay)."""
    h = 2166136261
    for c in name.encode("utf-8")[:31]:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h | 1


def _q16(v):
    return max(-32767, min(32767, int(round(v * 32767))))


def encode_pose(lp):
    """Ma pose -> texte compact (environ 460 caractères pour 23 os)."""
    bones = lp["bones"]
    out = bytearray(struct.pack("<IffffB", lp["t"] & 0xFFFFFFFF, *lp["pos"], lp["yaw"], len(bones)))
    for i, b in enumerate(bones):
        tx, ty, tz = (0.0, 0.0, 0.0) if i == 1 else (b[0], b[1], b[2])   # os 1 = l'ancre (= la position)
        out += struct.pack("<7h", _q16(tx / TRANS_SCALE), _q16(ty / TRANS_SCALE), _q16(tz / TRANS_SCALE),
                           _q16(b[4]), _q16(b[5]), _q16(b[6]), _q16(b[7]))
    return base64.b64encode(bytes(out)).decode("ascii")


def decode_pose(text):
    try:
        raw = base64.b64decode(text)
        t, x, y, z, yaw, nb = struct.unpack_from("<IffffB", raw, 0)
        if nb > MAX_BONES or len(raw) < 21 + nb * 14:
            return None
        bones = []
        for i in range(nb):
            v = struct.unpack_from("<7h", raw, 21 + i * 14)
            bones.append((v[0] / 32767 * TRANS_SCALE, v[1] / 32767 * TRANS_SCALE, v[2] / 32767 * TRANS_SCALE, 1.0,
                          v[3] / 32767, v[4] / 32767, v[5] / 32767, v[6] / 32767))
        if not all(math.isfinite(a) for a in (x, y, z, yaw)):
            return None
        return {"t": t, "pos": (x, y, z), "yaw": yaw, "bones": bones}
    except (ValueError, struct.error):
        return None


class PoseLink:
    def __init__(self):
        import mmap
        self.mm = mmap.mmap(-1, POSE_SIZE, tagname=POSE_NAME)
        struct.pack_into("<II", self.mm, 0, 0x5043454D, 3)
        self.rseq = struct.unpack_from("<I", self.mm, OFF_RSEQ)[0] & ~1
        self.lock = threading.Lock()

    def local_seq(self):
        """N° de ma dernière pose (pair = complète) : lecture de 4 octets, pour savoir s'il y a du neuf."""
        return struct.unpack_from("<I", self.mm, OFF_LOCAL)[0]

    def local(self):
        """Ma dernière pose (écrite par l'overlay), ou None."""
        for _ in range(4):
            s1 = struct.unpack_from("<I", self.mm, OFF_LOCAL)[0]
            if s1 & 1:
                continue
            raw = self.mm[OFF_LOCAL:OFF_LOCAL + POSE_LOCAL.size + BONES_SIZE]
            if struct.unpack_from("<I", self.mm, OFF_LOCAL)[0] != s1:
                continue
            _, valid, t, x, y, z, yaw, nb = POSE_LOCAL.unpack_from(raw, 0)
            if not valid or nb > MAX_BONES:
                return None
            f = struct.unpack_from(f"<{nb * 8}f", raw, POSE_LOCAL.size)
            return {"t": t, "pos": (x, y, z), "yaw": yaw, "bones": [f[i * 8:i * 8 + 8] for i in range(nb)]}
        return None

    def write_remotes(self, players):
        """players : liste de {"name", "rx", "look", "t", "pos", "yaw", "bones"} (au plus 8)."""
        players = players[:8]
        body = bytearray()
        for p in players:
            nb = min(len(p["bones"]), MAX_BONES)
            body += POSE_REMOTE.pack(name_id(p["name"]), p["name"].encode("utf-8")[:31], p["rx"] & 0xFFFFFFFF,
                                     int(p.get("look", 1)), p["t"] & 0xFFFFFFFF, *p["pos"], p["yaw"], nb)
            bones = bytearray(BONES_SIZE)
            for i in range(nb):
                struct.pack_into("<8f", bones, i * 32, *p["bones"][i])
            body += bones
        with self.lock:
            self.rseq = (self.rseq + 2) & 0xFFFFFFFE
            struct.pack_into("<I", self.mm, OFF_RSEQ, self.rseq + 1)        # écriture en cours
            struct.pack_into("<I", self.mm, OFF_RSEQ + 4, len(players))
            self.mm[OFF_R:OFF_R + len(body)] = bytes(body)
            struct.pack_into("<I", self.mm, OFF_RSEQ, self.rseq)            # terminé

    def status(self):
        raw = self.mm[OFF_STATUS:OFF_STATUS + 200]
        return raw.split(b"\0")[0].decode("utf-8", "replace")

    # --- contrôle : heure du jour et téléportation
    def set_role(self, role, follow=True):
        """0 rien, 1 hôte, 2 rejoint, 3 test solo."""
        struct.pack_into("<II", self.mm, OFF_CTRL, role, 1 if follow else 0)
        if role != 2:
            self.set_host_tod(-1.0)

    def set_host_tod(self, t):
        struct.pack_into("<f", self.mm, OFF_CTRL + 8, float(t))

    def my_tod(self):
        return struct.unpack_from("<f", self.mm, OFF_CTRL + 12)[0]

    def teleport(self, peer_id, pos):
        req = (struct.unpack_from("<I", self.mm, OFF_CTRL + 16)[0] + 1) & 0xFFFFFFFF
        struct.pack_into("<I3f", self.mm, OFF_CTRL + 20, peer_id, *(pos or (0.0, 0.0, 0.0)))
        struct.pack_into("<I", self.mm, OFF_CTRL + 16, req)          # en dernier : la demande est complète
        return req

    def teleport_state(self):
        """(n° de la dernière demande prise en compte, résultat : 0 en cours, 1 réussie, 2 échouée)"""
        return struct.unpack_from("<II", self.mm, OFF_CTRL + 36)

    def game_state(self):
        """0 : pas de niveau (chargement...), 1 : menu principal, 2 : en partie. None : overlay absent."""
        return struct.unpack_from("<I", self.mm, OFF_GS)[0]

    def request_menu(self, kind=1):
        """Sans quitter le jeu : 0 = retour au menu principal, 1 = recharger la sauvegarde (le monde repart du fichier)."""
        struct.pack_into("<I", self.mm, OFF_MENU + 4, kind)
        struct.pack_into("<I", self.mm, OFF_MENU, (struct.unpack_from("<I", self.mm, OFF_MENU)[0] + 1) & 0xFFFFFFFF)

    def set_notice(self, text):
        """Message affiché en haut de l'écran du jeu ("" = aucun)."""
        raw = text.encode("utf-8")[:127]
        self.mm[OFF_NOTICE + 4:OFF_NOTICE + 4 + 128] = raw + b"\0" * (128 - len(raw))
        struct.pack_into("<I", self.mm, OFF_NOTICE, (struct.unpack_from("<I", self.mm, OFF_NOTICE)[0] + 1) & 0xFFFFFFFF)

    def set_sounds(self, on):
        struct.pack_into("<I", self.mm, OFF_AUD, 1 if on else 0)

    def audio_out(self):
        """Les sons de mon personnage écrits par l'overlay depuis le dernier appel : [[t, ev, src, [k, v, ...]], ...]"""
        head = struct.unpack_from("<I", self.mm, OFF_AUD + 4)[0]
        last = getattr(self, "_aud_last", None)
        if last is None or (head - last) & 0xFFFFFFFF > 64:
            last = head if last is None else (head - 64) & 0xFFFFFFFF
        out = []
        while last != head:
            v = AUD_SAMPLE.unpack_from(self.mm, AUD_OUT + (last % 64) * AUD_SAMPLE.size)
            n = min(v[3], 16)
            kv = []
            for i in range(n):
                kv += [v[5 + i], round(v[21 + i], 4)]
            out.append([v[0], v[1], v[2], kv])
            last = (last + 1) & 0xFFFFFFFF
        self._aud_last = last
        return out

    def audio_in(self, name, samples):
        """Sons reçus d'un autre joueur -> overlay (qui les joue sur son personnage 3D)."""
        pid = name_id(name)
        head = struct.unpack_from("<I", self.mm, AUD_IN_HEAD)[0]
        for smp in samples[:32]:
            try:
                t, ev, src, kv = smp
                n = min(len(kv) // 2, 16)
                keys = [int(kv[2 * i]) & 0xFFFFFFFF for i in range(n)] + [0] * (16 - n)
                vals = [float(kv[2 * i + 1]) for i in range(n)] + [0.0] * (16 - n)
                raw = AUD_SAMPLE.pack(int(t) & 0xFFFFFFFF, int(ev) & 0xFFFFFFFF, int(src) & 0xFF, n, 0, *keys, *vals)
            except (ValueError, TypeError, struct.error):
                continue
            off = AUD_IN + (head % 128) * (4 + AUD_SAMPLE.size)
            struct.pack_into("<I", self.mm, off, pid)
            self.mm[off + 4:off + 4 + AUD_SAMPLE.size] = raw
            head = (head + 1) & 0xFFFFFFFF
        struct.pack_into("<I", self.mm, AUD_IN_HEAD, head)

    def combat_out(self):
        """Mes coups sur des ennemis depuis le dernier appel : [[x, y, z, santé, santé max, dégâts], ...]"""
        head = struct.unpack_from("<I", self.mm, CB_OUT_HEAD)[0]
        last = getattr(self, "_cb_last", None)
        if last is None or (head - last) & 0xFFFFFFFF > 32:
            last = head if last is None else (head - 32) & 0xFFFFFFFF
        out = []
        while last != head:
            v = CB_EVENT.unpack_from(self.mm, CB_OUT + (last % 32) * CB_EVENT.size)
            out.append([round(x, 2) for x in v[:6]])
            last = (last + 1) & 0xFFFFFFFF
        self._cb_last = last
        return out

    def combat_in(self, name, events):
        """Coups d'un autre joueur -> overlay (qui les applique aux mêmes ennemis chez moi)."""
        pid = name_id(name)
        head = struct.unpack_from("<I", self.mm, CB_IN_HEAD)[0]
        for ev in events[:16]:
            try:
                vals = [float(x) for x in ev[:6]]
                if len(vals) != 6 or not all(math.isfinite(x) for x in vals):
                    continue
                CB_EVENT.pack_into(self.mm, CB_IN + (head % 32) * CB_EVENT.size, *vals, pid)
            except (ValueError, TypeError, struct.error):
                continue
            head = (head + 1) & 0xFFFFFFFF
        struct.pack_into("<I", self.mm, CB_IN_HEAD, head)

    def prog_out(self):
        """Mes changements de drapeaux de progression depuis le dernier appel : [[empreinte, valeur], ...]
        (un petit fil vide la mémoire partagée toutes les 3 ms : une table entière n'y déborde jamais)"""
        if not getattr(self, "_drain_on", False):
            self._drain_on, self._buf, self._buf_lock = True, [], threading.Lock()

            def drain():
                while True:
                    try:
                        got = self._read_out()
                    except Exception:               # noqa: BLE001
                        got = []
                    if got:
                        with self._buf_lock:
                            self._buf.extend(got)
                    time.sleep(0.003)
            threading.Thread(target=drain, daemon=True).start()
        with self._buf_lock:
            out, self._buf = self._buf, []
        return out

    def _read_out(self):
        head = struct.unpack_from("<I", self.mm, PG_OUT_HEAD)[0]
        last = getattr(self, "_pg_last", None)
        if last is None or (head - last) & 0xFFFFFFFF > 256:
            last = head if last is None else (head - 256) & 0xFFFFFFFF
        out = []
        while last != head:
            out.append(list(PG_FLAG.unpack_from(self.mm, PG_OUT + (last % 256) * PG_FLAG.size)))
            last = (last + 1) & 0xFFFFFFFF
        self._pg_last = last
        return out

    def prog_in(self, flags):
        """Drapeaux de l'hôte -> file d'attente (prog_feed les donne à l'overlay sans déborder sa mémoire)."""
        if not hasattr(self, "_pg_pending"):
            self._pg_pending, self._pg_fed = [], 0.0
        self._pg_pending.extend(flags[:256])

    def prog_feed(self):
        """Au plus 128 drapeaux toutes les 50 ms (l'overlay en applique jusqu'à 256 par image du jeu)."""
        pending = getattr(self, "_pg_pending", None)
        if not pending or time.time() - self._pg_fed < 0.05:
            return
        self._pg_fed = time.time()
        flags, self._pg_pending = pending[:128], pending[128:]
        head = struct.unpack_from("<I", self.mm, PG_IN_HEAD)[0]
        for f in flags:
            try:
                h, v = int(f[0]) & 0xFFFFFFFF, int(f[1])
                if not -2**31 <= v < 2**31:
                    continue
                PG_FLAG.pack_into(self.mm, PG_IN + (head % 512) * PG_FLAG.size, h, v)
            except (ValueError, TypeError, IndexError, struct.error):
                continue
            head = (head + 1) & 0xFFFFFFFF
        struct.pack_into("<I", self.mm, PG_IN_HEAD, head)

    def request_snapshot(self):
        """L'overlay envoie toute ma table de drapeaux (par morceaux). Renvoie le n° de la demande."""
        req = (struct.unpack_from("<I", self.mm, PG_SNAP)[0] + 1) & 0xFFFFFFFF
        struct.pack_into("<I", self.mm, PG_SNAP, req)
        return req

    def snapshot_done(self):
        """(n° de la dernière table entièrement envoyée par l'overlay, nombre de drapeaux)"""
        return struct.unpack_from("<II", self.mm, PG_SNAP_DONE)

    def prog_in_all_done(self):
        """Vrai quand l'overlay a traité tous les drapeaux de l'hôte qu'on lui a donnés."""
        return (not getattr(self, "_pg_pending", None)
                and struct.unpack_from("<I", self.mm, PG_IN_DONE)[0] == struct.unpack_from("<I", self.mm, PG_IN_HEAD)[0])

    def prog_stats(self):
        """(drapeaux de l'hôte appliqués, drapeaux inconnus de mon jeu)"""
        return struct.unpack_from("<II", self.mm, PG_STATS)

    def my_mission(self):
        """Ma mission en cours d'après le jeu : (n°, nom, position de départ) ou None."""
        seq, active, have, x, y, z, name, kind = POSE_MS.unpack_from(self.mm, OFF_MS)
        if not seq or not active:
            return None
        name = name.split(b"\0")[0].decode("utf-8", "replace")
        return {"seq": seq, "name": name, "pos": (x, y, z) if have else None}

    def set_invite(self, text, pos):
        seq = (struct.unpack_from("<I", self.mm, OFF_IV)[0] + 1) & 0xFFFFFFFF
        struct.pack_into("<3f96s", self.mm, OFF_IV + 4, *pos, text.encode("utf-8")[:95])
        struct.pack_into("<I", self.mm, OFF_IV, seq)
        return seq

    def clear_invite(self):
        struct.pack_into("<96s", self.mm, OFF_IV + 16, b"")

    def invite_taken(self):
        seq, _x, _y, _z, text, taken = POSE_IV.unpack_from(self.mm, OFF_IV)
        return bool(seq) and taken == seq

    def tod_status(self):
        raw = self.mm[OFF_CTRL + 44:OFF_CTRL + 108]
        return raw.split(b"\0")[0].decode("utf-8", "replace")


POSE = None          # créé par l'appli (PoseLink), utilisé par le réseau et le test solo
LOOKS = ["Faith", "Fantôme"]   # apparence que les autres voient de toi : 1 = Faith, 2 = fantôme rouge
MY_LOOK = 1
ECHO_LOOK = 1                  # apparence de ton clone (Écho du test solo, faux_pote)


# ======================================================================================
#  Réseau (UDP) : un joueur héberge, les autres se connectent à lui
# ======================================================================================
class Server(threading.Thread):
    def __init__(self, port=PORT):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))      # peut lever OSError si le port est pris
        self.sock.settimeout(0.5)
        self.players = {}          # adresse -> {"n": nom, "p": [x, y, z], "t": heure}
        self.running = True
        self.save_name = "l'hôte"
        self.blobs = {}            # crc -> (heure, octets compressés) : la progression envoyée par morceaux
        self.sent_to = set()

    def serve_save(self, msg, addr):
        """Partage de la progression par le MÊME port que le jeu (celui qui marche déjà à travers le pare-feu)."""
        import zlib
        now = time.time()
        self.blobs = {k: v for k, v in list(self.blobs.items()) if now - v[0] < 120}
        if msg.get("sv") == "get":
            data = read_my_save()
            if not data:
                self.sock.sendto(json.dumps({"svh": 0}).encode("utf-8"), addr)
                return
            z = zlib.compress(data, 9)
            crc = zlib.crc32(z)
            self.blobs[crc] = (now, z)
            head = {"svh": 1, "name": self.save_name, "size": len(data), "z": len(z), "crc": crc,
                    "n": (len(z) + SAVE_CHUNK - 1) // SAVE_CHUNK}
            self.sock.sendto(json.dumps(head).encode("utf-8"), addr)
        elif msg.get("sv") == "part":
            blob = self.blobs.get(msg.get("crc"))
            idx = msg.get("i")
            if not blob or not isinstance(idx, list):
                return
            z = blob[1]
            for i in idx[:32]:
                if isinstance(i, int) and 0 <= i * SAVE_CHUNK < len(z):
                    self.sock.sendto(b"SVC" + struct.pack("<II", msg["crc"], i) + z[i * SAVE_CHUNK:(i + 1) * SAVE_CHUNK],
                                     addr)
            if (addr, msg["crc"]) not in self.sent_to and (len(z) - 1) // SAVE_CHUNK in idx:
                self.sent_to.add((addr, msg["crc"]))
                log(f"Progression envoyée à {addr[0]} ({len(z) // 1024} Ko)")

    def _serve_safe(self, msg, addr):
        try:
            self.serve_save(msg, addr)
        except (OSError, ValueError, TypeError):
            pass

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass

    def run(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(8192)
                msg = json.loads(data.decode("utf-8"))
            except (socket.timeout, ValueError, ConnectionResetError):
                continue
            except OSError:
                if not self.running:
                    return
                continue
            if not isinstance(msg, dict):
                continue
            if "sv" in msg:
                if msg.get("sv") == "get":         # lecture du fichier (jusqu'à ~1 s) : à côté, pour ne pas figer le relais
                    threading.Thread(target=self._serve_safe, args=(msg, addr), daemon=True).start()
                else:
                    self._serve_safe(msg, addr)
                continue
            if isinstance(msg.get("cb"), list):         # des coups sur les ennemis : relayés tout de suite
                if addr in self.players and len(msg["cb"]) <= 16:
                    out = json.dumps({"n": self.players[addr]["n"], "cb": msg["cb"]}).encode("utf-8")
                    for a in list(self.players):
                        if a != addr:
                            try:
                                self.sock.sendto(out, a)
                            except OSError:
                                pass
                continue
            if msg.get("pgreq") and addr in self.players:   # un joueur qui rejoint demande toute la progression
                out = json.dumps({"n": self.players[addr]["n"], "pgreq": 1}).encode("utf-8")
                for a in list(self.players):
                    if a != addr and a[0] == "127.0.0.1":
                        try:
                            self.sock.sendto(out, a)
                        except OSError:
                            pass
                continue
            if isinstance(msg.get("pg"), list):         # progression : seulement celle de l'hôte (son coop est ici)
                if addr in self.players and addr[0] == "127.0.0.1" and len(msg["pg"]) <= 128:
                    out = json.dumps({"n": self.players[addr]["n"], "pg": msg["pg"]}).encode("utf-8")
                    for a in list(self.players):
                        if a != addr:
                            try:
                                self.sock.sendto(out, a)
                            except OSError:
                                pass
                continue
            if isinstance(msg.get("a"), list):          # des sons : relayés tout de suite aux autres
                if addr in self.players and len(msg["a"]) <= 32:
                    out = json.dumps({"n": self.players[addr]["n"], "a": msg["a"]}).encode("utf-8")
                    for a in list(self.players):
                        if a != addr:
                            try:
                                self.sock.sendto(out, a)
                            except OSError:
                                pass
                continue
            now = time.time()
            self.players = {a: v for a, v in self.players.items() if now - v["t"] < 5}
            if addr not in self.players and len(self.players) >= MAX_PLAYERS:
                continue
            prev = self.players.get(addr, {})
            e = dict(prev)
            e["n"], e["t"] = str(msg.get("n", prev.get("n", "?")))[:16], now
            if msg.get("l") in (1, 2):
                e["l"] = msg["l"]
            q = msg.get("q") if isinstance(msg.get("q"), str) and len(msg.get("q")) < 1400 else None
            if q:
                e["q"] = q
            pose_only = q is not None and "p" not in msg
            if not pose_only:                          # l'état (position radar, heure, mission) : ~30 fois/s
                e["p"] = msg.get("p")
                e["tod"] = msg.get("tod") if isinstance(msg.get("tod"), (int, float)) else None
                ms = msg.get("ms")
                if not (isinstance(ms, list) and len(ms) == 5 and isinstance(ms[0], int) and isinstance(ms[1], str)
                        and all(isinstance(v, (int, float)) for v in ms[2:])):
                    ms = None
                e["ms"] = [ms[0], ms[1][:63]] + ms[2:] if ms else None
            e.setdefault("l", 1)
            self.players[addr] = e
            try:
                # la pose du corps : relayée TOUT DE SUITE aux autres (pas au prochain message de chacun)
                if q:
                    out = json.dumps({"n": e["n"], "q": q, "l": e.get("l", 1)}).encode("utf-8")
                    for a in self.players:
                        if a != addr:
                            self.sock.sendto(out, a)
                if not pose_only:
                    state = []
                    for a, v in self.players.items():
                        st = {"n": v["n"], "p": v.get("p")}
                        if v.get("ms"):
                            st["ms"] = v["ms"]
                        if a[0] == "127.0.0.1":            # le joueur qui héberge (son coop est sur cette machine)
                            st["h"] = 1
                            if v.get("tod") is not None:
                                st["tod"] = v["tod"]
                        state.append(st)
                    self.sock.sendto(json.dumps(state).encode("utf-8"), addr)
            except OSError:
                pass


def mission_title(name):
    """Nom de mission du jeu -> texte lisible (le nom interne, sans préfixe ni soulignés)."""
    n = name.rsplit("/", 1)[-1]
    for pre in ("MI_", "mi_", "Mission_", "mission_", "M_"):
        if n.startswith(pre):
            n = n[len(pre):]
    return n.replace("_", " ").strip() or name


class MissionFollow:
    """Suivre en mission : quand un autre joueur commence une mission, une invitation s'affiche dans le jeu
    (Alt+M = téléportation au départ de la mission ; on la lance soi-même, avec ses cinématiques)."""

    def __init__(self):
        self.inv = None          # (joueur, n° de mission, nom) de l'invitation affichée
        self.seen = set()        # invitations déjà proposées (une seule fois chacune)
        self.taken_at = 0.0
        self.text = ""           # pour la fenêtre de coop

    def update(self, others, pose):
        mine = pose.my_mission()
        active = {}
        for o in others:
            ms = o.get("ms")
            if isinstance(ms, list) and len(ms) == 5:
                active[o.get("n")] = ms
        if self.inv:
            who, seq, name = self.inv
            ms = active.get(who)
            same = mine and mine["name"] == name
            if not ms or ms[0] != seq or same:
                pose.clear_invite()
                self.text = f"Vous êtes dans la même mission que {who} ✓" if same else ""
                self.inv = None
            elif pose.invite_taken():
                self.taken_at = self.taken_at or time.time()
                self.text = f"Mission de {who} : lance-la au départ, tu y es."
                if time.time() - self.taken_at > 90:
                    pose.clear_invite()
            return
        for who, ms in active.items():
            key = (who, ms[0])
            if key in self.seen or (mine and mine["name"] == ms[1]):
                continue
            self.seen.add(key)
            self.inv = (who, ms[0], ms[1])
            self.taken_at = 0.0
            title = mission_title(ms[1])
            pose.set_invite(f"{who[:16]} a lancé la mission « {title[:40]} »", ms[2:5])
            self.text = f"{who} a lancé la mission « {title} » : Alt+M dans le jeu pour y aller."
            log(f"Invitation : {who} a lancé la mission {ms[1]} (départ {ms[2:5]})")
            return


class HostClock:
    """L'heure du jour de l'hôte, prolongée entre deux nouvelles : son jeu ne la donne qu'une fois par seconde
    (et elle arrive avec le retard du réseau). Sans ça, chez celui qui rejoint, l'heure était remise en arrière
    de ~20 s de jeu chaque seconde (le ciel « sautait »)."""
    DAY = 86400.0

    def __init__(self):
        self.value, self.at, self.rate = None, 0.0, 0.0

    def update(self, tod):
        now = time.time()
        if self.value is None or tod != self.value:
            if self.value is not None and self.at:
                d = tod - self.value
                if d < -self.DAY / 2:
                    d += self.DAY
                dt = now - self.at
                r = d / dt if dt > 0.2 else -1
                # vitesse plausible (le jeu va jusqu'à ~120 s de jeu par seconde) ; sinon : saut (mission, réglage)
                self.rate = r if 0 < r < 200 else 0.0
            self.value, self.at = tod, now
        dt = now - self.at
        if dt > 1.6:                     # plus de nouvelle : l'hôte est en pause / menu, on n'invente rien
            return self.value
        return (self.value + self.rate * dt) % self.DAY


class Client(threading.Thread):
    def __init__(self, name, host, game, port=PORT):
        super().__init__(daemon=True)
        self.name, self.addr, self.game = name, (host, port), game
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.05)
        self.others = []
        self.last_rx = 0
        self.running = True
        self.follow = MissionFollow()
        self.tod = HostClock()
        self.cb_resend = []
        self.pg_resend = []
        self.send_progress = False             # j'héberge : ma progression part chez les autres
        self.can_apply = lambda: False         # je rejoins avec le monde de l'hôte : j'applique sa progression
        self.pg_received = 0
        self.pg_last_rx = 0.0
        self.pg_applied_base = None
        self.pg_host_flags = {}                # (je rejoins) les drapeaux reçus de l'hôte
        self.own_state = 0                     # (je rejoins) ma propre table : 0 à relire, 1 en cours, 2 gardée
        self.own_flags = {}
        self.own_rx = 0.0
        self.on_own_flags = None               # appelé avec ma table (l'appli la range avec ma sauvegarde)
        self.own_saved = lambda: False         # vrai si ma table est déjà rangée (rejoindre une 2e fois)
        self.snapshot_ready = lambda: True     # (l'appli) le jeu est en partie, pas en train de charger
        self.own_req = None
        self.own_t = 0.0
        self.pg_verify = None                  # (je rejoins) ma table relue après le rechargement
        self.pg_verify_rx = 0.0
        self.pg_host = ""
        self.pg_asked = 0                      # demandes de la table complète envoyées
        self.pg_ask_at = 0.0
        self.pg_snap_at = 0.0                  # (hôte) dernière table envoyée

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass

    def run(self):
        last_seq = None
        poses = {}            # nom -> {"q": texte, "rx": n° de paquet, "at": heure d'arrivée}
        next_state = next_follow = 0.0
        self.sock.settimeout(0.004)
        last_err = 0.0
        while self.running:
            try:
                last_seq, next_state, next_follow = self.step(poses, last_seq, next_state, next_follow)
            except Exception as e:             # une erreur ne doit jamais couper le réseau
                if not self.running:
                    break
                if time.time() - last_err > 5:
                    last_err = time.time()
                    log(f"Erreur réseau : {e!r}")
                time.sleep(0.05)
        if POSE:
            POSE.write_remotes([])
            POSE.clear_invite()

    def step(self, poses, last_seq, next_state, next_follow):
        if True:
            now = time.time()
            # 1) ma pose : envoyée dès que l'overlay en a écrit une nouvelle (pas d'attente)
            seq = POSE.local_seq() if POSE else None
            if seq is not None and seq != last_seq and not (seq & 1):
                lp = POSE.local()
                if lp:
                    last_seq = seq
                    try:
                        self.sock.sendto(json.dumps({"n": self.name, "q": encode_pose(lp), "l": MY_LOOK}).encode("utf-8"),
                                         self.addr)
                    except OSError:
                        pass
            # 2) mon état (radar, heure, mission) : 30 fois/s
            if now >= next_state:
                next_state = now + 0.033
                p = self.game.pos
                msg = {"n": self.name, "p": [round(v, 2) for v in p] if p else None, "l": MY_LOOK}
                mt = POSE.my_tod() if POSE else -1.0
                if mt >= 0:
                    msg["tod"] = round(mt, 1)
                mine = POSE.my_mission() if POSE else None
                if mine and mine["pos"]:
                    msg["ms"] = [mine["seq"], mine["name"][:63]] + [round(v, 2) for v in mine["pos"]]
                try:
                    self.sock.sendto(json.dumps(msg).encode("utf-8"), self.addr)
                except OSError:
                    pass
            # 3) mes coups sur les ennemis (combats partagés)
            # (UDP peut perdre un paquet : chaque coup part 3 fois, à 0, 60 et 180 ms ; chez l'autre, recevoir
            # deux fois le même coup ne change rien — la santé ne descend jamais sous celle reçue)
            cb = POSE.combat_out() if POSE else []
            if cb:
                self.cb_resend.append([now + 0.06, cb[:16], 2])
            for item in [it for it in self.cb_resend if now >= it[0]]:
                item[2] -= 1
                item[0] = now + 0.12
                cb = cb + item[1] if cb else item[1]
            self.cb_resend = [it for it in self.cb_resend if it[2] > 0]
            if cb:
                try:
                    self.sock.sendto(json.dumps({"n": self.name, "cb": cb[:16]}).encode("utf-8"), self.addr)
                except OSError:
                    pass
            # 3a) ma progression (drapeaux de l'histoire) : envoyée 3 fois, appliquer deux fois ne change rien
            pg = POSE.prog_out() if POSE else []
            if pg and self.own_state == 1:             # ma propre table, avant toute progression de l'hôte
                for h, v in pg:
                    self.own_flags[h] = v
                self.own_rx = now
            if pg and self.pg_verify is not None:      # vérification : ma propre table, comparée à celle de l'hôte
                for h, v in pg:
                    self.pg_verify[h] = v
                self.pg_verify_rx = now
            if pg and self.send_progress:
                for i in range(0, len(pg), 100):
                    self.pg_resend.append([now, pg[i:i + 100], 3])
            for item in [it for it in self.pg_resend if now >= it[0]]:
                item[2] -= 1
                item[0] = now + 0.25
                try:
                    self.sock.sendto(json.dumps({"n": self.name, "pg": item[1]}).encode("utf-8"), self.addr)
                except OSError:
                    pass
            self.pg_resend = [it for it in self.pg_resend if it[2] > 0]
            if POSE and hasattr(POSE, "prog_feed"):
                POSE.prog_feed()
            # 3a bis) je rejoins avec le monde de l'hôte : d'abord je garde MA progression (relue dans le jeu : un
            # rechargement ne la remet pas depuis le fichier), puis je demande toute la sienne (3 fois, à 2 s)
            if not self.send_progress and self.own_state < 2 and self.connected and self.can_apply() and POSE:
                if self.own_saved():
                    self.own_state = 2
                elif self.own_state == 0:
                    # seulement en pleine partie (pas pendant un chargement) : la table entière, d'un coup
                    if self.snapshot_ready():
                        self.own_req = POSE.request_snapshot()
                        self.own_state, self.own_flags, self.own_t = 1, {}, now
                else:
                    done, count = POSE.snapshot_done()
                    if done == self.own_req and count and len(self.own_flags) >= count:
                        self.own_state = 2
                        if self.on_own_flags:
                            self.on_own_flags(dict(self.own_flags))
                    elif now - self.own_t > 8:   # (overlay qui vient d'arriver, chargement...) : on redemande
                        self.own_state = 0
            if (not self.send_progress and self.own_state == 2 and self.pg_asked < 3 and now >= self.pg_ask_at and self.connected
                    and not self.pg_received and self.can_apply()):
                if not self.pg_asked and POSE:
                    self.pg_applied_base = POSE.prog_stats()[0]   # avant tout drapeau de l'hôte
                self.pg_asked += 1
                self.pg_ask_at = now + 2.0
                try:
                    self.sock.sendto(json.dumps({"n": self.name, "pgreq": 1}).encode("utf-8"), self.addr)
                except OSError:
                    pass
            # 3b) mes sons : paquets à part, petits
            snd = POSE.audio_out() if POSE else []
            while snd:
                part, size = [], 40
                while snd and size < 1000:
                    smp = snd.pop(0)
                    size += len(json.dumps(smp)) + 1
                    part.append(smp)
                try:
                    self.sock.sendto(json.dumps({"n": self.name, "a": part}).encode("utf-8"), self.addr)
                except OSError:
                    pass
            # 4) tout ce qui arrive (attente de 4 ms au plus : c'est aussi le rythme de la boucle)
            got_pose = False
            first = True
            while True:
                try:
                    data, _ = self.sock.recvfrom(65536)
                except (socket.timeout, ConnectionResetError, BlockingIOError, OSError):
                    break
                finally:
                    if first:
                        first = False
                        self.sock.settimeout(0.0)
                try:
                    obj = json.loads(data.decode("utf-8"))
                except ValueError:
                    continue
                if isinstance(obj, list):
                    self.others = [o for o in obj if isinstance(o, dict) and o.get("n") != self.name]
                    self.last_rx = time.time()
                    if POSE:                       # l'heure de l'hôte (pour celui qui rejoint)
                        host = [o for o in self.others if o.get("h") and isinstance(o.get("tod"), (int, float))]
                        POSE.set_host_tod(self.tod.update(float(host[0]["tod"])) if host else -1.0)
                elif isinstance(obj, dict) and isinstance(obj.get("cb"), list) and obj.get("n") != self.name:
                    if POSE:
                        POSE.combat_in(str(obj.get("n"))[:16], obj["cb"])
                elif isinstance(obj, dict) and obj.get("pgreq") and obj.get("n") != self.name:
                    if POSE and self.send_progress and time.time() - self.pg_snap_at > 5:
                        self.pg_snap_at = time.time()
                        POSE.request_snapshot()
                        log(f"{obj.get('n')} demande toute ma progression : envoi")
                elif isinstance(obj, dict) and isinstance(obj.get("pg"), list) and obj.get("n") != self.name:
                    # seulement une fois MA table gardée : sinon les changements de l'hôte (il les envoie en direct
                    # à tout le monde) se mélangeraient à ma progression avant que je l'aie mise de côté (vu le 22/10)
                    if POSE and self.can_apply() and self.own_state == 2:
                        POSE.prog_in(obj["pg"])
                        for f in obj["pg"]:
                            if isinstance(f, list) and len(f) == 2:
                                self.pg_host_flags[f[0]] = f[1]
                        if not self.pg_received:
                            log(f"Progression de {obj.get('n')} : premiers drapeaux reçus")
                            self.pg_host = str(obj.get("n"))[:16]
                        self.pg_received += len(obj["pg"])
                        self.pg_last_rx = time.time()
                elif isinstance(obj, dict) and isinstance(obj.get("a"), list) and obj.get("n") != self.name:
                    if POSE:
                        POSE.audio_in(str(obj.get("n"))[:16], obj["a"])
                elif isinstance(obj, dict) and isinstance(obj.get("q"), str) and obj.get("n") != self.name:
                    e = poses.setdefault(str(obj.get("n"))[:16], {"q": None, "rx": 0, "at": 0})
                    e["look"] = obj.get("l") if obj.get("l") in (1, 2) else 1
                    if e["q"] != obj["q"]:
                        e["q"], e["rx"], e["at"] = obj["q"], e["rx"] + 1, time.time()
                        e["d"] = None
                        got_pose = True
            self.sock.settimeout(0.004)
            # 5) les poses reçues -> overlay, tout de suite
            if POSE and (got_pose or now >= next_follow):
                names = {o.get("n") for o in self.others}
                out = []
                for n, e in poses.items():
                    if n in names and time.time() - e["at"] < 3 and e["q"]:
                        if e.get("d") is None:
                            e["d"] = decode_pose(e["q"]) or False
                        if e["d"]:
                            out.append(dict(e["d"], name=n, rx=e["rx"], look=e.get("look", 1)))
                POSE.write_remotes(out)
            if POSE and now >= next_follow:
                next_follow = now + 0.1
                self.follow.update(self.others, POSE)
        return last_seq, next_state, next_follow

    @property
    def connected(self):
        return time.time() - self.last_rx < 3


class Demo(threading.Thread):
    """Test solo : un repère reste là où tu as commencé, un bot tourne autour de toi."""

    def __init__(self, game):
        super().__init__(daemon=True)
        self.game, self.others, self.connected = game, [], True
        self.start_pos = None
        self.running = True
        self.echo_t, self.echo_rx = None, 0

    def stop(self):
        self.running = False

    def run(self):
        t0 = time.time()
        while self.running:
            p = self.game.pos
            if p:
                self.start_pos = self.start_pos or p
                # le bot court en rond autour du point de départ, comme un vrai joueur (6 m/s),
                # et envoie sa position 30 fois/s, comme le réseau
                a = (time.time() - t0) * 0.3
                c = self.start_pos
                bot = [c[0] + 20 * math.cos(a), c[1], c[2] + 20 * math.sin(a)]
                others = [{"n": "Départ", "p": list(self.start_pos)}, {"n": "Bot", "p": bot}]
                # Écho : ta propre pose, rejouée 3 m à côté de toi (pour voir un personnage 3D tout seul)
                lp = POSE.local() if POSE else None
                if lp:
                    if lp["t"] != self.echo_t:
                        self.echo_t, self.echo_rx = lp["t"], self.echo_rx + 1
                    ep = (lp["pos"][0] + 3.0, lp["pos"][1], lp["pos"][2])
                    POSE.write_remotes([{"name": "Écho", "rx": self.echo_rx, "look": ECHO_LOOK, "t": lp["t"], "pos": ep,
                                         "yaw": lp["yaw"], "bones": lp["bones"]}])
                    snd = POSE.audio_out()          # mes sons rejoués par l'Écho (test des sons tout seul)
                    if snd:
                        POSE.audio_in("Écho", snd)
                    others.append({"n": "Écho", "p": [p[0] + 3.0, p[1], p[2]]})
                self.others = others
            time.sleep(0.033)
        if POSE:
            POSE.write_remotes([])


# ======================================================================================
#  Overlay dans le jeu : DLL injectée + mémoire partagée
# ======================================================================================
SHM_NAME = "Local\\MECCoopOverlay_v12"
SHM_HEAD = struct.Struct("<IIIiffffiffiii")        # 56 octets
SHM_PLAYER = struct.Struct("<32sfffiI")            # 52 octets
FRAMES_OFF = SHM_HEAD.size + 8 * SHM_PLAYER.size + 96          # compteur d'images de la DLL
# v2 : la DLL lit elle-même la caméra et ma position dans le jeu, à chaque image (zéro retard)
SHM_CAM = struct.Struct("<Qi6iiffii4iI")      # + v5 : drapeaux (radar, pseudos)
SHM_SIZE = FRAMES_OFF + 4 + SHM_CAM.size
DLL_NAME = "mec_overlay.dll"
DLL_COPY = "mec_overlay_v33.dll"


def imgui_color(hex_color):
    r, g, b = int(hex_color[1:3], 16), int(hex_color[3:5], 16), int(hex_color[5:7], 16)
    return 0xFF000000 | (b << 16) | (g << 8) | r


def inject_dll(pid, dll_path):
    """Charge la DLL dans le jeu (méthode classique LoadLibraryW + CreateRemoteThread)."""
    import ctypes.wintypes as wt
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.restype = wt.HANDLE
    k.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    k.VirtualAllocEx.restype = ctypes.c_void_p
    k.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
    k.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.c_size_t)]
    k.GetModuleHandleW.restype = wt.HMODULE
    k.GetProcAddress.restype = ctypes.c_void_p
    k.GetProcAddress.argtypes = [wt.HMODULE, ctypes.c_char_p]
    k.CreateRemoteThread.restype = wt.HANDLE
    k.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                     ctypes.c_void_p, wt.DWORD, ctypes.c_void_p]
    k.VirtualFreeEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
    k.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    k.GetExitCodeThread.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
    k.CloseHandle.argtypes = [wt.HANDLE]
    access = 0x0002 | 0x0400 | 0x0008 | 0x0020 | 0x0010   # create thread, query, vm op/write/read
    h = k.OpenProcess(access, False, pid)
    if not h:
        return "accès refusé au jeu (lance coop en administrateur)"
    try:
        data = ctypes.create_unicode_buffer(dll_path)
        size = ctypes.sizeof(data)
        mem = k.VirtualAllocEx(h, None, size, 0x3000, 0x04)
        if not mem:
            return "allocation impossible dans le jeu"
        k.WriteProcessMemory(h, mem, data, size, None)
        load = k.GetProcAddress(k.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
        th = k.CreateRemoteThread(h, None, 0, load, mem, 0, None)
        if not th:
            return "impossible de démarrer l'overlay dans le jeu"
        k.WaitForSingleObject(th, 10000)
        code = wt.DWORD()
        k.GetExitCodeThread(th, ctypes.byref(code))
        k.CloseHandle(th)
        k.VirtualFreeEx(h, mem, 0, 0x8000)
        return None if code.value else "le jeu a refusé de charger l'overlay"
    finally:
        k.CloseHandle(h)


class OverlayLink:
    """Mémoire partagée lue par la DLL à chaque image du jeu."""

    def __init__(self):
        import mmap
        self.mm = mmap.mmap(-1, SHM_SIZE, tagname=SHM_NAME)
        self.seq = 0
        self.wanted = False        # l'utilisateur veut l'overlay
        self.injected_pid = None
        self.error = ""
        self.last_frames, self.last_change = 0, 0.0

    @property
    def frames(self):
        return struct.unpack_from("<I", self.mm, FRAMES_OFF)[0]

    @property
    def active(self):
        """Vrai si la DLL dessine réellement (son compteur d'images avance)."""
        f, now = self.frames, time.time()
        if f != self.last_frames:
            self.last_frames, self.last_change = f, now
        return self.wanted and now - self.last_change < 1.5

    def ensure_injected(self, mem):
        if not self.wanted or not mem or not getattr(mem, "pid", None) or not mem.alive():
            return
        if self.injected_pid == mem.pid:
            return
        src = resource(DLL_NAME)
        if not src:
            self.error = f"{DLL_NAME} introuvable à côté de coop"
            return
        dst_dir = os.path.join(os.environ.get("LOCALAPPDATA", HERE), "MECCoop")
        os.makedirs(dst_dir, exist_ok=True)
        dst = os.path.join(dst_dir, DLL_COPY)
        try:
            import shutil
            shutil.copyfile(src, dst)
        except OSError:
            pass                    # déjà chargée (fichier verrouillé) : on garde la copie existante
        err = inject_dll(mem.pid, dst)
        self.injected_pid = mem.pid
        self.error = err or ""
        log(f"Injection overlay dans {mem.pid} : {err or 'OK'}")

    def keepalive(self):
        """Sans radar (avant Héberger/Rejoindre), personne n'écrivait l'en-tête : la DLL ne comptait pas ses images,
        coop la croyait absente et le premier Rejoindre ne chargeait pas le monde de l'hôte (23/10)."""
        if struct.unpack_from("<I", self.mm, 0)[0] == 0x4F43454D:
            return
        head = SHM_HEAD.pack(0x4F43454D, 5, 0, 1 if self.wanted else 0, 0.0, 0.0, 0.0, 0.0, 0,
                             150.0, 1.0, 0, 0, 0)
        self.seq = (self.seq + 2) & 0xFFFFFFFE
        struct.pack_into("<I", self.mm, 8, self.seq + 1)
        self.mm[0:8] = head[0:8]
        self.mm[12:SHM_HEAD.size] = head[12:]
        struct.pack_into("<I", self.mm, 8, self.seq)

    def write(self, radar):
        cfg, game, net = radar.cfg, radar.game, radar.net
        me = game.pos
        others = (net.others if net else [])[:8]
        status = game.status if game.status != "OK" else ""
        if net is not None and not net.connected:
            status = (status + " - " if status else "") + "pas connecté au serveur"
        head = SHM_HEAD.pack(
            0x4F43454D, 5, 0,
            1 if self.wanted else 0,
            *(me if me else (0.0, 0.0, 0.0)), float(radar.heading), 1 if me else 0,
            float(cfg.get("range", 150)), float(SIZES[cfg.get("size", 1) % len(SIZES)]),
            int(cfg.get("corner", 0)) % 4, 1 if cfg.get("mirror") else 0, len(others))
        body = bytearray()
        for i in range(8):
            if i < len(others):
                o = others[i]
                p = o.get("p")
                name = o["n"].encode("utf-8")[:31]
                body += SHM_PLAYER.pack(name, *(p if p else (0.0, 0.0, 0.0)), 1 if p else 0,
                                        imgui_color(COLORS[(i + 1) % len(COLORS)]))
            else:
                body += bytes(SHM_PLAYER.size)
        st = status.encode("utf-8")[:95]
        body += st + bytes(96 - len(st))
        # où la DLL doit lire la caméra et ma position (chemins depuis Faith)
        ca = game.cam_addr or 0
        cam_ok = 1 if (ca and radar.cam_sign is not None) else 0
        lo, hi = ca & 0xFFFFFFFF, ca >> 32
        lo, hi = (lo - (1 << 32) if lo >= 1 << 31 else lo), (hi - (1 << 32) if hi >= 1 << 31 else hi)
        cam = SHM_CAM.pack(game.faith or 0, -1, lo, hi, 0, 0, 0, 0, int(game.cam_stride or 0),
                           float(radar.cam_sign or 1), float(radar.cam_off or 0.0), cam_ok,
                           len(POS_PATH), *(list(POS_PATH) + [0] * 4)[:4],
                           (0 if cfg.get("hidden", False) else 1) | (2 if cfg.get("nametags", True) else 0))
        self.seq = (self.seq + 2) & 0xFFFFFFFE
        mm = self.mm
        struct.pack_into("<I", mm, 8, self.seq + 1)                 # écriture en cours (impair)
        mm[0:8] = head[0:8]
        mm[12:SHM_HEAD.size] = head[12:]
        mm[SHM_HEAD.size:FRAMES_OFF] = bytes(body)
        mm[FRAMES_OFF + 4:SHM_SIZE] = cam
        struct.pack_into("<I", mm, 8, self.seq)                     # terminé (pair)


# ======================================================================================
#  Radar (fenêtre transparente par-dessus le jeu)
# ======================================================================================
COLORS = ["#ff4d4d", "#4dd2ff", "#ffd24d", "#7dff7d"]
SIZES = [0.75, 1.0, 1.35]
CORNERS = ["haut-droite", "haut-gauche", "bas-gauche", "bas-droite"]
RANGES = [50, 100, 150, 250, 400, 700]
VK_ALT = 0x12


class Win:
    """Petits outils Windows : fenêtre du jeu, premier plan, touches."""

    def __init__(self):
        try:
            import ctypes.wintypes as wt
            self.u = ctypes.windll.user32
            self.wt = wt
            self.ok = True
        except Exception:
            self.ok = False
        self.prev_keys = {}

    def game_window(self, pid):
        """Rectangle (gauche, haut, droite, bas) de la plus grande fenêtre visible du jeu."""
        if not self.ok or not pid:
            return None
        wt, u = self.wt, self.u
        best = [None, 0]
        proto = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def cb(hwnd, _):
            owner = wt.DWORD()
            u.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and u.IsWindowVisible(hwnd):
                r = wt.RECT()
                u.GetWindowRect(hwnd, ctypes.byref(r))
                area = (r.right - r.left) * (r.bottom - r.top)
                if area > best[1]:
                    best[0], best[1] = (hwnd, (r.left, r.top, r.right, r.bottom)), area
            return True

        u.EnumWindows(proto(cb), 0)
        return best[0]

    def foreground_pid(self):
        if not self.ok:
            return None
        owner = self.wt.DWORD()
        self.u.GetWindowThreadProcessId(self.u.GetForegroundWindow(), ctypes.byref(owner))
        return owner.value

    def pressed_alt(self, digit):
        """Vrai une seule fois quand on appuie sur Alt + chiffre."""
        if not self.ok:
            return False
        vk = 0x30 + digit
        down = bool(self.u.GetAsyncKeyState(VK_ALT) & 0x8000) and bool(self.u.GetAsyncKeyState(vk) & 0x8000)
        was = self.prev_keys.get(vk, False)
        self.prev_keys[vk] = down
        return down and not was


class Radar:
    BASE_SIZE, BASE_R = 260, 115
    FPS_MS = 8

    def __init__(self, master, game, net, cfg, link=None):
        import tkinter as tk
        self.tk = tk
        self.link = link
        self.alive = True
        self.game, self.net, self.cfg = game, net, cfg
        self.win = Win()
        self.heading = 0.0
        self.cam_sign = self.cam_off = None
        self.trail = []
        self.smooth = {}            # nom -> position affichée (lissée)
        self.game_rect = None
        self.next_window_check = 0
        self.visible = True
        self.geom = None
        self.root = tk.Toplevel(master)
        self.root.title("Coop radar")
        self.root.overrideredirect(True)
        self.root.attributes("-topmost", True)
        self.key = "#010101"
        self.root.configure(bg=self.key)
        try:
            self.root.attributes("-transparentcolor", self.key)
        except tk.TclError:
            pass
        self.c = tk.Canvas(self.root, bg=self.key, highlightthickness=0)
        self.c.pack()
        self.root.after(200, self.click_through)
        self.root.after(self.FPS_MS, self.loop)

    def destroy(self):
        self.alive = False
        try:
            self.root.destroy()
        except Exception:
            pass

    # ---------------------------------------------------------------- fenêtre
    def click_through(self):
        try:
            u = ctypes.windll.user32
            hwnd = u.GetParent(self.root.winfo_id()) or self.root.winfo_id()
            ex = u.GetWindowLongW(hwnd, -20)
            u.SetWindowLongW(hwnd, -20, ex | 0x80000 | 0x20 | 0x80)   # layered, transparent, tool
        except Exception:
            pass

    def scale(self):
        return SIZES[self.cfg.get("size", 1) % len(SIZES)]

    def place(self):
        k = self.scale()
        w = int(self.BASE_SIZE * k)
        h = w + int(130 * k)
        if self.game_rect:
            left, top, right, bottom = self.game_rect
        else:
            left, top = 0, 0
            right, bottom = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        m = 20
        corner = CORNERS[self.cfg.get("corner", 0) % len(CORNERS)]
        x = right - w - m if "droite" in corner else left + m
        y = top + m + 30 if "haut" in corner else bottom - h - m
        geom = f"{w}x{h}+{int(x)}+{int(y)}"
        if geom != self.geom:
            self.geom = geom
            self.root.geometry(geom)
            self.c.config(width=w, height=h)

    def set_visible(self, v):
        if v != self.visible:
            self.visible = v
            self.root.attributes("-alpha", 1.0 if v else 0.0)

    def save_cfg(self):
        try:
            json.dump(self.cfg, open(CONFIG, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        except OSError:
            pass

    def handle_keys(self):
        changed = False
        cur = self.cfg.get("range", 150)
        rng_i = min(range(len(RANGES)), key=lambda i: abs(RANGES[i] - cur))
        if self.win.pressed_alt(1):
            self.cfg["range"] = RANGES[max(0, rng_i - 1)]
            changed = True
        if self.win.pressed_alt(2):
            self.cfg["range"] = RANGES[min(len(RANGES) - 1, rng_i + 1)]
            changed = True
        if self.win.pressed_alt(3):
            self.cfg["size"] = (self.cfg.get("size", 1) + 1) % len(SIZES)
            changed = True
        if self.win.pressed_alt(4):
            self.cfg["corner"] = (self.cfg.get("corner", 0) + 1) % len(CORNERS)
            changed = True
        if self.win.pressed_alt(5):
            self.cfg["hidden"] = not self.cfg.get("hidden", False)
            changed = True
        if changed:
            self.save_cfg()

    # ---------------------------------------------------------------- calculs
    def update_heading(self, p):
        """Le haut du radar = où Faith regarde.
        On lit la rotation de son squelette ; le décalage et le sens de rotation sont
        appris tout seuls pendant qu'on court (et sauvegardés)."""
        now = time.time()
        self.trail = [(t, x, z) for t, x, z in self.trail if now - t < 0.6] + [(now, p[0], p[2])]
        t0, x0, z0 = self.trail[0]
        dx, dz = p[0] - x0, p[2] - z0
        moving = dx * dx + dz * dz > 1.5 and now - t0 > 0.3
        raw = self.game.yaw_raw
        cal = self.cfg.setdefault("yaw_cal", {"1": [0.0, 0.0], "-1": [0.0, 0.0]})
        # on n'apprend que pendant une course rapide et bien droite (pas en pas chassé)
        straight = False
        if moving and len(self.trail) > 6:
            tm, xm, zm = self.trail[len(self.trail) // 2]
            speed = math.hypot(dx, dz) / max(now - t0, 1e-3)
            h1, h2 = math.atan2(xm - x0, zm - z0), math.atan2(p[0] - xm, p[2] - zm)
            bend = abs((h2 - h1 + math.pi) % (2 * math.pi) - math.pi)
            straight = speed > 4.0 and bend < math.radians(5) and now - t0 > 0.5
        if straight and raw is not None:
            mh = math.atan2(dx, dz)
            for s in (1, -1):                       # apprentissage du décalage, pour chaque sens
                d = mh - s * raw
                c = cal[str(s)]
                rate = self.alpha(4.0) if math.hypot(*c) > 0.9 else self.alpha(1.2)
                c[0] = c[0] * (1 - rate) + math.cos(d) * rate
                c[1] = c[1] * (1 - rate) + math.sin(d) * rate
            self.cal_frames = getattr(self, "cal_frames", 0) + 1
            if self.cal_frames % 300 == 0:
                self.save_cfg()
        best = max((1, -1), key=lambda s: math.hypot(*cal[str(s)]))
        strength = math.hypot(*cal[str(best)])
        self.cal_state = ("noskel" if raw is None else
                          "ok" if strength > 0.5 else f"learn:{min(99, int(strength / 0.5 * 100))}")
        self.cal_learning = straight
        skel_heading = None
        if raw is not None and strength > 0.5:
            skel_heading = best * raw + math.atan2(cal[str(best)][1], cal[str(best)][0])

        # ---- caméra : lecture directe, aucun retard. Il reste juste à savoir dans quel sens
        # elle est rangée en mémoire (sens + quart de tour), appris tout seul.
        self.cam_sign = self.cam_off = None
        if getattr(self.game, "cam_exact", False) and self.game.cam_raw is not None:
            self.heading = self.game.cam_raw          # caméra exacte : rien à apprendre
            self.cam_sign, self.cam_off = 1, math.pi  # l'overlay lit "forward" : cap = atan2(x, z) + pi
            self.cal_state = "cam"
            return
        cam, key = self.game.cam_raw, self.game.cam_key
        if cam is not None and key:
            cc = self.cfg.setdefault("cam_cal", {}).setdefault(key, {"1": [0.0, 0.0], "-1": [0.0, 0.0]})
            # sens de rotation : quand je tourne en courant, la caméra tourne dans le même sens
            speed_ok = moving and math.hypot(dx, dz) / max(now - t0, 1e-3) > 3.0
            vw = getattr(self, "cam_vwin", None)
            if not speed_ok:
                self.cam_vwin = None
            elif vw is None or vw[3] != key:
                self.cam_vwin = (now, cam, math.atan2(dx, dz), key)
            elif now - vw[0] >= 0.5:
                dc, dm = wrap_angle(cam - vw[1]), wrap_angle(math.atan2(dx, dz) - vw[2])
                if abs(dc) > math.radians(15) and abs(dm) > math.radians(10):
                    cc["votes"] = max(-20, min(20, cc.get("votes", 0) + (1 if dc * dm > 0 else -1)))
                self.cam_vwin = (now, cam, math.atan2(dx, dz), key)
            votes = cc.get("votes", 0)
            sign_known = 1 if votes >= 4 else -1 if votes <= -4 else None
            hist = getattr(self, "cam_hist", [])
            hist = [h for h in hist if now - h[0] < 0.8] + [(now, cam, raw)]
            self.cam_hist = hist
            samples = []
            if straight:
                samples.append((math.atan2(dx, dz), self.alpha(0.8)))
            if skel_heading is not None and len(hist) > 10 and now - hist[0][0] > 0.6:
                still = all(abs(wrap_angle(h[1] - cam)) < math.radians(1) for h in hist) and \
                    all(h[2] is not None and abs(wrap_angle(h[2] - raw)) < math.radians(2) for h in hist)
                if still:                       # immobile : le corps a rattrapé la caméra
                    samples.append((skel_heading, self.alpha(3.0)))
            for target_h, rate in samples:
                for s in (1, -1):
                    d = target_h - s * cam
                    c = cc[str(s)]
                    c[0] = c[0] * (1 - rate) + math.cos(d) * rate
                    c[1] = c[1] * (1 - rate) + math.sin(d) * rate
            st = {s: math.hypot(*cc[str(s)]) for s in (1, -1)}
            cb = sign_known or max(st, key=st.get)
            if sign_known and st[cb] > 0.5:
                q = math.pi / 2                 # la vraie valeur est forcément un quart de tour exact
                off = round(math.atan2(cc[str(cb)][1], cc[str(cb)][0]) / q) * q
                self.cam_sign, self.cam_off = cb, off
                self.heading = cb * cam + off
                self.cal_state = "cam"
                if not getattr(self, "cam_saved", False):
                    self.cam_saved = True
                    self.game.cam_trusted.add(key)
                    self.save_cfg()
                return
            prog = (min(abs(votes), 4) / 4 + min(st[cb], 0.5) / 0.5) / 2
            self.cal_state = f"camlearn:{min(99, int(prog * 100))}"
        target = None
        if raw is not None and strength > 0.5:      # calibré : on suit la caméra
            target = best * raw + math.atan2(cal[str(best)][1], cal[str(best)][0])
        elif moving:                                # pas encore calibré : direction de course
            target = math.atan2(dx, dz)
        if target is not None:
            diff = (target - self.heading + math.pi) % (2 * math.pi) - math.pi
            self.heading += diff * self.alpha(0.1)

    def alpha(self, tau):
        """Facteur de lissage indépendant du nombre d'images par seconde."""
        return 1.0 - math.exp(-getattr(self, "dt", 0.016) / tau)

    def smoothed(self, name, p):
        s = self.smooth.get(name)
        if s is None or sum(abs(a - b) for a, b in zip(s, p)) > 60:   # téléportation : pas de lissage
            s = list(p)
        else:
            k = self.alpha(0.09)
            s = [a + (b - a) * k for a, b in zip(s, p)]
        self.smooth[name] = s
        return s

    def to_screen(self, me, other, rng, size=None, R=None):
        size = size or self.BASE_SIZE
        R = R or self.BASE_R
        rx, rz = other[0] - me[0], other[2] - me[2]
        a = self.heading
        fwd = rx * math.sin(a) + rz * math.cos(a)
        right = rx * math.cos(a) - rz * math.sin(a)
        if self.cfg.get("mirror"):
            right = -right
        d = math.hypot(fwd, right)
        k = R / rng
        if d * k > R:                          # trop loin : on le colle au bord
            fwd, right = fwd * rng / d, right * rng / d
        cx = cy = size / 2
        return cx + right * k, cy - fwd * k, d

    # ---------------------------------------------------------------- boucle
    def loop(self):
        if not self.alive:
            return
        try:
            now = time.time()
            if now > self.next_window_check:
                self.next_window_check = now + 0.5
                gw = self.win.game_window(getattr(self.game.mem, "pid", None) if self.game.mem else None)
                self.game_rect = gw[1] if gw else None
                fg = self.win.foreground_pid()
                game_pid = getattr(self.game.mem, "pid", None) if self.game.mem else None
                # visible quand le jeu OU l'appli coop est au premier plan
                focused = (fg is None or game_pid is None or fg in (game_pid, os.getpid()))
                overlay = bool(self.link and self.link.active)
                self.set_visible(focused and not self.cfg.get("hidden", False) and not overlay)
            self.handle_keys()
            self.dt = min(0.1, now - getattr(self, "last_frame", now - 0.016)) or 0.016
            self.last_frame = now
            if self.game.pos:
                self.update_heading(self.game.pos)
            overlay_on = bool(self.link and self.link.active)
            if self.link:
                self.link.write(self)
            if self.cfg.get("hidden", False) or overlay_on:
                self.set_visible(False)
            self.place()
            if self.visible:
                self.draw()
        except Exception as e:
            log(f"Erreur radar : {e}")
        # l'overlay lit lui-même la caméra et ma position à chaque image : 60 mises à jour/s lui suffisent
        self.root.after(16 if (self.link and self.link.active) else self.FPS_MS, self.loop)

    def draw(self):
        c = self.c
        k = self.scale()
        S, R = self.BASE_SIZE * k, self.BASE_R * k
        c.delete("all")
        cx = cy = S / 2
        rng = self.cfg.get("range", 150)
        f_small, f_name, f_line = ("Segoe UI", int(8 * k)), ("Segoe UI", int(9 * k), "bold"), \
            ("Segoe UI", int(10 * k), "bold")
        c.create_oval(cx - R, cy - R, cx + R, cy + R, fill="#111418", outline="#e8e8e8", width=2)
        c.create_oval(cx - R / 2, cy - R / 2, cx + R / 2, cy + R / 2, outline="#3a3f46")
        c.create_text(cx, cy - R + 10 * k, text=f"{rng} m", fill="#8a9099", font=f_small)
        c.create_polygon(cx, cy - 9 * k, cx - 6 * k, cy + 6 * k, cx + 6 * k, cy + 6 * k, fill="#ffffff")
        me = self.game.pos
        lines = []
        if me:
            me = self.smoothed("__moi__", me)
            for i, o in enumerate(self.net.others[: MAX_PLAYERS - 1]):
                col = COLORS[(i + 1) % len(COLORS)]
                if not o.get("p"):
                    lines.append((col, f"{o['n']} : pas en jeu"))
                    continue
                p = self.smoothed(o["n"], o["p"])
                x, y, d = self.to_screen(me, p, rng, S, R)
                r = 6 * k
                c.create_oval(x - r, y - r, x + r, y + r, fill=col, outline="#000000")
                c.create_text(x, y - 13 * k, text=o["n"], fill=col, font=f_name)
                dh = p[1] - me[1]
                arrow = "↑" if dh > 2 else "↓" if dh < -2 else ""
                lines.append((col, f"{o['n']} : {d:.0f} m  {arrow}{abs(dh):.0f} m" if arrow
                              else f"{o['n']} : {d:.0f} m"))
        status = self.game.status if self.game.status != "OK" else ""
        if not self.net.connected:
            status = (status + " · " if status else "") + "pas connecté au serveur"
        y0 = S + 8 * k
        if status:
            c.create_text(S / 2, y0, text=status, fill="#ffb347", font=f_small)
            y0 += 18 * k
        for col, txt in lines:
            c.create_text(10, y0, text=txt, fill=col, anchor="w", font=f_line)
            y0 += 18 * k


# ======================================================================================
#  Démarrage
# ======================================================================================
_LOG = [os.path.join(HERE, "log_coop.txt")]


def log_dir(game_dir):
    if game_dir:
        d = os.path.join(game_dir, "_mod_coop")
        os.makedirs(d, exist_ok=True)
        _LOG[0] = os.path.join(d, "log_coop.txt")


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line)
    try:
        with open(_LOG[0], "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


_IPS = [0, []]


# ======================================================================================
#  Progression partagée : celui qui rejoint peut jouer avec la sauvegarde de l'hôte
#  (portes ouvertes, QG détruit, missions faites...). Sa propre sauvegarde est mise de côté
#  (copie datée, jamais effacée) et remise quand il le demande.
# ======================================================================================
SAVE_PORT = PORT + 1                    # 27778 (TCP)
SAVE_CHUNK = 1100                       # morceaux UDP (sous la taille max d'un paquet VPN)
SAVE_FILES = ("PROF_SAVE", "PROF_SAVE_backup")
BACKUP_DIR = os.path.join(HERE, "sauvegardes")


def save_dir():
    """Documents\\Mirrors Edge Catalyst\\settings (là où le jeu range la progression)."""
    docs = None
    try:
        buf = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf) == 0:   # CSIDL_PERSONAL (Documents)
            docs = buf.value
    except Exception:
        pass
    docs = docs or os.path.join(os.path.expanduser("~"), "Documents")
    return os.path.join(docs, "Mirrors Edge Catalyst", "settings")


def read_my_save():
    """Ma sauvegarde (octets) ; lue deux fois pour ne pas attraper une écriture du jeu en cours."""
    path = os.path.join(save_dir(), "PROF_SAVE")
    for _ in range(5):
        try:
            a = open(path, "rb").read()
            time.sleep(0.05)
            b = open(path, "rb").read()
        except OSError:
            return None
        if a == b and a[:8] == b"FBCHUNKS":
            return a
        time.sleep(0.3)
    return None


class SaveServer(threading.Thread):
    """Hôte : envoie sa sauvegarde à qui la demande (lecture seule, rien n'est jamais reçu)."""

    def __init__(self, name):
        super().__init__(daemon=True)
        self.name = name
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", SAVE_PORT))
        self.sock.listen(4)
        self.sock.settimeout(0.5)
        self.running = True

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass

    def run(self):
        import zlib
        while self.running:
            try:
                conn, addr = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            try:
                conn.settimeout(10)
                data = read_my_save()
                if not data:
                    conn.sendall(b"ERR\n")
                else:
                    z = zlib.compress(data, 9)
                    head = json.dumps({"name": self.name, "size": len(data), "z": len(z)}).encode() + b"\n"
                    conn.sendall(head + z)
                    log(f"Progression envoyée à {addr[0]} ({len(z) // 1024} Ko)")
            except OSError:
                pass
            finally:
                conn.close()


def fetch_host_save_udp(host):
    """Par le port du jeu (UDP 27777), par morceaux, en redemandant ceux qui se perdent."""
    import zlib
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.4)
    try:
        head = None
        for _ in range(10):                              # ~4 s pour que l'hôte réponde
            s.sendto(json.dumps({"sv": "get"}).encode("utf-8"), (host, PORT))
            try:
                while True:
                    data, _a = s.recvfrom(65536)
                    if data[:1] == b"{":
                        m = json.loads(data.decode("utf-8"))
                        if isinstance(m, dict) and "svh" in m:
                            head = m
                            break
            except (socket.timeout, ConnectionResetError, ValueError):
                pass
            if head:
                break
        if not head:
            raise OSError("l'hôte ne répond pas (a-t-il cliqué Héberger ?)")
        if not head.get("svh"):
            raise OSError("l'hôte n'a pas pu lire sa sauvegarde")
        crc, n = head["crc"], head["n"]
        parts = {}
        end = time.time() + 20
        while len(parts) < n:
            if time.time() > end:
                raise OSError(f"transfert incomplet ({len(parts)}/{n})")
            missing = [i for i in range(n) if i not in parts][:32]
            s.sendto(json.dumps({"sv": "part", "crc": crc, "i": missing}).encode("utf-8"), (host, PORT))
            want = set(missing)
            try:
                while want:                              # dès que le lot est arrivé, on demande le suivant
                    data, _a = s.recvfrom(65536)
                    if data[:3] == b"SVC" and len(data) >= 11:
                        c, i = struct.unpack_from("<II", data, 3)
                        if c == crc and 0 <= i < n:
                            parts[i] = data[11:]
                            want.discard(i)
            except (socket.timeout, ConnectionResetError):
                pass
        z = b"".join(parts[i] for i in range(n))
    finally:
        s.close()
    if len(z) != head["z"] or zlib.crc32(z) != crc:
        raise OSError("sauvegarde reçue abîmée")
    data = zlib.decompress(z)
    if len(data) != head["size"] or data[:8] != b"FBCHUNKS":
        raise OSError("sauvegarde reçue abîmée")
    return head.get("name", "l'hôte"), data


def fetch_host_save(host):
    """Celui qui rejoint : télécharge la sauvegarde de l'hôte. Renvoie (nom de l'hôte, octets) ou lève une erreur."""
    try:
        return fetch_host_save_udp(host)
    except OSError as e:
        log(f"Progression par le port du jeu : {e}")
        try:
            return fetch_host_save_tcp(host)
        except (OSError, ValueError):
            raise e


def fetch_host_save_tcp(host):
    import zlib
    with socket.create_connection((host, SAVE_PORT), timeout=4) as c:
        buf = b""
        while b"\n" not in buf:
            part = c.recv(4096)
            if not part:
                raise OSError("l'hôte n'a rien envoyé")
            buf += part
        line, rest = buf.split(b"\n", 1)
        if line == b"ERR":
            raise OSError("l'hôte n'a pas pu lire sa sauvegarde")
        head = json.loads(line)
        while len(rest) < head["z"]:
            part = c.recv(65536)
            if not part:
                raise OSError("transfert coupé")
            rest += part
    data = zlib.decompress(rest[:head["z"]])
    if len(data) != head["size"] or data[:8] != b"FBCHUNKS":
        raise OSError("sauvegarde reçue abîmée")
    return head.get("name", "l'hôte"), data


def marker_path():
    """Fichier posé à côté de la sauvegarde tant qu'elle est celle d'un hôte : la vérité sur « à qui est la
    sauvegarde installée », même si coop a planté, a été fermé, ou a été ouvert deux fois."""
    return os.path.join(save_dir(), "coop_monde_hote.json")


def read_marker():
    try:
        m = json.load(open(marker_path(), encoding="utf-8"))
        return m if isinstance(m, dict) and m.get("backup") else None
    except (OSError, ValueError):
        return None


PROFILE_FILES = ("PROF_SAVE_profile", "PROF_SAVE_backup_profile")


def write_file_safe(path, data):
    """Écrit d'un coup : fichier temporaire puis remplacement, pour ne jamais laisser une sauvegarde à moitié
    écrite (si le remplacement est refusé parce que le jeu tient le fichier, écriture directe)."""
    tmp = path + ".coop_tmp"
    try:
        with open(tmp, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, path)
        return
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
    with open(path, "wb") as out:
        out.write(data)


def backup_current(tag):
    """Copie de la sauvegarde installée avant de l'écraser (jamais deux copies identiques de suite)."""
    import hashlib
    d = save_dir()
    try:
        cur = open(os.path.join(d, "PROF_SAVE"), "rb").read()
    except OSError:
        return None
    digest = hashlib.md5(cur).hexdigest()
    last = os.path.join(BACKUP_DIR, "derniere_copie.txt")
    try:
        if open(last, encoding="utf-8").read().strip() == digest:
            return None
    except OSError:
        pass
    dest = os.path.join(BACKUP_DIR, f"{tag}_{time.strftime('%Y%m%d_%H%M%S')}")
    n = 2
    while os.path.exists(dest):
        dest = os.path.join(BACKUP_DIR, f"{tag}_{time.strftime('%Y%m%d_%H%M%S')}_{n}")
        n += 1
    try:
        os.makedirs(dest, exist_ok=True)
        for f in SAVE_FILES + PROFILE_FILES:
            src = os.path.join(d, f)
            if os.path.exists(src):
                with open(os.path.join(dest, f), "wb") as out:
                    out.write(open(src, "rb").read())
        with open(last, "w", encoding="utf-8") as out:
            out.write(digest)
    except OSError:
        return None
    return dest


def use_host_save(host_name, data, make_backup=True):
    """Met MA sauvegarde de côté (copie datée) puis installe celle de l'hôte.
    Si la sauvegarde installée est déjà celle d'un hôte (marqueur présent), on ne la prend JAMAIS pour la mienne :
    la copie de la mienne faite au départ reste la bonne."""
    import shutil
    d = save_dir()
    marker = read_marker()
    if marker and os.path.exists(os.path.join(marker["backup"], "PROF_SAVE")):
        mine = marker["backup"]
        backup_current("monde_remplace")           # le monde d'hôte en cours, au cas où
    else:
        stamp = time.strftime("%Y%m%d_%H%M%S")
        mine = os.path.join(BACKUP_DIR, f"ma_progression_{stamp}")
        os.makedirs(mine, exist_ok=True)
        for f in SAVE_FILES + PROFILE_FILES:
            src = os.path.join(d, f)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(mine, f))
        if not os.path.exists(os.path.join(mine, "PROF_SAVE")):
            raise OSError("ta sauvegarde est introuvable : rien n'a été changé")
    # le marqueur d'abord : si quelque chose casse ensuite, on sait toujours où est la mienne
    with open(marker_path(), "w", encoding="utf-8") as f:
        json.dump({"from": host_name, "backup": mine, "at": time.strftime("%d/%m %H:%M")}, f, ensure_ascii=False)
    for f in SAVE_FILES:
        write_file_safe(os.path.join(d, f), data)
    return mine


def clear_marker():
    try:
        os.remove(marker_path())
    except OSError:
        pass


# Drapeaux qui changent tout seuls (expérience, heures, ceux que le jeu remet à 0 à chaque chargement) : une
# différence sur eux ne justifie pas un 2e rechargement.
VOLATILE_FLAGS = {0x55a782c6, 0xed6f924f, 0x0daa7e29, 0x440bc24b}


def volatile_flag(h, want, got):
    big = 100_000_000                              # une date (secondes depuis 1970) ou un compteur de temps
    return h in VOLATILE_FLAGS or abs(want) > big or abs(got) > big


def own_flags_file(backup):
    """Ma progression telle qu'elle était EN MÉMOIRE dans le jeu quand j'ai rejoint (drapeaux de l'histoire)."""
    return os.path.join(backup, "drapeaux.json") if backup else None


def load_own_flags(backup):
    try:
        with open(own_flags_file(backup), encoding="utf-8") as f:
            flags = json.load(f).get("flags")
        return [x for x in flags if isinstance(x, list) and len(x) == 2] if isinstance(flags, list) else None
    except (OSError, ValueError, TypeError, AttributeError):
        return None


def restore_my_save(backup, keep_marker=False):
    """Remet ma progression (les 4 fichiers de la copie). keep_marker : le marqueur reste tant que le jeu n'a
    pas rechargé (sinon une sauvegarde auto du jeu pourrait réécraser la mienne sans que coop le sache)."""
    d = save_dir()
    marker = read_marker()
    if marker and os.path.exists(os.path.join(marker["backup"], "PROF_SAVE")):
        backup = marker["backup"]                # le marqueur fait foi
    if not backup or not os.path.exists(os.path.join(backup, "PROF_SAVE")):
        raise OSError("copie de ta progression introuvable")
    files = {}
    for f in SAVE_FILES + PROFILE_FILES:
        src = os.path.join(backup, f)
        if os.path.exists(src):
            files[f] = open(src, "rb").read()
    if files["PROF_SAVE"][:8] != b"FBCHUNKS":
        raise OSError("copie de ta progression abîmée")
    backup_current("monde_remplace")
    for f, data in files.items():
        write_file_safe(os.path.join(d, f), data)
    if not keep_marker:
        clear_marker()


def local_ips():
    if time.time() - _IPS[0] < 5:
        return _IPS[1]
    ips = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        ips |= {ip for ip in socket.gethostbyname_ex(socket.gethostname())[2] if not ip.startswith("127.")}
    except OSError:
        pass
    _IPS[0], _IPS[1] = time.time(), sorted(ips)
    return _IPS[1]


def resource(name):
    """Fichier livré avec l'appli (dans l'exe) ou posé à côté du script."""
    for base in (getattr(sys, "_MEIPASS", None), HERE, os.path.join(HERE, "overlay", "build")):
        if base and os.path.exists(os.path.join(base, name)):
            return os.path.join(base, name)
    return None


def load_cfg():
    try:
        return json.load(open(CONFIG, encoding="utf-8"))
    except (OSError, ValueError):
        return {}


# Couleurs façon Mirror's Edge : noir profond, blanc, rouge runner
BG, CARD, FIELD, LINE = "#0b0d10", "#14171c", "#0e1115", "#252a32"
TXT, DIM, FAINT = "#f2f3f5", "#8b929c", "#5a616b"
RED, RED_HI, RED_LO = "#e3261c", "#ff3b30", "#9e1a13"
GREEN, ORANGE, GREY = "#4cd964", "#ffb347", "#3a404b"
FONT = "Segoe UI"
APP_VERSION = "3.4.0"


def _round_rect(c, x1, y1, x2, y2, r, **kw):
    """Rectangle aux coins arrondis sur un Canvas (polygone lissé)."""
    r = max(0, min(r, (x2 - x1) / 2, (y2 - y1) / 2))
    pts = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
           x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    return c.create_polygon(pts, smooth=True, **kw)


def make_widgets(tk):
    """Petits widgets dessinés (boutons arrondis, interrupteurs, choix segmentés)."""

    class RoundButton(tk.Canvas):
        STYLES = {   # fond, fond survolé, texte, bord
            "primary": (RED, RED_HI, "#ffffff", None),
            "secondary": ("#232831", "#2d333d", TXT, None),
            "ghost": (CARD, "#1b1f26", TXT, LINE),
            "danger": (CARD, "#2a1414", "#ff6b61", RED_LO),
            "active": ("#ffffff", "#ffffff", RED, None),
        }

        def __init__(self, parent, text, command=None, kind="primary", height=40, width=120, size=10, radius=10):
            super().__init__(parent, height=height, width=width, bg=parent["bg"], highlightthickness=0, bd=0,
                             cursor="hand2")
            self.text, self.command, self.kind, self.size, self.radius = text, command, kind, size, radius
            self.hover = self.pressed = False
            self.enabled = True
            self.bind("<Configure>", lambda e: self.draw())
            self.bind("<Enter>", lambda e: self._set(hover=True))
            self.bind("<Leave>", lambda e: self._set(hover=False, pressed=False))
            self.bind("<ButtonPress-1>", lambda e: self._set(pressed=True))
            self.bind("<ButtonRelease-1>", self._release)

        def _set(self, **kw):
            for k, v in kw.items():
                setattr(self, k, v)
            self.draw()

        def _release(self, e):
            was = self.pressed
            self._set(pressed=False)
            if was and self.enabled and self.command and 0 <= e.x <= self.winfo_width() and 0 <= e.y <= self.winfo_height():
                self.command()

        def set(self, text=None, kind=None, enabled=None):
            changed = False
            for k, v in (("text", text), ("kind", kind), ("enabled", enabled)):
                if v is not None and getattr(self, k) != v:
                    setattr(self, k, v)
                    changed = True
            if changed:
                self.config(cursor="hand2" if self.enabled else "arrow")
                self.draw()

        def draw(self):
            self.delete("all")
            w, h = self.winfo_width(), self.winfo_height()
            if w < 4:
                return
            bg, hi, fg, border = self.STYLES.get(self.kind, self.STYLES["primary"])
            fill = hi if (self.hover and self.enabled) else bg
            if not self.enabled:
                fill, fg = "#1c2027", FAINT
            off = 1 if self.pressed else 0
            _round_rect(self, 1, 1 + off, w - 1, h - 1, self.radius, fill=fill, outline=border or fill)
            self.create_text(w / 2, h / 2 + off, text=self.text, fill=fg, font=(FONT, self.size, "bold"))

    class Toggle(tk.Frame):
        """Une ligne : titre (+ explication) à gauche, interrupteur à droite."""

        def __init__(self, parent, title, variable, command=None, hint=""):
            super().__init__(parent, bg=parent["bg"])
            self.var, self.command = variable, command
            txt = tk.Frame(self, bg=self["bg"])
            txt.pack(side="left", fill="x", expand=True)
            tk.Label(txt, text=title, bg=self["bg"], fg=TXT, font=(FONT, 10), anchor="w").pack(fill="x")
            if hint:
                tk.Label(txt, text=hint, bg=self["bg"], fg=DIM, font=(FONT, 8), anchor="w", justify="left",
                         wraplength=330).pack(fill="x")
            self.sw = tk.Canvas(self, width=40, height=22, bg=self["bg"], highlightthickness=0, cursor="hand2")
            self.sw.pack(side="right", padx=(10, 0))
            for w in (self, self.sw, txt) + tuple(txt.winfo_children()):
                w.bind("<Button-1>", self.flip)
            self.draw()

        def flip(self, _e=None):
            self.var.set(not self.var.get())
            self.draw()
            if self.command:
                self.command()

        def draw(self):
            c = self.sw
            c.delete("all")
            on = bool(self.var.get())
            _round_rect(c, 1, 1, 39, 21, 10, fill=RED if on else "#2a2f38", outline=RED if on else "#2a2f38")
            x = 28 if on else 12
            c.create_oval(x - 8, 3, x + 8, 19, fill="#ffffff", outline="")

    class Segmented(tk.Canvas):
        """Choix parmi quelques options (une seule active)."""

        def __init__(self, parent, options, value, command=None, height=32, size=9):
            super().__init__(parent, height=height, bg=parent["bg"], highlightthickness=0, cursor="hand2")
            self.options, self.value, self.command, self.size = list(options), value, command, size
            self.bind("<Configure>", lambda e: self.draw())
            self.bind("<Button-1>", self.click)

        def set(self, value):
            if value != self.value:
                self.value = value
                self.draw()

        def click(self, e):
            w = self.winfo_width()
            i = min(len(self.options) - 1, max(0, int(e.x / max(1, w) * len(self.options))))
            if self.options[i] != self.value:
                self.value = self.options[i]
                self.draw()
                if self.command:
                    self.command(self.value)

        def draw(self):
            self.delete("all")
            w, h = self.winfo_width(), self.winfo_height()
            if w < 10:
                return
            _round_rect(self, 1, 1, w - 1, h - 1, 9, fill=FIELD, outline=LINE)
            n = len(self.options)
            seg = (w - 6) / n
            for i, o in enumerate(self.options):
                x1 = 3 + i * seg
                if o == self.value:
                    _round_rect(self, x1, 3, x1 + seg, h - 3, 7, fill=RED, outline=RED)
                self.create_text(x1 + seg / 2, h / 2, text=str(o), fill="#ffffff" if o == self.value else DIM,
                                 font=(FONT, self.size, "bold" if o == self.value else "normal"))

    class Dot(tk.Canvas):
        def __init__(self, parent, size=10):
            super().__init__(parent, width=size, height=size, bg=parent["bg"], highlightthickness=0)
            self.s, self.color = size, None

        def set(self, color):
            if color != self.color:
                self.color = color
                self.delete("all")
                self.create_oval(1, 1, self.s - 1, self.s - 1, fill=color, outline="")

    return RoundButton, Toggle, Segmented, Dot

CORNER_ICONS = ["Haut D", "Haut G", "Bas G", "Bas D"]   # même ordre que CORNERS


class App:
    def __init__(self):
        import tkinter as tk
        try:                                        # minuteries Windows précises à 1 ms (au lieu de 15,6)
            ctypes.windll.winmm.timeBeginPeriod(1)
        except Exception:
            pass
        self.tk = tk
        self.cfg = load_cfg()
        self.game = Game()
        self.game.cam_trusted = {k for k, v in self.cfg.get("cam_cal", {}).items()
                                 if isinstance(v, dict) and abs(v.get("votes", 0)) >= 4}
        self.game.start()
        self.net = None
        self.server = None
        self.radar = None
        self.mode = None
        self.error = ""
        try:
            self.link = OverlayLink()
        except Exception as e:          # hors Windows ou mémoire partagée impossible
            self.link = None
            log(f"Overlay indisponible : {e}")
        if self.link:
            self.link.wanted = bool(self.cfg.get("overlay", True))
        global POSE
        try:
            POSE = PoseLink()
            POSE.set_notice("")              # un vieux message laissé par une session précédente
        except Exception as e:
            POSE = None
            log(f"Poses indisponibles : {e}")
        self.next_inject = 0

        self.root = tk.Tk()
        self.root.title(f"MEC Coop {APP_VERSION}")
        self.root.configure(bg=BG)
        self.root.geometry("480x800")
        self.root.minsize(460, 560)
        ico = resource("icon.ico")
        if ico:
            try:
                self.root.iconbitmap(ico)
            except tk.TclError:
                pass
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self.build()
        self.root.after(250, self.refresh)

    # ---------------------------------------------------------------- widgets
    def label(self, parent, text="", size=10, color=TXT, bold=False, **kw):
        return self.tk.Label(parent, text=text, bg=parent["bg"], fg=color,
                             font=(FONT, size, "bold" if bold else "normal"), **kw)

    def card(self, parent, title=None, pad=14):
        tk = self.tk
        f = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=LINE)
        f.pack(fill="x", padx=16, pady=(0, 10))
        inner = tk.Frame(f, bg=CARD)
        inner.pack(fill="x", padx=pad, pady=(10, 12))
        if title:
            self.label(inner, title.upper(), 8, DIM, True).pack(anchor="w", pady=(0, 6))
        return inner

    def entry(self, parent, var, mono=False):
        e = self.tk.Entry(parent, textvariable=var, bg=FIELD, fg=TXT, insertbackground=TXT, relief="flat",
                          font=("Consolas", 11) if mono else (FONT, 11), highlightthickness=1,
                          highlightbackground=LINE, highlightcolor=RED)
        return e

    def build(self):
        tk = self.tk
        RoundButton, Toggle, Segmented, Dot = make_widgets(tk)
        self.W = (RoundButton, Toggle, Segmented, Dot)

        # ---- en-tête
        head = tk.Frame(self.root, bg=BG)
        head.pack(fill="x", padx=18, pady=(16, 8))
        logo = resource("logo.png")
        if logo:
            try:
                self.logo_img = tk.PhotoImage(file=logo)
                tk.Label(head, image=self.logo_img, bg=BG).pack(side="left", padx=(0, 12))
            except tk.TclError:
                pass
        t = tk.Frame(head, bg=BG)
        t.pack(side="left")
        self.label(t, "MIRROR'S EDGE CATALYST", 8, DIM, True).pack(anchor="w")
        tr = tk.Frame(t, bg=BG)
        tr.pack(anchor="w")
        self.label(tr, "COOP", 24, RED, True).pack(side="left")
        self.label(tr, f"v{APP_VERSION}", 10, DIM, True).pack(side="left", padx=(8, 0), anchor="s", pady=(0, 6))
        pill = tk.Frame(head, bg=BG)
        pill.pack(side="right", anchor="n", pady=(6, 0))
        self.d_state = Dot(pill, 10)
        self.d_state.pack(side="left", padx=(0, 6))
        self.l_state = self.label(pill, "Prêt", 10, TXT, True)
        self.l_state.pack(side="left")

        # ---- onglets
        nav = tk.Frame(self.root, bg=BG)
        nav.pack(fill="x", padx=18, pady=(4, 10))
        self.tabs, self.tab_btns = {}, {}
        for key, title in (("play", "Jouer"), ("settings", "Réglages"), ("info", "Infos")):
            col = tk.Frame(nav, bg=BG, cursor="hand2")
            col.pack(side="left", padx=(0, 18))
            lb = self.label(col, title, 11, DIM, True, cursor="hand2")
            lb.pack()
            bar = tk.Frame(col, bg=BG, height=3)
            bar.pack(fill="x", pady=(3, 0))
            for w in (col, lb):
                w.bind("<Button-1>", lambda e, k=key: self.show_tab(k))
            self.tab_btns[key] = (lb, bar)
            self.tabs[key] = self.scroll_page()
        tk.Frame(self.root, bg=LINE, height=1).pack(fill="x", padx=18, pady=(0, 12))

        # ---- pied : voyants
        foot = tk.Frame(self.root, bg=BG)
        foot.pack(side="bottom", fill="x", padx=18, pady=(0, 12))
        self.chips = {}
        for key, title in (("game", "Jeu"), ("net", "Réseau"), ("ovl", "Overlay"), ("3d", "3D"), ("snd", "Sons")):
            c = tk.Frame(foot, bg=BG)
            c.pack(side="left", padx=(0, 14))
            d = Dot(c, 8)
            d.pack(side="left", padx=(0, 5))
            self.label(c, title, 8, DIM).pack(side="left")
            self.chips[key] = d
        self.label(foot, f"v{APP_VERSION}", 8, FAINT).pack(side="right")
        self.build_play(self.tabs["play"].inner)
        self.build_settings(self.tabs["settings"].inner)
        self.build_info(self.tabs["info"].inner)

        self.show_tab("play")

    def scroll_page(self):
        """Une page qui défile à la molette si elle dépasse de la fenêtre."""
        tk = self.tk
        outer = tk.Frame(self.root, bg=BG)
        cv = tk.Canvas(outer, bg=BG, highlightthickness=0, bd=0)
        cv.pack(fill="both", expand=True)
        inner = tk.Frame(cv, bg=BG)
        win = cv.create_window(0, 0, window=inner, anchor="nw")

        def fit(_e=None):
            cv.itemconfigure(win, width=cv.winfo_width())
            h = inner.winfo_reqheight()
            cv.configure(scrollregion=(0, 0, cv.winfo_width(), max(h, cv.winfo_height())))
        inner.bind("<Configure>", fit)
        cv.bind("<Configure>", fit)

        def wheel(e):
            if inner.winfo_reqheight() > cv.winfo_height():
                cv.yview_scroll(-1 if (e.delta > 0 or getattr(e, "num", 0) == 4) else 1, "units")
        outer.bind("<Enter>", lambda e: (cv.bind_all("<MouseWheel>", wheel), cv.bind_all("<Button-4>", wheel),
                                         cv.bind_all("<Button-5>", wheel)))
        outer.bind("<Leave>", lambda e: (cv.unbind_all("<MouseWheel>"), cv.unbind_all("<Button-4>"),
                                         cv.unbind_all("<Button-5>")))
        outer.inner = inner
        return outer

    def show_tab(self, key):
        for k, f in self.tabs.items():
            f.pack_forget()
            lb, bar = self.tab_btns[k]
            lb.config(fg=TXT if k == key else DIM)
            bar.config(bg=RED if k == key else BG)
        self.tabs[key].pack(fill="both", expand=True)
        self.tab = key

    # ------------------------------------------------------------ onglet Jouer
    def build_play(self, page):
        tk = self.tk
        RoundButton, Toggle, Segmented, Dot = self.W
        global MY_LOOK, ECHO_LOOK
        MY_LOOK = 1 if self.cfg.get("look", 1) not in (1, 2) else self.cfg.get("look", 1)
        ECHO_LOOK = 1 if self.cfg.get("echo_look", 1) not in (1, 2) else self.cfg.get("echo_look", 1)

        # Toi
        f = self.card(page, "Toi")
        self.v_name = tk.StringVar(value=self.cfg.get("name", ""))
        self.entry(f, self.v_name).pack(fill="x", ipady=6)
        row = tk.Frame(f, bg=CARD)
        row.pack(fill="x", pady=(10, 0))
        self.label(row, "Les autres te voient en", 9, DIM).pack(side="left")
        self.s_look = Segmented(row, LOOKS, LOOKS[MY_LOOK - 1], self.set_look, height=30)
        self.s_look.pack(side="right", fill="x", expand=True, padx=(12, 0))

        # Partie
        f = self.card(page, "Partie")
        self.idle_box = tk.Frame(f, bg=CARD)
        btns = tk.Frame(self.idle_box, bg=CARD)
        btns.pack(fill="x")
        self.b_host = RoundButton(btns, "Héberger", lambda: self.start("host"), "primary", height=46, size=11)
        self.b_join = RoundButton(btns, "Rejoindre", lambda: self.start("join"), "secondary", height=46, size=11)
        self.b_host.pack(side="left", fill="x", expand=True, padx=(0, 5))
        self.b_join.pack(side="left", fill="x", expand=True, padx=(5, 0))
        self.label(self.idle_box, "IP de l'hôte (pour rejoindre)", 8, DIM).pack(anchor="w", pady=(10, 3))
        iprow = tk.Frame(self.idle_box, bg=CARD)
        iprow.pack(fill="x")
        self.v_ip = tk.StringVar(value=self.cfg.get("ip", ""))
        self.entry(iprow, self.v_ip, mono=True).pack(side="left", fill="x", expand=True, ipady=5)
        RoundButton(iprow, "Coller", self.paste_ip, "ghost", height=32, width=70, size=9).pack(side="left", padx=(8, 0))
        solo = tk.Frame(self.idle_box, bg=CARD)
        solo.pack(fill="x", pady=(10, 0))
        self.label(solo, "Tout seul ? Un clone court à côté de toi.", 8, DIM).pack(side="left")
        self.b_demo = RoundButton(solo, "Test solo", lambda: self.start("demo"), "ghost", height=30, width=96, size=9)
        self.b_demo.pack(side="right")

        self.run_box = tk.Frame(f, bg=CARD)
        top = tk.Frame(self.run_box, bg=CARD)
        top.pack(fill="x")
        self.l_mode = self.label(top, "", 13, TXT, True, anchor="w")
        self.l_mode.pack(side="left")
        self.b_stop = RoundButton(top, "Arrêter", self.stop, "danger", height=34, width=96, size=10)
        self.b_stop.pack(side="right")
        self.l_sub = self.label(self.run_box, "", 9, DIM, anchor="w", justify="left", wraplength=400)
        self.l_sub.pack(fill="x", pady=(4, 0))
        self.ip_box = tk.Frame(self.run_box, bg=FIELD, highlightthickness=1, highlightbackground=LINE)
        self.l_ips = self.label(self.ip_box, "", 12, TXT, True)
        self.l_ips.config(font=("Consolas", 12, "bold"))
        self.l_ips.pack(side="left", padx=10, pady=8)
        self.b_copy = RoundButton(self.ip_box, "Copier", self.copy_ip, "secondary", height=30, width=80, size=9)
        self.b_copy.pack(side="right", padx=6, pady=4)
        self.l_err = self.label(f, "", 9, ORANGE, anchor="w", justify="left", wraplength=400)
        self.idle_box.pack(fill="x")

        # Joueurs
        f = self.card(page, "Joueurs")
        self.players_box = tk.Frame(f, bg=CARD)
        self.players_box.pack(fill="x")
        self.l_noplayers = self.label(self.players_box, "", 9, DIM, anchor="w", justify="left", wraplength=400)
        self.player_rows = {}
        self.l_tp = self.label(f, "", 9, DIM, anchor="w")
        self.tp_req = None

        # Bandeaux (mission, progression)
        self.banner = tk.Frame(page, bg="#2a1f0c", highlightthickness=1, highlightbackground="#5a4316")
        self.l_banner = tk.Label(self.banner, text="", bg="#2a1f0c", fg=ORANGE, font=(FONT, 9), justify="left",
                                 anchor="w", wraplength=410)
        self.l_banner.pack(fill="x", padx=12, pady=(10, 8))
        self.b_mine = RoundButton(self.banner, "Remettre ma progression", self.restore_save, "secondary", height=32,
                                  width=210, size=9)
        self.save_msg = ""
        self.next_save_check = 0
        m = read_marker()
        if m:
            self.cfg["save_swapped"] = m             # sauvegarde d'un hôte installée (même si coop avait été fermé)
        else:
            self.cfg.pop("save_swapped", None)
        self.menu_since = None
        self.reloading = None
        self.pending_swap = None
        self.notice_until_playing = False

    # ------------------------------------------------------------ onglet Réglages
    def build_settings(self, page):
        tk = self.tk
        RoundButton, Toggle, Segmented, Dot = self.W
        self.v_overlay = tk.BooleanVar(value=bool(self.cfg.get("overlay", True)))
        self.v_tags = tk.BooleanVar(value=bool(self.cfg.get("nametags", True)))
        self.v_show = tk.BooleanVar(value=not self.cfg.get("hidden", False))
        self.v_mirror = tk.BooleanVar(value=self.cfg.get("mirror", False))
        self.v_tod = tk.BooleanVar(value=bool(self.cfg.get("sync_tod", True)))
        self.v_snd = tk.BooleanVar(value=bool(self.cfg.get("sounds", True)))
        self.v_auto_save = tk.BooleanVar(value=bool(self.cfg.get("auto_save", True)))
        if POSE:
            POSE.set_sounds(self.v_snd.get())

        f = self.card(page, "Ensemble")
        self.toggles = []
        for title, var, cmd, hint in (
                ("Même monde que l'hôte", self.v_auto_save, self.set_auto_save,
                 "En rejoignant, tu joues avec sa progression (portes, QG, missions). La tienne revient après."),
                ("Même heure du jour", self.v_tod, self.set_tod, "Le ciel de l'hôte chez celui qui rejoint."),
                ("Entendre les autres", self.v_snd, self.set_sounds, "Pas, atterrissages, respiration, coups.")):
            t = Toggle(f, title, var, cmd, hint)
            t.pack(fill="x", pady=4)
            self.toggles.append(t)

        f = self.card(page, "Dans le jeu")
        for title, var, cmd, hint in (
                ("Overlay dans le jeu", self.v_overlay, self.set_overlay, "Radar, pseudos et personnages 3D."),
                ("Pseudos au-dessus des têtes", self.v_tags, self.set_tags, ""),
                ("Radar", self.v_show, self.set_show, "Alt+5 pour le masquer en jeu."),
                ("Radar inversé gauche / droite", self.v_mirror, self.set_mirror, "")):
            t = Toggle(f, title, var, cmd, hint)
            t.pack(fill="x", pady=4)
            self.toggles.append(t)
        for title, attr, opts, cur, cmd in (
                ("Portée du radar", "s_range", [f"{r}" for r in RANGES], str(self.cfg.get("range", 150)), self.set_range),
                ("Taille", "s_size", ["Petit", "Moyen", "Grand"], ["Petit", "Moyen", "Grand"][self.cfg.get("size", 1) % 3],
                 self.set_size),
                ("Coin", "s_corner", ["Haut G", "Haut D", "Bas G", "Bas D"], CORNER_ICONS[self.cfg.get("corner", 0) % 4], self.set_corner)):
            self.label(f, title, 9, DIM).pack(anchor="w", pady=(8, 3))
            s = Segmented(f, opts, cur, cmd, height=30)
            s.pack(fill="x")
            setattr(self, attr, s)

        f = self.card(page, "Test solo")
        row = tk.Frame(f, bg=CARD)
        row.pack(fill="x")
        self.label(row, "Ton clone apparaît en", 9, DIM).pack(side="left")
        self.s_echo = Segmented(row, LOOKS, LOOKS[ECHO_LOOK - 1], self.set_echo_look, height=30)
        self.s_echo.pack(side="right", fill="x", expand=True, padx=(12, 0))

    # ------------------------------------------------------------ onglet Infos
    def build_info(self, page):
        tk = self.tk
        RoundButton, Toggle, Segmented, Dot = self.W
        f = self.card(page, "État détaillé")
        self.info = {}
        for key in ("game", "net", "ovl", "cam", "3d", "mission"):
            row = tk.Frame(f, bg=CARD)
            row.pack(fill="x", pady=2)
            d = Dot(row, 8)
            d.pack(side="left", padx=(0, 8), anchor="n", pady=5)
            lb = self.label(row, "", 9, TXT, anchor="w", justify="left", wraplength=380)
            lb.pack(side="left", fill="x", expand=True)
            self.info[key] = (d, lb)

        f = self.card(page, "Raccourcis dans le jeu")
        for k, v in (("Alt+M", "aller à la mission d'un autre (quand il en lance une)"),
                     ("Alt+1 / Alt+2", "zoom du radar"), ("Alt+3", "taille du radar"),
                     ("Alt+4", "coin du radar"), ("Alt+5", "masquer / afficher le radar")):
            row = tk.Frame(f, bg=CARD)
            row.pack(fill="x", pady=1)
            self.label(row, k, 9, TXT, True, width=13, anchor="w").pack(side="left")
            self.label(row, v, 9, DIM, anchor="w").pack(side="left")

        f = self.card(page, "Comment jouer")
        self.label(f, "1. Tout le monde lance Radmin VPN dans le même réseau.\n"
                      "2. L'hôte lance le jeu (balade libre) puis clique Héberger et donne son IP.\n"
                      "3. Les autres collent l'IP et cliquent Rejoindre : coop s'occupe du reste.",
                   9, DIM, anchor="w", justify="left", wraplength=400).pack(fill="x")
        row = tk.Frame(f, bg=CARD)
        row.pack(fill="x", pady=(10, 0))
        RoundButton(row, "Journaux", self.open_logs, "ghost", height=30, width=110, size=9).pack(side="left")
        RoundButton(row, "Mes sauvegardes", self.open_backups, "ghost", height=30, width=140, size=9).pack(
            side="left", padx=(8, 0))
        row2 = tk.Frame(f, bg=CARD)
        row2.pack(fill="x", pady=(8, 0))
        RoundButton(row2, "Jeu : recharger la sauvegarde", lambda: self.ask_menu(1), "ghost", height=30, width=220,
                    size=9).pack(side="left")
        self.label(page, "Personnages 3D et sons : méthode du mod MECMP (licence MIT).", 8, FAINT).pack(
            pady=(0, 6))

    def ask_menu(self, kind=0):
        """Sans quitter le jeu : 0 = menu principal, 1 = recharger la sauvegarde."""
        if POSE and POSE.game_state() == 2 and kind == 1:
            POSE.request_menu(kind)
            log("Rechargement de la sauvegarde demandé (bouton)" if kind else "Retour au menu principal demandé (bouton)")

    def open_logs(self):
        try:
            os.startfile(os.path.dirname(_LOG[0]))
        except OSError:
            pass

    def open_backups(self):
        try:
            os.makedirs(BACKUP_DIR, exist_ok=True)
            os.startfile(BACKUP_DIR)
        except OSError:
            pass

    def paste_ip(self):
        try:
            self.v_ip.set(self.root.clipboard_get().strip()[:64])
        except self.tk.TclError:
            pass

    # ---------------------------------------------------------------- réglages
    def save(self):
        try:
            json.dump(self.cfg, open(CONFIG, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        except OSError:
            pass

    def set_range(self, v):
        self.cfg["range"] = int(v.split()[0])
        self.save()

    def set_size(self, v):
        self.cfg["size"] = ["Petit", "Moyen", "Grand"].index(v)
        self.save()

    def set_corner(self, v):
        self.cfg["corner"] = CORNER_ICONS.index(v)
        self.save()

    def set_show(self):
        self.cfg["hidden"] = not self.v_show.get()
        self.save()

    def set_overlay(self):
        self.cfg["overlay"] = self.v_overlay.get()
        if self.link:
            self.link.wanted = self.cfg["overlay"]
        self.save()

    def set_look(self, v):
        global MY_LOOK
        MY_LOOK = LOOKS.index(v) + 1
        self.cfg["look"] = MY_LOOK
        self.save()

    # ---------------------------------------------------------------- progression partagée
    def game_running(self):
        for p in psutil.process_iter(["name"]):
            n = (p.info["name"] or "").lower()
            if "mirrorsedgecatalyst" in n and "trial" not in n:
                return True
        return False

    def set_auto_save(self):
        self.cfg["auto_save"] = self.v_auto_save.get()
        self.save()

    def launch_game(self):
        try:
            os.startfile("steam://rungameid/1233570")      # Mirror's Edge Catalyst sur Steam
            return True
        except OSError:
            return False

    def join_with_host_save(self):
        """Rejoindre = même monde que l'hôte : sa progression est installée (la mienne mise de côté),
        le jeu est (re)lancé avec. Tout est automatique ; une seule question si le jeu est ouvert."""
        from tkinter import messagebox
        host = self.cfg["ip"]
        self.save_msg = "Récupération de la progression de l'hôte..."
        self.refresh_save()
        self.root.update()
        log(f"Demande de la progression de l'hôte à {host}")
        # dans un fil à part : la fenêtre reste réactive, et au bout de 40 s on abandonne quoi qu'il arrive
        res = {}

        def fetch():
            try:
                res["ok"] = fetch_host_save(host)
            except Exception as e:                  # noqa: BLE001 (toute erreur = on rejoint sans sa progression)
                res["err"] = e
        th = threading.Thread(target=fetch, daemon=True)
        th.start()
        end = time.time() + 40
        while th.is_alive() and time.time() < end:
            self.root.update()
            time.sleep(0.05)
        if "ok" not in res:
            e = res.get("err") or "pas de réponse en 40 s"
            self.save_msg = f"Progression de l'hôte indisponible ({e}) : tu rejoins avec la tienne."
            log(self.save_msg)
            return
        name, data = res["ok"]
        log(f"Progression de {name} reçue ({len(data) // 1024} Ko)")
        try:
            installed = open(os.path.join(save_dir(), "PROF_SAVE"), "rb").read()
        except OSError:
            installed = b""
        if installed == data:
            # même monde : rien à charger, mais ma progression est quand même mise de côté (copie + marqueur),
            # pour que la progression de l'hôte puisse s'appliquer ensuite sans risque
            if not read_marker():
                self.install_host_save(name, data, launch=False)
            self.save_msg = f"Tu as déjà la progression de {name}."
            return
        if self.game_running() and POSE and self.link and self.link.wanted and not self.link.active:
            # coop vient d'être ouvert : l'overlay n'est pas encore dans le jeu. On l'injecte maintenant et on attend
            # qu'il réponde (sinon on croirait le jeu hors partie et on proposerait de le fermer)
            self.save_msg = "Connexion au jeu..."
            self.refresh_save()
            try:
                self.link.ensure_injected(self.game.mem)
            except Exception as e:                  # noqa: BLE001
                log(f"Injection avant de rejoindre : {e}")
            end = time.time() + 20
            while time.time() < end and not (self.link.active and POSE.game_state() == 2):
                self.link.keepalive()
                self.root.update()
                time.sleep(0.1)
        if (self.game_running() and POSE and self.link and self.link.active and POSE.game_state() == 2
                and POSE.my_mission()):
            # rejoindre en pleine mission : le point de réapparition de la mission restait, invalide dans le monde de
            # l'hôte, et le joueur mourait en boucle (23/10). On attend qu'il en sorte.
            self.pending_join = time.time() + 1800
            self.pending_ok = 0.0
            ms = POSE.my_mission()
            self.save_msg = (f"Tu es en mission ({mission_title(ms['name'])}) : finis-la ou quitte-la, le monde de "
                             "l'hôte se chargera tout seul après.")
            log("Rejoindre : en mission, j'attends qu'elle soit finie")
            return
        if self.game_running() and POSE and self.link and self.link.active and POSE.game_state() == 2:
            # ma sauvegarde est mise de côté (copie + marqueur) et celle de l'hôte installée, SANS recharger : un
            # rechargement ne change pas l'histoire (le jeu la garde en mémoire). C'est sa progression (drapeaux),
            # appliquée juste après, qui compte ; un seul rechargement suit (progress_flow).
            if self.install_host_save(name, data, launch=False):
                self.save_msg = f"Réception du monde de {name}..."
                self.notice(f"Réception du monde de {name}...", until_playing=False)
            return
        if self.game_running():
            # jamais de fermeture du jeu : on rejoint en partie, ou on attend que le joueur y soit
            if POSE and self.link and self.link.active and POSE.game_state() == 0:
                self.save_msg = ("L'overlay dans ton jeu est une ancienne version : redémarre le jeu une fois (mise à "
                                 "jour), puis rejoins. En attendant, tu rejoins avec ta propre progression.")
                log("Overlay du jeu d'une ancienne version (pas de réponse) : redémarrage du jeu nécessaire une fois")
            else:
                # souvent : le joueur est dans la fenêtre de coop (le jeu en arrière-plan ne dessine plus). On attend
                # qu'il retourne en ville, puis on relance Rejoindre tout seul (join_wait_tick)
                self.pending_join = time.time() + 600
                self.pending_ok = 0.0
                self.save_msg = ("Retourne dans le jeu, en ville : le monde de l'hôte se chargera tout seul dès que "
                                 "tu y es.")
                log("Rejoindre : jeu pas encore en partie, j'attends que le joueur y soit")
            return
        try:
            swapped = self.cfg.get("save_swapped")
            backup = use_host_save(name, data, make_backup=not swapped)
            if not swapped:
                self.cfg["save_swapped"] = {"from": name, "backup": backup, "at": time.strftime("%d/%m %H:%M")}
            else:
                swapped["from"] = name
            self.save()
            log(f"Progression de {name} installée (la mienne : {self.cfg['save_swapped']['backup']})")
        except OSError as e:
            self.save_msg = f"Impossible d'installer la progression de l'hôte : {e}"
            return
        self.guard_save(data, name)
        launched = self.launch_game()
        self.save_msg = f"Progression de {name} installée ✓ " + ("Le jeu se lance..." if launched else "Lance le jeu.")

    def guard_save(self, data, name):
        """L'appli EA peut remettre la sauvegarde du cloud juste avant le lancement : tant que le jeu
        n'a pas démarré, on remet celle de l'hôte si elle a été remplacée."""
        def run():
            d = save_dir()
            end = time.time() + 300
            fixed = 0
            while time.time() < end:
                running = self.game_running()
                try:
                    cur = open(os.path.join(d, "PROF_SAVE"), "rb").read()
                except OSError:
                    cur = None
                if cur != data and not running:
                    time.sleep(1.0)                  # laisse l'appli EA finir d'écrire
                    try:
                        for f in SAVE_FILES:
                            write_file_safe(os.path.join(d, f), data)
                        fixed += 1
                        log("Sauvegarde remplacée avant le lancement (cloud EA ?) : progression de l'hôte remise")
                    except OSError:
                        pass
                if running:
                    time.sleep(4.0)                  # le jeu lit sa sauvegarde au démarrage
                    if cur is not None and cur != data:
                        self.save_msg = ("Le jeu a démarré avec une autre sauvegarde (cloud EA ?). Désactive la "
                                         "sauvegarde cloud dans l'appli EA puis réessaie.")
                        log(self.save_msg)
                    else:
                        log(f"Jeu lancé avec la progression de {name}")
                    return
                time.sleep(0.5)
        threading.Thread(target=run, daemon=True).start()

    def notice(self, text, until_playing=False):
        """Message en haut de l'écran du jeu. until_playing : s'efface tout seul quand la partie reprend."""
        if POSE:
            POSE.set_notice(text)
        self.notice_until_playing = until_playing and bool(text)

    def menu_ready(self):
        """Vrai quand le jeu est au menu principal depuis 2 s et qu'il a fini d'écrire sa sauvegarde."""
        if not POSE or POSE.game_state() != 1:
            self.menu_since = None
            return False
        now = time.time()
        try:
            mt = os.path.getmtime(os.path.join(save_dir(), "PROF_SAVE"))
        except OSError:
            mt = 0
        if self.menu_since is None or mt != getattr(self, "menu_mtime", None):
            self.menu_since, self.menu_mtime = now, mt
            return False
        return now - self.menu_since >= 2.0

    def reload_game(self, text, kind=""):
        """Demande au jeu de recharger sa sauvegarde (sans quitter) ; le message s'efface une fois revenu en partie.
        kind : « restore » (ma progression remise) ou « join » (celle de l'hôte installée) : ce qu'il faut
        terminer ou défaire selon que le jeu a rechargé ou non."""
        POSE.request_menu(1)
        self.notice(text)
        self.reloading = {"at": time.time(), "left": False, "kind": kind}
        log(f"Rechargement de la sauvegarde dans le jeu : {text}")

    def save_flow(self):
        """Suit le rechargement de la sauvegarde dans le jeu, et efface les messages devenus inutiles."""
        if getattr(self, "notice_until_playing", False) and POSE and POSE.game_state() == 2:
            self.notice("")
        r = getattr(self, "reloading", None)
        if r and POSE:
            st = POSE.game_state()
            if st != 2:
                r["left"] = True                    # la partie s'est arrêtée : le rechargement a commencé
            elif r["left"]:
                self.reloading = None
                self.notice("")
                self.save_msg = ""
                if r.get("kind") == "restore":       # le jeu a bien rechargé ma progression : c'est fini
                    clear_marker()
                    self.cfg.pop("save_swapped", None)
                    self.save()
                    self.save_msg = "Ta progression est revenue ✓"
                    log("Ma progression a été remise et rechargée dans le jeu")
            elif time.time() - r["at"] > 8 and not r["left"] and r.get("tries", 1) < 5:
                # pas encore de chargement (le jeu refuse tant que la partie n'est pas chargée) : on redemande
                r["tries"] = r.get("tries", 1) + 1
                r["at"] = time.time()
                POSE.request_menu(1)
            elif time.time() - r["at"] > 8 and not r["left"]:
                # rien ne s'est passé (pas de chargement) : on prévient, la sauvegarde attendra le prochain lancement
                self.reloading = None
                self.notice("")
                self.save_msg = "Le jeu n'a pas rechargé : ferme-le et relance-le pour changer de monde."
                log("Le rechargement n'a pas démarré")
                self.reload_failed_at = time.time()
                if r.get("kind") == "join":
                    # le jeu garde mon monde en mémoire et va le réécrire : on remet tout de suite ma copie
                    # (sinon la copie « mienne » deviendrait plus vieille que ce que je joue)
                    try:
                        restore_my_save(self.cfg.get("save_swapped", {}).get("backup"))
                        self.cfg.pop("save_swapped", None)
                        self.save()
                        self.save_msg = "Le jeu n'a pas rechargé : tu restes dans ton monde. Réessaie dans quelques secondes."
                    except OSError as e:
                        log(f"Remise de ma progression impossible : {e}")

    def join_wait_tick(self):
        """Rejoindre cliqué hors partie : dès que le joueur est en ville depuis 3 s, on relance Rejoindre (même
        chemin que quand il reclique), qui récupère et charge le monde de l'hôte."""
        until = getattr(self, "pending_join", None)
        if not until:
            return
        if self.mode != "join" or time.time() > until or getattr(self, "leave", None):
            self.pending_join = None
            return
        if self.link:
            self.link.keepalive()
        ok = bool(POSE and self.link and self.link.active and POSE.game_state() == 2
                  and not getattr(self, "reloading", None) and not POSE.my_mission())
        if not ok:
            self.pending_ok = 0.0
            return
        if not self.pending_ok:
            self.pending_ok = time.time()
            return
        if time.time() - self.pending_ok >= 3:
            self.pending_join = None
            log("Rejoindre : le joueur est en partie, chargement du monde de l'hôte")
            self.start("join")

    def progress_flow(self):
        if POSE and hasattr(POSE, "prog_feed"):
            POSE.prog_feed()                   # (aussi sans réseau : ma progression remise en partant)
        self._progress_flow()

    def _progress_flow(self):
        """Celui qui rejoint : une fois toute la progression de l'hôte appliquée, la ville doit être reconstruite
        avec (les points de départ de mission ne sont créés qu'au chargement). Le jeu sauvegarde tout seul 1,5 s
        après un changement de drapeau hors mission ; on attend cette sauvegarde, puis on recharge (une fois)."""
        net = self.net
        if self.mode != "join" or not POSE or not isinstance(net, Client) or not net.pg_received:
            return
        st = getattr(self, "pg_reload", None)
        if st is None:
            try:
                mt = os.path.getmtime(os.path.join(save_dir(), "PROF_SAVE"))
            except OSError:
                mt = 0
            base = net.pg_applied_base if net.pg_applied_base is not None else POSE.prog_stats()[0]
            st = self.pg_reload = {"mtime": mt, "applied": base, "done": False, "since": 0.0}
        now = time.time()
        if st["done"]:
            self.progress_verify(st, now)
            return
        if (not POSE.prog_in_all_done() or now - net.pg_last_rx < 3 or getattr(self, "reloading", None)
                or POSE.game_state() != 2):
            st["calm"] = 0.0
            return
        st["calm"] = st.get("calm") or now
        if now - st["calm"] < 2:               # l'overlay a tout traité, en pleine partie, depuis 2 s
            return
        applied = POSE.prog_stats()[0] - st["applied"]
        if applied <= 0:                       # rien n'a changé : même monde, pas besoin de recharger
            st["done"] = True
            log("Progression de l'hôte : déjà la même, pas de rechargement")
            self.notice("")
            return
        st["since"] = st["since"] or now
        try:
            mt = os.path.getmtime(os.path.join(save_dir(), "PROF_SAVE"))
        except OSError:
            mt = 0
        if mt > max(st["mtime"], net.pg_last_rx) and POSE.game_state() == 2:   # sauvegarde faite APRÈS les derniers
            st["done"] = True
            st["reloaded_at"] = now
            log(f"Progression de l'hôte appliquée ({applied} drapeaux) et sauvegardée : rechargement de la ville")
            host = net.pg_host or "l'hôte"
            self.reload_game(f"Monde de {host} : chargement...")
        elif now - st["since"] > 20:
            st["done"] = True
            log("Progression de l'hôte appliquée, mais le jeu n'a pas sauvegardé (en mission ?) : pas de rechargement")
            self.notice("")

    def keep_own_flags(self, flags):
        """Ma table de drapeaux (relue dans le jeu avant la progression de l'hôte) rangée avec ma sauvegarde."""
        m = read_marker()
        if not m or load_own_flags(m["backup"]) is not None:
            return
        try:
            with open(own_flags_file(m["backup"]), "w", encoding="utf-8") as f:
                json.dump({"at": time.strftime("%d/%m %H:%M"), "flags": [[h, v] for h, v in flags.items()]}, f)
            log(f"Ma progression en mémoire gardée ({len(flags)} drapeaux) avec ma sauvegarde")
        except OSError as e:
            log(f"Impossible de garder ma progression en mémoire : {e}")

    def push_own_flags(self, backup):
        """Remet MA progression dans la mémoire du jeu (un rechargement de fichier ne le fait pas)."""
        flags = load_own_flags(backup)
        if not flags or not POSE:
            return 0
        for i in range(0, len(flags), 256):
            POSE.prog_in(flags[i:i + 256])
        return len(flags)

    def progress_verify(self, st, now):
        """Après le rechargement : je relis toute ma table et je la compare à celle de l'hôte (dans le journal)."""
        net = self.net
        if st.get("phase") == "apply2":           # 2e passe : on attend la sauvegarde du jeu, puis on recharge
            try:
                mt = os.path.getmtime(os.path.join(save_dir(), "PROF_SAVE"))
            except OSError:
                mt = 0
            if getattr(POSE, "_pg_pending", None) or (mt <= st["mtime2"] and now - st["t2"] < 12):
                return
            st.update(phase="", verified=False, reloaded_at=now, ingame=0.0)
            host = net.pg_host or "l'hôte"
            self.reload_game(f"Monde de {host} : chargement...")
            return
        if st.get("verified") or not st.get("reloaded_at") or getattr(self, "reloading", None):
            return
        if POSE.game_state() != 2:
            st["ingame"] = 0.0
            return
        st["ingame"] = st.get("ingame") or now
        if net.pg_verify is None:
            if now - st["ingame"] > 8:             # la ville est chargée : on relit
                net.pg_verify, net.pg_verify_rx = {}, now
                st["vreq"] = POSE.request_snapshot()
            return
        done, count = POSE.snapshot_done()
        if not (done == st.get("vreq") and len(net.pg_verify) >= count) and now - net.pg_verify_rx < 15:
            return
        st["verified"] = True
        mine, host = net.pg_verify, net.pg_host_flags
        net.pg_verify = None
        same = sum(1 for h, v in host.items() if mine.get(h) == v)
        diff = [f"{h:08x}: hôte {v}, moi {mine.get(h)}" for h, v in host.items() if h in mine and mine[h] != v]
        log(f"Vérification de la progression après rechargement : {same}/{len(host)} drapeaux identiques à l'hôte"
            f" ({len(diff)} différents, {len(host) - same - len(diff)} absents)" + (" ; ex. " + ", ".join(diff[:8]) if diff else ""))
        # comme au retour : au chargement, la ville remet certains drapeaux de mission à sa façon. Une 2e passe
        # (drapeaux remis, sauvegarde du jeu, 2e rechargement) donne une carte construite avec l'histoire de l'hôte.
        todo = [[h, v] for h, v in host.items() if h in mine and mine[h] != v]
        if todo and False:      # (2e passe en rejoignant : retirée, 3 chargements de suite faisaient tout bugger)
            POSE.prog_in(todo)
            try:
                mt = os.path.getmtime(os.path.join(save_dir(), "PROF_SAVE"))
            except OSError:
                mt = 0
            st.update(round2=True, phase="apply2", t2=now, mtime2=mt)
            log(f"Progression de l'hôte : {len(todo)} drapeaux remis une 2e fois, puis rechargement")

    def install_host_save(self, name, data, launch):
        try:
            swapped = self.cfg.get("save_swapped")
            backup = use_host_save(name, data, make_backup=not swapped)
            if not swapped:
                self.cfg["save_swapped"] = {"from": name, "backup": backup, "at": time.strftime("%d/%m %H:%M")}
            else:
                swapped["from"] = name
            self.save()
            log(f"Progression de {name} installée (la mienne : {self.cfg['save_swapped']['backup']})")
        except OSError as e:
            self.save_msg = f"Impossible d'installer la progression de l'hôte : {e}"
            return False
        if launch:
            self.guard_save(data, name)
            launched = self.launch_game()
            self.save_msg = f"Progression de {name} installée ✓ " + ("Le jeu se lance..." if launched else "Lance le jeu.")
        return True

    def auto_restore(self):
        """Ma progression revient toute seule quand je ne rejoins plus personne, dès que le jeu est fermé (seul un
        redémarrage du jeu relit vraiment la sauvegarde : un rechargement en jeu garde l'histoire en mémoire)."""
        sw = self.cfg.get("save_swapped")
        if not sw or self.mode == "join" or time.time() < self.next_save_check:
            if sw and self.mode != "join":
                self.next_save_check = min(self.next_save_check, time.time() + 0.5)
            return
        self.next_save_check = time.time() + 0.5
        # tant qu'un ami joue avec moi, on garde le monde partagé (on ne recharge pas sous ses pieds)
        if self.game_running() and self.net and getattr(self.net, "others", None) and getattr(self.net, "connected", False):
            return
        if self.game_running():
            # le jeu garde l'histoire en mémoire : avec ma table (gardée en rejoignant), on la remet dans le jeu puis
            # on recharge (leave_tick) ; sans elle, il faut fermer le jeu (il relit le fichier au démarrage)
            if load_own_flags(sw.get("backup")) is not None:
                if (not getattr(self, "leave", None) and POSE and self.link and self.link.active
                        and POSE.game_state() == 2 and not getattr(self, "reloading", None)
                        and time.time() - getattr(self, "reload_failed_at", 0) > 30):
                    self.start_leave(sw)
                return
            if not getattr(self, "close_asked", False):
                self.close_asked = True
                self.notice("Ferme le jeu pour retrouver ta progression")
                self.save_msg = "Ferme le jeu : ta progression reviendra dès qu'il sera fermé (coop s'en occupe)."
                log("Je ne rejoins plus personne : ma progression reviendra à la fermeture du jeu")
            return
        time.sleep(0.5)                              # le jeu vient de se fermer : il a fini d'écrire
        self.close_asked = False
        try:
            restore_my_save(sw.get("backup"))
            self.cfg.pop("save_swapped", None)
            self.save()
            self.save_msg = "Ta progression est revenue ✓"
            self.notice("")
            log("Ma progression a été remise automatiquement")
        except OSError as e:
            self.save_msg = f"Impossible de remettre ta progression : {e}"

    # ---- retour à ma progression SANS quitter le jeu : ma table remise en mémoire, puis fichier + rechargement
    def start_leave(self, sw):
        n = self.push_own_flags(sw.get("backup"))
        own = {h: v for h, v in load_own_flags(sw.get("backup")) or []}
        self.leave = {"step": "apply", "t": time.time(), "own": own, "backup": sw.get("backup")}
        self.notice("Retour à ta progression...")
        self.save_msg = "Retour à ta progression dans le jeu..."
        log(f"Retour à ma progression : {n} drapeaux remis dans le jeu")

    def start_collect(self):
        now = time.time()
        self._collect, self._collect_rx, self._collect_at = {}, now, now
        if isinstance(self.net, Client):
            self.net.pg_verify, self.net.pg_verify_rx = {}, now
        else:
            def drain(end=now + 15):
                while time.time() < end:
                    for h, v in POSE.prog_out():
                        self._collect[h] = v
                        self._collect_rx = time.time()
                    time.sleep(0.01)
            threading.Thread(target=drain, daemon=True).start()
        self._collect_req = POSE.request_snapshot()

    def collect_result(self, now):
        """Ma table relue dans le jeu (None tant qu'elle arrive)."""
        if isinstance(self.net, Client) and self.net.pg_verify is not None:
            got, rx = self.net.pg_verify, self.net.pg_verify_rx
        else:
            got, rx = self._collect, self._collect_rx
        done, count = POSE.snapshot_done()
        if (done == self._collect_req and count and len(got) >= count) or now - self._collect_at > 15:
            if isinstance(self.net, Client):
                self.net.pg_verify = None
            return dict(got)
        return None

    @staticmethod
    def compare_flags(what, ref, got):
        same = sum(1 for h, v in ref.items() if got.get(h) == v)
        diff = [f"{h:08x}: voulu {v}, jeu {got.get(h)}" for h, v in ref.items() if h in got and got[h] != v]
        log(f"Vérification ({what}) : {same}/{len(ref)} drapeaux identiques ({len(diff)} différents)"
            + (" ; ex. " + ", ".join(diff[:10]) if diff else ""))

    def leave_tick(self):
        lf = getattr(self, "leave", None)
        if not lf or not POSE:
            return
        now = time.time()
        step = lf["step"]
        if step == "apply":
            if getattr(POSE, "_pg_pending", None) or now - lf["t"] < 2.5:
                return
            self.start_collect()
            lf.update(step="verify1", t=now)
        elif step == "verify1":
            got = self.collect_result(now)
            if got is None:
                return
            self.compare_flags("ma progression remise en mémoire, avant rechargement", lf["own"], got)
            try:
                restore_my_save(lf["backup"], keep_marker=True)
            except OSError as e:
                log(f"Impossible de remettre ma sauvegarde : {e}")
            self.reload_game("Retour à ta progression...", kind="restore")
            lf.update(step="reload", t=now, ingame=0.0)
        elif step == "reload":
            if getattr(self, "reloading", None) or POSE.game_state() != 2:
                lf["ingame"] = 0.0
                if now - lf["t"] > 90:
                    log("Retour à ma progression : le rechargement n'a pas abouti")
                    self.end_leave()
                return
            lf["ingame"] = lf["ingame"] or now
            if now - lf["ingame"] > 8:
                self.start_collect()
                lf.update(step="verify2", t=now)
        elif step == "verify2":
            got = self.collect_result(now)
            if got is None:
                return
            self.compare_flags("ma progression après rechargement", lf["own"], got)
            # Au chargement, la ville remet certains drapeaux de mission à sa façon (vu le 22/10 : efe44530 repassé
            # à 0, et la mission d'avant toujours affichée). On les remet une 2e fois, le jeu sauvegarde, et on
            # recharge encore : la ville est alors construite avec la bonne histoire.
            diff = [[h, v] for h, v in lf["own"].items() if h in got and got[h] != v and not volatile_flag(h, v, got[h])]
            if diff and not lf.get("round2"):
                POSE.prog_in(diff)
                try:
                    mt = os.path.getmtime(os.path.join(save_dir(), "PROF_SAVE"))
                except OSError:
                    mt = 0
                lf.update(step="apply2", t=now, round2=True, mtime=mt)
                log(f"Retour à ma progression : {len(diff)} drapeaux remis une 2e fois, puis rechargement")
                return
            self.end_leave()
        elif step == "apply2":
            try:
                mt = os.path.getmtime(os.path.join(save_dir(), "PROF_SAVE"))
            except OSError:
                mt = 0
            if getattr(POSE, "_pg_pending", None) or (mt <= lf["mtime"] and now - lf["t"] < 12):
                return                      # on attend la sauvegarde que le jeu fait après un changement de drapeau
            self.reload_game("Retour à ta progression...")
            lf.update(step="reload", t=now, ingame=0.0)

    def end_leave(self):
        self.leave = None
        if getattr(self, "quitting", False):
            if POSE:
                POSE.set_notice("")
            self.save()
            self.destroyed = True
            self.root.destroy()

    def take_host_save(self):
        host = self.v_ip.get().strip()
        if not host:
            self.save_msg = "Entre d'abord l'IP de l'hôte (dans Partie)."
            return
        if self.game_running():
            self.save_msg = "Ferme d'abord le jeu (il réécrit sa sauvegarde en quittant)."
            return
        self.save_msg = "Téléchargement de la progression de l'hôte..."
        self.root.update_idletasks()
        try:
            name, data = fetch_host_save(host)
            swapped = self.cfg.get("save_swapped")
            backup = use_host_save(name, data, make_backup=not swapped)
            if not swapped:
                self.cfg["save_swapped"] = {"from": name, "backup": backup, "at": time.strftime("%d/%m %H:%M")}
            else:
                swapped["from"] = name
            self.save()
            self.save_msg = f"C'est fait : lance le jeu, tu as la progression de {name}."
            log(f"Progression de {name} installée (la mienne : {self.cfg['save_swapped']['backup']})")
        except (OSError, ValueError) as e:
            self.save_msg = f"Impossible : {e}"

    def restore_save(self):
        swapped = self.cfg.get("save_swapped")
        if not swapped:
            self.save_msg = "Tu as déjà ta propre progression."
            return
        if self.game_running():
            self.save_msg = "Ferme d'abord le jeu, puis clique à nouveau."
            return
        try:
            restore_my_save(swapped.get("backup"))
            self.cfg.pop("save_swapped", None)
            self.save()
            self.save_msg = "Ta progression est revenue. (La copie reste dans le dossier « sauvegardes ».)"
            log("Ma progression a été remise")
        except OSError as e:
            self.save_msg = f"Impossible : {e}"

    def set_sounds(self):
        self.cfg["sounds"] = self.v_snd.get()
        self.save()
        if POSE:
            POSE.set_sounds(self.cfg["sounds"])

    def set_tod(self):
        self.cfg["sync_tod"] = self.v_tod.get()
        self.save()
        if POSE and self.mode:
            POSE.set_role({"host": 1, "join": 2}.get(self.mode, 3), self.cfg["sync_tod"])

    def set_echo_look(self, v):
        global ECHO_LOOK
        ECHO_LOOK = LOOKS.index(v) + 1
        self.cfg["echo_look"] = ECHO_LOOK
        self.save()

    def set_tags(self):
        self.cfg["nametags"] = self.v_tags.get()
        self.save()

    def set_mirror(self):
        self.cfg["mirror"] = self.v_mirror.get()
        self.save()

    # ---------------------------------------------------------------- parties
    def start(self, mode):
        name = self.v_name.get().strip()[:16]
        if not name:
            self.error = "Choisis d'abord un pseudo"
            return
        if mode == "join" and not self.v_ip.get().strip():
            self.error = "Entre l'IP de l'hôte pour rejoindre"
            return
        if getattr(self, "leave", None):
            # le retour à ma progression est en cours (rechargements) : le mélanger avec un nouveau monde
            # cassait tout (vu le 22/10 au soir)
            self.error = "Attends la fin du retour à ta progression (quelques secondes), puis réessaie"
            return
        self.stop()
        self.cfg["name"] = name
        self.cfg["ip"] = self.v_ip.get().strip()
        self.save()
        self.error = ""
        if mode == "join" and self.v_auto_save.get():
            self.join_with_host_save()
        try:
            if mode == "host":
                self.server = Server()
                self.server.save_name = name
                self.server.start()
                self.net = Client(name, "127.0.0.1", self.game)
                self.net.send_progress = True
                try:
                    self.save_server = SaveServer(name)
                    self.save_server.start()
                except OSError as e:
                    self.save_server = None
                    log(f"Partage de progression impossible : {e}")
            elif mode == "join":
                self.net = Client(name, self.cfg["ip"], self.game)
                # seulement si ma progression est à l'abri (copie + marqueur) : sinon on ne touche à rien
                self.net.can_apply = lambda: bool(self.cfg.get("save_swapped")) and read_marker() is not None
                self.net.on_own_flags = self.keep_own_flags
                self.net.snapshot_ready = lambda: bool(POSE and POSE.game_state() == 2 and self.link and self.link.active
                                                       and not getattr(self, "reloading", None))
                self.net.own_saved = lambda: bool(read_marker()) and load_own_flags(read_marker()["backup"]) is not None
            else:
                self.net = Demo(self.game)
        except OSError as e:
            self.error = f"Impossible de lancer le serveur ({e})"
            self.stop()
            return
        self.net.start()
        self.mode = mode
        if POSE:
            POSE.set_role({"host": 1, "join": 2}.get(mode, 3), self.cfg.get("sync_tod", True))
        self.radar = Radar(self.root, self.game, self.net, self.cfg, self.link)
        log(f"Démarrage mode {mode} en tant que {name}")

    def stop(self):
        for obj in (self.net, self.server, getattr(self, "save_server", None)):
            if obj:
                obj.stop()
        self.save_server = None
        if POSE:                    # plus personne : l'overlay fait disparaître les personnages tout de suite
            POSE.set_role(0)
            for _ in range(3):
                POSE.write_remotes([])
                time.sleep(0.05)
        if self.radar:
            self.radar.destroy()
        self.net = self.server = self.radar = None
        self.pg_reload = None
        self.pending_join = None
        self.mode = None

    def quit(self):
        if getattr(self, "quitting", False):
            return
        sw = self.cfg.get("save_swapped")
        if (sw and self.game_running() and load_own_flags(sw.get("backup")) is not None and POSE and self.link
                and self.link.active and POSE.game_state() == 2):
            # retour à ma progression dans le jeu (sans le fermer), puis coop se ferme tout seul
            self.quitting = True
            self.stop()
            self.save_msg = "Retour à ta progression dans le jeu... coop se fermera ensuite."
            if not getattr(self, "leave", None):
                self.start_leave(sw)
            return
        if sw and self.game_running():
            # Un rechargement en jeu ne remet PAS ma progression (le jeu garde l'histoire en mémoire, objets compris :
            # vu le 22/10 avec le grappin). Seul un redémarrage du jeu relit le fichier : on attend que le jeu soit
            # fermé, on remet ma sauvegarde, puis coop se ferme.
            from tkinter import messagebox
            wait = messagebox.askyesno(
                "MEC Coop", f"Ton jeu a encore le monde de {sw.get('from')}.\n\n"
                "Pour retrouver TA progression, il faut fermer le jeu (il la relit au démarrage).\n\n"
                "Oui : coop attend que tu fermes le jeu, remet ta progression, puis se ferme tout seul.\n"
                "Non : quitter maintenant (ta progression reviendra au prochain lancement de coop, jeu fermé).")
            if wait:
                self.notice("Ferme le jeu pour retrouver ta progression")
                self.save_msg = "Ferme le jeu : coop remettra ta progression puis se fermera."
                while self.game_running():
                    for _ in range(5):
                        self.root.update()
                        time.sleep(0.1)
                time.sleep(1.0)                     # le jeu a fini d'écrire en quittant
                sw = self.cfg.get("save_swapped")
                if sw:
                    try:
                        restore_my_save(sw.get("backup"))
                        self.cfg.pop("save_swapped", None)
                        log("Ma progression a été remise (jeu fermé) en quittant coop")
                    except OSError as e:
                        log(f"Impossible de remettre ma progression en quittant : {e}")
        elif sw:
            try:
                restore_my_save(sw.get("backup"))
                self.cfg.pop("save_swapped", None)
                log("Ma progression a été remise en quittant coop")
            except OSError as e:
                log(f"Impossible de remettre ma progression en quittant : {e}")
        if POSE:
            POSE.set_notice("")              # sans coop, personne ne l'effacerait (il resterait affiché dans le jeu)
        self.save()
        self.stop()
        self.root.destroy()

    # ---------------------------------------------------------------- affichage
    def refresh(self):
        if getattr(self, "destroyed", False):
            return
        try:
            self.refresh_once()
        except Exception as e:
            log(f"Erreur interface : {e}")
        self.root.after(250, self.refresh)

    def refresh_save(self):
        """Bandeau orange : invitation de mission, ou progression d'un autre en cours."""
        sw = self.cfg.get("save_swapped")
        follow = getattr(getattr(self, "net", None), "follow", None)
        lines = []
        if follow and follow.text:
            lines.append(follow.text)
        if sw:
            lines.append(f"Tu joues avec la progression de {sw.get('from')}. La tienne revient toute seule quand tu "
                         "arrêtes et fermes le jeu — garde coop ouvert jusque-là.")
        if self.save_msg:
            lines.append(self.save_msg)
        text = "\n".join(lines)
        if text != getattr(self, "_banner_text", None):
            self._banner_text = text
            self.l_banner.config(text=text)
            if text:
                self.banner.pack(fill="x", padx=16, pady=(0, 10))
            else:
                self.banner.pack_forget()
        want = bool(sw)
        if want != bool(self.b_mine.winfo_ismapped()):
            if want:
                self.b_mine.pack(anchor="e", padx=10, pady=(0, 10))
            else:
                self.b_mine.pack_forget()

    def do_teleport(self, name):
        others = getattr(self.net, "others", []) if self.net else []
        target = next((o for o in others if o.get("n") == name), None)
        if not POSE or not target:
            self.l_tp.config(text="Personne à rejoindre.", fg=ORANGE)
            return
        p = target.get("p")
        if not p or sum(abs(c) for c in p) < 1.0:
            # position inconnue (il charge, il est mort...) : on l'envoyait en 0,0,0, dans le vide (23/10)
            self.l_tp.config(text=f"Position de {name} inconnue pour l'instant, réessaie.", fg=ORANGE)
            if not self.l_tp.winfo_ismapped():
                self.l_tp.pack(anchor="w", pady=(6, 0))
            return
        self.tp_req = POSE.teleport(name_id(name), tuple(p) if p else None)
        self.l_tp.config(text=f"Téléportation vers {name}...", fg=ORANGE)
        if not self.l_tp.winfo_ismapped():
            self.l_tp.pack(anchor="w", pady=(6, 0))

    def refresh_teleport(self):
        if POSE and self.tp_req is not None:
            done, res = POSE.teleport_state()
            if done == self.tp_req and res in (1, 2):
                self.l_tp.config(text="Arrivé ✓" if res == 1 else "Impossible pour l'instant (chargement, cinématique ?)",
                                 fg=GREEN if res == 1 else ORANGE)
                self.tp_req = None
                self.tp_clear_at = time.time() + 4
        if self.tp_req is None and time.time() > getattr(self, "tp_clear_at", 0) and self.l_tp.winfo_ismapped():
            self.l_tp.pack_forget()

    def refresh_players(self):
        RoundButton, Toggle, Segmented, Dot = self.W
        tk = self.tk
        others = list(self.net.others[: MAX_PLAYERS - 1]) if self.net else []
        names = [o.get("n") for o in others if o.get("n")]
        if names != list(self.player_rows) or self.mode != getattr(self, "_rows_mode", None):
            self._rows_mode = self.mode
            for row in self.player_rows.values():
                row["frame"].destroy()
            self.player_rows = {}
            for i, n in enumerate(names):
                fr = tk.Frame(self.players_box, bg=CARD)
                fr.pack(fill="x", pady=3)
                d = Dot(fr, 10)
                d.set(COLORS[(i + 1) % len(COLORS)])
                d.pack(side="left", padx=(0, 8))
                self.label(fr, n, 11, TXT, True).pack(side="left")
                info = self.label(fr, "", 9, DIM)
                info.pack(side="left", padx=(8, 0))
                b = RoundButton(fr, "Y aller", lambda v=n: self.do_teleport(v), "secondary", height=28, width=78, size=9)
                if self.mode in ("host", "join"):
                    b.pack(side="right")
                self.player_rows[n] = {"frame": fr, "info": info}
        me = self.game.pos
        for o in others:
            row = self.player_rows.get(o.get("n"))
            if not row:
                continue
            ms = o.get("ms")
            if o.get("p") and me:
                d = math.dist((me[0], me[1], me[2]), (o["p"][0], o["p"][1], o["p"][2]))
                txt = f"{d:.0f} m"
            else:
                txt = "pas en jeu"
            if isinstance(ms, list) and len(ms) >= 2:
                txt += f"  ·  mission {mission_title(str(ms[1]))}"
            if o.get("h"):
                txt += "  ·  hôte"
            if row["info"]["text"] != txt:
                row["info"].config(text=txt)
        if not names:
            msg = {"host": "Personne pour l'instant : donne ton IP à tes potes.",
                   "join": "En attente de l'hôte...", "demo": "Le clone et le bot apparaissent dans le jeu."}.get(
                self.mode, "Lance ou rejoins une partie.")
            self.l_noplayers.config(text=msg)
            if not self.l_noplayers.winfo_ismapped():
                self.l_noplayers.pack(anchor="w")
        elif self.l_noplayers.winfo_ismapped():
            self.l_noplayers.pack_forget()

    def copy_ip(self):
        ips = local_ips()
        radmin = [ip for ip in ips if ip.startswith("26.")]
        self.root.clipboard_clear()
        self.root.clipboard_append((radmin or ips or [""])[0])
        self.b_copy.set(text="Copiée ✓")
        self.root.after(1500, lambda: self.b_copy.set(text="Copier"))

    def set_info(self, key, color, text):
        d, lb = self.info[key]
        d.set(color)
        if lb["text"] != text:
            lb.config(text=text)

    def refresh_once(self):
        g = self.game
        chip = {}
        # ---- jeu
        if g.pos:
            self.set_info("game", GREEN, f"Jeu : Faith trouvée ({g.pos[0]:.0f}, {g.pos[1]:.0f}, {g.pos[2]:.0f})")
            chip["game"] = GREEN
        else:
            self.set_info("game", ORANGE, f"Jeu : {g.status}")
            chip["game"] = ORANGE
        # ---- overlay
        lk = self.link
        if not lk:
            self.set_info("ovl", GREY, "Overlay : indisponible")
            chip["ovl"] = GREY
        elif not lk.wanted:
            self.set_info("ovl", GREY, "Overlay : désactivé (radar en fenêtre)")
            chip["ovl"] = GREY
        else:
            if self.mode and time.time() > self.next_inject:
                self.next_inject = time.time() + 2
                lk.ensure_injected(self.game.mem)
            if lk.active:
                self.set_info("ovl", GREEN, "Overlay : actif dans le jeu")
                chip["ovl"] = GREEN
            elif lk.error:
                self.set_info("ovl", ORANGE, f"Overlay : {lk.error}")
                chip["ovl"] = ORANGE
            else:
                self.set_info("ovl", GREY if not self.mode else ORANGE,
                              "Overlay : lance une partie" if not self.mode else "Overlay : démarrage...")
                chip["ovl"] = GREY if not self.mode else ORANGE

        self.refresh_teleport()
        self.save_flow()
        self.leave_tick()
        if getattr(self, "destroyed", False):   # coop vient de se fermer tout seul (retour terminé)
            return
        self.join_wait_tick()
        self.progress_flow()
        self.auto_restore()
        self.refresh_save()

        # ---- personnages 3D, sons, heure, mission
        st3 = POSE.status() if POSE else ""
        tods = POSE.tod_status() if POSE else ""
        snd = ""
        cbs = ""
        if " · combats : " in st3:
            st3, cbs = st3.split(" · combats : ", 1)
        if " · sons : " in st3:
            st3, snd = st3.split(" · sons : ", 1)
        if not (lk and lk.active):
            self.set_info("3d", GREY, "Personnages 3D : overlay inactif")
            chip["3d"] = chip["snd"] = GREY
        elif not st3:
            self.set_info("3d", ORANGE, "Personnages 3D : démarrage...")
            chip["3d"] = ORANGE
        else:
            ok3 = "animé" in st3
            col = GREEN if ok3 else (GREY if st3.startswith("0 ") else ORANGE)
            extra = (f"\nSons : {snd}" if snd else "") + (f"\nCombats : {cbs}" if cbs else "") + \
                    (f"\nHeure : {tods}" if tods else "")
            self.set_info("3d", col, f"Personnages 3D : {st3}{extra}")
            chip["3d"] = col
            chip["snd"] = (GREEN if self.cfg.get("sounds", True) else GREY) if snd and "indisponible" not in snd else GREY
        mine = POSE.my_mission() if POSE else None
        if mine:
            self.set_info("mission", GREEN, f"Mission en cours : {mission_title(mine['name'])}")
            if mine["name"] != getattr(self, "_last_mission", None):
                self._last_mission = mine["name"]
                log(f"Mission commencée : {mine['name']} (départ {mine['pos']})")
        else:
            self.set_info("mission", GREY, "Pas de mission en cours (balade libre)")
            if getattr(self, "_last_mission", None):
                log(f"Mission terminée : {self._last_mission}")
                self._last_mission = None

        # ---- orientation (radar)
        st = getattr(self.radar, "cal_state", None) if self.radar else None
        if st is None:
            self.set_info("cam", GREY, "Orientation : lance une partie")
        elif st == "cam" or st == "ok":
            self.set_info("cam", GREEN, "Orientation : caméra du jeu (exacte)" if st == "cam" else "Orientation : calibrée")
        else:
            self.set_info("cam", ORANGE, "Orientation : recherche de la caméra... tourne-toi un peu")

        # ---- réseau
        if not self.net:
            self.set_info("net", GREY, "Réseau : pas de partie")
            chip["net"] = GREY
        elif self.mode == "demo":
            self.set_info("net", GREY, "Réseau : test solo (pas de réseau)")
            chip["net"] = GREY
        elif self.net.connected:
            n = len(self.net.others)
            self.set_info("net", GREEN, f"Réseau : connecté · {n} autre(s) joueur(s)")
            chip["net"] = GREEN
        else:
            self.set_info("net", ORANGE, "Réseau : en attente de l'hôte (Radmin VPN lancé ? bonne IP ?)")
            chip["net"] = ORANGE
        for k, d in self.chips.items():
            d.set(chip.get(k, GREY))

        # ---- carte Partie et pastille d'état
        running = bool(self.mode)
        if running != bool(self.run_box.winfo_ismapped()):
            (self.idle_box.pack_forget if running else self.run_box.pack_forget)()
            (self.run_box if running else self.idle_box).pack(fill="x")
            self.player_rows_reset = True
            for row in self.player_rows.values():
                row["frame"].destroy()
            self.player_rows = {}
        host_name = next((o.get("n") for o in (self.net.others if self.net else []) if o.get("h")), None)
        titles = {"host": ("Tu héberges", "Tes potes rejoignent avec cette IP :"),
                  "join": (f"Avec {host_name}" if host_name and self.net.connected else "Connexion...",
                           "Connecté à l'hôte." if self.net and self.net.connected else
                           f"On cherche l'hôte {self.cfg.get('ip', '')}..."),
                  "demo": ("Test solo", "Ton clone (l'Écho) copie tes mouvements à 3 m, un bot tourne autour de toi.")}
        if running:
            t, sub = titles.get(self.mode, ("", ""))
            if self.l_mode["text"] != t:
                self.l_mode.config(text=t)
            if self.l_sub["text"] != sub:
                self.l_sub.config(text=sub)
        if self.mode == "host":
            ips = local_ips()
            radmin = [ip for ip in ips if ip.startswith("26.")]
            txt = radmin[0] if radmin else (ips[0] if ips else "?")
            if self.l_ips["text"] != txt:
                self.l_ips.config(text=txt)
            if not self.ip_box.winfo_ismapped():
                self.ip_box.pack(fill="x", pady=(8, 0))
        elif self.ip_box.winfo_ismapped():
            self.ip_box.pack_forget()
        if self.error:
            if self.l_err["text"] != self.error:
                self.l_err.config(text=self.error)
            if not self.l_err.winfo_ismapped():
                self.l_err.pack(fill="x", pady=(8, 0))
        elif self.l_err.winfo_ismapped():
            self.l_err.pack_forget()
        if not self.mode:
            state, col = "Prêt", GREY
        elif self.mode == "demo":
            state, col = "Test solo", ORANGE
        elif self.net and self.net.connected:
            state, col = ("En ligne" if self.mode == "host" else "Connecté"), GREEN
        else:
            state, col = "En attente", ORANGE
        if self.l_state["text"] != state:
            self.l_state.config(text=state)
        self.d_state.set(col)

        self.refresh_players()

        # garder les réglages d'accord avec les raccourcis Alt du jeu
        self.s_range.set(str(self.cfg.get("range", 150)))
        self.s_size.set(["Petit", "Moyen", "Grand"][self.cfg.get("size", 1) % 3])
        self.s_corner.set(CORNER_ICONS[self.cfg.get("corner", 0) % 4])
        if self.v_show.get() == self.cfg.get("hidden", False):
            self.v_show.set(not self.cfg.get("hidden", False))
            for t in self.toggles:
                t.draw()


    def run(self):
        self.root.mainloop()


def already_running():
    """Une seule fenêtre coop à la fois : deux coop ouverts se disputeraient les sauvegardes."""
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateMutexW.restype = ctypes.c_void_p
        h = k32.CreateMutexW(None, False, "Local\\MECCoopApp")
        err = ctypes.get_last_error()
        globals()["_APP_MUTEX"] = h
        return err == 183                         # ERROR_ALREADY_EXISTS
    except Exception:
        return False


def main():
    if already_running():
        try:
            import tkinter as tk
            from tkinter import messagebox
            r = tk.Tk()
            r.withdraw()
            messagebox.showinfo("MEC Coop", "Coop est déjà ouvert (regarde dans la barre des tâches).")
            r.destroy()
        except Exception:
            pass
        return
    App().run()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        log(traceback.format_exc())
        raise
