"""
test_progression - projet coop Mirror's Edge Catalyst
Teste SEUL le partage de progression : fait exactement la demande que fait ton pote en cliquant
« Rejoindre », mais ne touche à RIEN (aucune sauvegarde écrite, jeu pas fermé).

Utilisation : dans coop (v1.2), clique "Héberger", puis double-clique ce fichier.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import coop  # noqa: E402


def essai(ip, label):
    print(f"\n{label} ({ip}) :")
    t = time.time()
    try:
        name, data = coop.fetch_host_save_udp(ip)
    except OSError as e:
        print(f"  ÉCHEC : {e}")
        return False
    mine = coop.read_my_save()
    same = mine is not None and (data == mine or abs(len(data) - len(mine)) < 4096)
    print(f"  reçu la progression de {name} : {len(data) // 1024} Ko en {time.time() - t:.1f} s")
    print("  identique à ta sauvegarde ✓" if same else "  ATTENTION : différente de ta sauvegarde")
    return True


def main():
    print("Test du partage de progression (rien n'est modifié)")
    ok = essai("127.0.0.1", "1) Directement")
    vpn = [ip for ip in coop.local_ips() if ip.startswith("26.")]
    if vpn:
        ok = essai(vpn[0], "2) Par ton adresse Radmin, celle que ton pote utilise") and ok
    else:
        print("\n2) Radmin VPN pas trouvé : lance-le pour tester l'adresse de ton pote.")
    print("\nRÉSULTAT :", "tout marche ✓" if ok else "problème : as-tu cliqué « Héberger » dans coop v1.2 ?")
    input("\nEntrée pour fermer...")


if __name__ == "__main__":
    main()
