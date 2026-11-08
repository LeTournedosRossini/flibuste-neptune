#pragma once
// Intervalles d'un accord : bit n = n demi-tons au-dessus de la fondamentale, SUR DEUX OCTAVES (n de 0 à 23) : une neuvième (14) n'est pas une seconde (2).
// Les anciennes valeurs sur 12 bits (une seule octave) restent valides.
#include <cstdint>

namespace neptune::core
{
constexpr int kIntervalBits = 24;
constexpr std::uint32_t kIntervalMask = (1u << kIntervalBits) - 1u;

// Les mêmes notes ramenées dans l'octave (classes de hauteur relatives, 12 bits) : pour ce qui raisonne en classes de hauteur.
constexpr std::uint16_t foldToOctave (std::uint32_t rel) noexcept
{
    std::uint16_t r = 0;
    for (int s = 0; s < kIntervalBits; ++s)
        if (((rel >> s) & 1u) != 0u) r = static_cast<std::uint16_t> (r | (1u << (s % 12)));
    return r;
}
} // namespace neptune::core
