"""
faux_hote - projet coop Mirror's Edge Catalyst
Un faux HÔTE, pour tester seul la progression partagée du côté de celui qui rejoint.
Il héberge sur ton PC (ta propre sauvegarde sert de « monde de l'hôte », donc rien ne change),
puis, quand ton coop lui demande sa progression, il envoie un drapeau de test qu'il fait changer
toutes les 3 s. Ton overlay doit l'appliquer dans ton jeu (journal : « drapeau de l'hôte ... appliqué »).

Utilisation :
  1. Coop ne doit PAS héberger. Lance ce fichier.
  2. Dans coop : IP = 127.0.0.1, clique Rejoindre (en partie dans le jeu).
  3. Quand tu as fini, ferme coop : ta sauvegarde est remise comme avant.
"""
import json
import os
import socket
import sys
import time

HERE_T = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE_T))

NAME = "FauxHote"
TEST_FLAG = 0x8f17e6dc     # un drapeau que le jeu fait déjà changer tout seul en jouant (0 / 1)


def crash(msg):
    try:
        with open(os.path.join(HERE_T, "faux_hote_erreur.txt"), "w", encoding="utf-8") as f:
            f.write(msg)
    except OSError:
        pass
    print(msg)
    input("\nAppuie sur Entrée pour fermer...")


def main():
    import coop
    try:
        server = coop.Server()
    except OSError as e:
        crash(f"Impossible d'héberger sur le port {coop.PORT} ({e}). Coop héberge-t-il déjà ? Arrête-le d'abord.")
        return
    server.save_name = NAME
    server.start()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.05)
    addr = ("127.0.0.1", coop.PORT)
    print(f"{NAME} héberge sur ce PC (port {coop.PORT}).")
    print("Dans coop : IP 127.0.0.1, puis Rejoindre (en partie dans le jeu).\n")
    asked = False
    value, next_flip, sent = 1, 0.0, 0
    joined = False
    while True:
        # je suis un joueur comme un autre (l'hôte, vu par le serveur)
        sock.sendto(json.dumps({"n": NAME, "p": None, "l": 2}).encode("utf-8"), addr)
        try:
            while True:
                data, _ = sock.recvfrom(65536)
                try:
                    o = json.loads(data.decode("utf-8"))
                except ValueError:
                    continue
                if isinstance(o, list) and any(p.get("n") != NAME for p in o if isinstance(p, dict)) and not joined:
                    joined = True
                    print("  quelqu'un a rejoint ✓")
                if isinstance(o, dict) and o.get("pgreq"):
                    if not asked:
                        print(f"  {o.get('n')} demande ma progression ✓ : envoi du drapeau de test toutes les 3 s")
                    asked = True
        except (socket.timeout, OSError):
            pass
        if asked and time.time() >= next_flip:
            next_flip = time.time() + 3
            value = 1 - value
            msg = {"n": NAME, "pg": [[TEST_FLAG, value]]}
            sock.sendto(json.dumps(msg).encode("utf-8"), addr)
            sent += 1
            print(f"  drapeau de test {TEST_FLAG:08x} = {value} envoyé ({sent})")
        time.sleep(0.033)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except BaseException:
        import traceback
        crash(traceback.format_exc())
