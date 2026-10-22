"""
faux_pote - projet coop Mirror's Edge Catalyst
Un faux joueur qui rejoint TA partie par le réseau, comme un vrai pote (même protocole, même port),
avec les retards irréguliers d'Internet. Il COPIE TES MOUVEMENTS (ta pose du corps) 4 m à côté de toi :
tu dois voir un personnage 3D qui bouge comme toi. Sans pose disponible, il court en rond.

Utilisation : dans coop, clique "Héberger", puis lance ce fichier.
"""
import json
import math
import os
import random
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # coop.py est un dossier au-dessus
import coop  # noqa: E402

NAME = "FauxPote"
RADIUS = 15.0        # mètres
SPEED = 6.0          # m/s (course)
FAKE_MISSION_AT = 25 # s : la fausse mission (test de « suivre en mission »)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    game = coop.Game()
    game.start()
    print("Recherche de Faith (lance une partie)...")
    while not game.pos:
        time.sleep(0.2)
    center = list(game.pos)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.05)
    print(f"{NAME} rejoint {host}:{coop.PORT} et court autour de l'endroit où tu es.")
    print("Ferme cette fenêtre pour l'arrêter.\n")
    try:
        link = coop.PoseLink()          # la mémoire des poses ouverte par coop (ta pose y est écrite par l'overlay)
    except Exception:
        link = None
    t0 = time.time()
    seen = 0.0
    last_t = None
    look, look_at = 1, 0.0
    announced = [False]
    pg_flags, pg_asked, pg_last, pg_shown = {}, False, 0.0, 0
    print(f"Test des missions : dans {FAKE_MISSION_AT} s, {NAME} « lance une mission » ici. Éloigne-toi d'ici d'ici là.\n")
    while True:
        t = time.time() - t0
        lp = link.local() if link else None
        msg = {}
        if lp and game.pos:
            # copie de toi, 4 m à côté
            p = [game.pos[0] + 4.0, game.pos[1], game.pos[2]]
            if lp["t"] != last_t:
                last_t = lp["t"]
                msg["q"] = coop.encode_pose(dict(lp, pos=(lp["pos"][0] + 4.0, lp["pos"][1], lp["pos"][2])))
        else:
            a = t * SPEED / RADIUS
            p = [center[0] + RADIUS * math.cos(a), center[1] + 1.5 * math.sin(t * 0.4), center[2] + RADIUS * math.sin(a)]
        if time.time() - look_at > 2:            # l'apparence du clone, réglée dans coop
            look_at = time.time()
            try:
                look = int(json.load(open(coop.CONFIG, encoding="utf-8")).get("echo_look", 1))
            except (OSError, ValueError):
                look = 1
        msg.update({"n": NAME, "p": [round(v, 2) for v in p], "l": look if look in (1, 2) else 1})
        # fausse mission : 25 s après le début, « lancée » là où tu étais au départ (pour tester Alt+M)
        if FAKE_MISSION_AT <= t < FAKE_MISSION_AT + 180:
            if not announced[0]:
                announced[0] = True
                print(f"  -> fausse mission lancée à ton point de départ : une invitation doit s'afficher dans le jeu,"
                      " appuie sur Alt+M pour y être téléporté.")
            msg["ms"] = [1, "MI_Test_Faux_Pote"] + [round(v, 2) for v in center]
        elif t >= FAKE_MISSION_AT + 180 and announced[0] and announced[0] != 2:
            announced[0] = 2
            print("  -> fausse mission terminée (l'invitation disparaît).")
        try:
            sock.sendto(json.dumps(msg).encode("utf-8"), (host, coop.PORT))
            sock.recvfrom(65536)
            # tes sons, rejoués par le faux pote (test des sons)
            sock.settimeout(0.002)
            try:
                while True:
                    data, _ = sock.recvfrom(65536)
                    try:
                        o = json.loads(data.decode("utf-8"))
                    except ValueError:
                        continue
                    if isinstance(o, dict) and isinstance(o.get("pg"), list):   # ta progression (test 2.2)
                        for f in o["pg"]:
                            if isinstance(f, list) and len(f) == 2:
                                pg_flags[f[0]] = f[1]
                        pg_last = time.time()
                    if isinstance(o, dict) and isinstance(o.get("a"), list):
                        sock.sendto(json.dumps({"n": NAME, "a": o["a"]}).encode("utf-8"), (host, coop.PORT))
            except (socket.timeout, OSError):
                pass
            sock.settimeout(0.05)
            if time.time() - seen > 2:
                print("  connecté à l'hôte ✓")
                seen = time.time() + 1e9
        except (socket.timeout, OSError, ConnectionResetError):
            if seen < 1e9 and time.time() - seen > 3:
                print("  pas de réponse : as-tu cliqué « Héberger » dans coop ?")
                seen = time.time()
        # test de la progression partagée : 5 s après le début, il demande toute ta progression
        if not pg_asked and t > 5:
            pg_asked = True
            print("  -> demande de toute ta progression (drapeaux de l'histoire)...")
            try:
                sock.sendto(json.dumps({"n": NAME, "pgreq": 1}).encode("utf-8"), (host, coop.PORT))
            except OSError:
                pass
        if pg_flags and len(pg_flags) != pg_shown and time.time() - pg_last > 1.5:
            pg_shown = len(pg_flags)
            print(f"  -> progression reçue : {pg_shown} drapeaux"
                  f" (ex. mission en cours 2563f4f5 = {pg_flags.get(0x2563f4f5, '?')})")
        # comme Internet : 30 envois/s, avec un retard irrégulier
        time.sleep(0.033 + random.uniform(-0.01, 0.02))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except Exception:
        import traceback
        traceback.print_exc()
        input("\nAppuie sur Entrée pour fermer...")
