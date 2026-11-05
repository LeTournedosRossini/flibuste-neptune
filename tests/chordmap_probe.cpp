// Petit programme lancé par test_shared_chordmap.cpp : un second processus, pour vérifier la carte partagée entre deux processus (Linux et Windows).
//   chordmap_probe read <tag> <canal>   : lit la carte (accord 5 sur [0,4), accord 10 sur [4,8)) ; code 0 si c'est bien elle
//   chordmap_probe open <tag> <canal>   : tente d'ouvrir en écrivain ; code 0 si accepté, 2 si le canal est occupé par un écrivain vivant
#include <cstdio>
#include <cstdlib>
#include <string>

#include "neptune/core/SharedChordMap.h"

using namespace neptune::core::chordmap;

int main (int argc, char** argv)
{
    if (argc < 4) return 64;
    const std::string mode = argv[1], tag = argv[2];
    const int channel = std::atoi (argv[3]);
    if (mode == "read")
    {
        Reader r (tag);
        r.setChannel (channel);
        r.poll();
        Container c;
        const bool ok = r.connected() && r.lookup (1.0, Kind::Chord, c) && c.root == 5 && r.lookup (5.0, Kind::Chord, c) && c.root == 10;
        return ok ? 0 : 1;
    }
    if (mode == "open")
    {
        Writer w (tag);
        const auto s = w.open (channel);
        return s == Status::Ok ? 0 : s == Status::InUse ? 2 : 3;
    }
    return 64;
}
