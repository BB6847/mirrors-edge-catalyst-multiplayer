"""Téléportation de secours : sort Faith d'une boucle de morts en l'envoyant sur un toit sûr de la ville.

Il faut que coop soit ouvert (n'importe quel mode, par ex. Test solo) pour que l'overlay soit dans le jeu.
Lance : py tests\\tp_secours.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import coop  # noqa: E402

# départ de la mission Reunion : un toit de la ville, accessible en balade libre
SAFE = (-2.06, 69.30, 293.18)


def main():
    pose = coop.PoseLink()
    print("Téléportation de secours vers", SAFE)
    for essai in range(1, 61):
        if pose.game_state() != 2:
            print("  le jeu charge... j'attends")
            time.sleep(1)
            continue
        req = pose.teleport(0, SAFE)
        end = time.time() + 12
        res = 0
        while time.time() < end:
            done, res = pose.teleport_state()
            if done == req and res in (1, 2):
                break
            time.sleep(0.1)
        if res == 1:
            print(f"OK : arrivé (essai {essai}). Bouge un peu, le jeu va sauvegarder ta nouvelle position.")
            break
        print(f"  essai {essai} raté (mort ou chargement pendant le saut), je réessaie")
        time.sleep(0.5)
    else:
        print("Pas réussi. Vérifie que coop est ouvert (bas de sa fenêtre : Overlay en vert).")
    input("Entrée pour fermer")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001
        print("Erreur :", e)
        input("Entrée pour fermer")
