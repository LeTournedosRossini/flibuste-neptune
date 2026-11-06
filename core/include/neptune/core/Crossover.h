#pragma once
// Séparateur de fréquences (Trident) : deux ou trois bandes par filtres de Linkwitz-Riley (deux Butterworth en cascade, pente 12, 24 ou 48 dB/octave).
//   - La somme des bandes a une réponse plate en amplitude (elle ne diffère du signal d'entrée que par la phase, celle d'un filtre passe-tout) : un trajet « complet »
//     reconstruit à partir des bandes est donc cohérent avec elles. Les LR de 12 dB/octave (ordre 2) ont leurs bandes en opposition de phase : la bande haute est inversée.
//   - Trois bandes : séparation à f2, puis la partie basse est coupée à f1 ; la bande haute passe par un filtre passe-tout de f1 (la somme de ses deux moitiés), pour que la
//     somme des trois bandes soit plate.
//   - Les fréquences sont lissées (pas de claquement quand on les déplace) ; les filtres sont redessinés par morceaux de 32 échantillons seulement s'ils ont bougé.
// Sans allocation, sans verrou ; fil audio.
#include <algorithm>
#include <array>
#include <cmath>

#include "neptune/core/Biquad.h"

namespace neptune::core::trident
{
class Crossover
{
public:
    static constexpr int kChunk = 32;

    void prepare (double sampleRate)
    {
        fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        smoothCoef_ = 1.0 - std::exp (-static_cast<double> (kChunk) / (fs_ * 0.02));   // constante de temps de 20 ms
        snap_ = true;
        reset();
    }
    void reset() noexcept
    {
        for (auto& c : chan_) c.reset();
    }
    void setBands (int bands) noexcept { bands_ = bands >= 3 ? 3 : 2; }
    int bands() const noexcept { return bands_; }
    void setSlope (int dbPerOctave) noexcept { slope_ = dbPerOctave >= 48 ? 48 : (dbPerOctave >= 24 ? 24 : 12); }
    int slope() const noexcept { return slope_; }
    // Fréquences visées (Hz) : f1 sépare grave et médium (grave et aigu à deux bandes), f2 sépare médium et aigu.
    void setFrequencies (double f1, double f2) noexcept
    {
        target1_ = f1;
        target2_ = f2;
    }

    // Sépare `n` échantillons. `lowL/R`, `midL/R`, `highL/R` : sorties (la bande médium est silencieuse à deux bandes). Les entrées et sorties ne se recouvrent pas.
    void process (const float* inL, const float* inR, float* lowL, float* lowR, float* midL, float* midR, float* highL, float* highR, int n) noexcept
    {
        for (int pos = 0; pos < n; pos += kChunk)
        {
            const int m = std::min (kChunk, n - pos);
            update();
            for (int i = pos; i < pos + m; ++i)
            {
                split (chan_[0], inL[i], lowL[i], midL[i], highL[i]);
                split (chan_[1], inR[i], lowR[i], midR[i], highR[i]);
            }
        }
    }

    // Réponse (amplitude) de chaque bande à la fréquence f, pour l'affichage : {grave, médium, aigu} avec les fréquences actuellement visées.
    std::array<double, 3> bandMagnitudes (double f) const noexcept
    {
        Crossover tmp;
        tmp.fs_ = fs_;
        tmp.bands_ = bands_;
        tmp.slope_ = slope_;
        tmp.design (clampF1 (target1_), clampF2 (target1_, target2_));
        // les modules des filtres du premier canal
        const auto& c = tmp.chan_[0];
        const double lp1 = c.b_lp.magnitude (fs_, f), hp1 = c.b_hp.magnitude (fs_, f);
        if (bands_ == 2) return { lp1, 0.0, hp1 };
        const double lp2 = c.a_lp.magnitude (fs_, f), hp2 = c.a_hp.magnitude (fs_, f);
        return { lp2 * lp1, lp2 * hp1, hp2 };
    }

private:
    // Un filtre de Linkwitz-Riley : deux Butterworth identiques en cascade.
    struct LR
    {
        ButterworthCascade a, b;
        void design (bool highpass, double fs, double f, int slope) noexcept
        {
            a.design (highpass, fs, f, slope / 2);
            b.design (highpass, fs, f, slope / 2);
        }
        float process (float x) noexcept { return b.process (a.process (x)); }
        void reset() noexcept { a.reset(); b.reset(); }
        double magnitude (double fs, double f) const noexcept { const double m = a.magnitude (fs, f); return m * m; }
    };
    struct Chan
    {
        LR a_lp, a_hp;   // séparation à f2 (trois bandes)
        LR b_lp, b_hp;   // séparation à f1 (de l'entrée à deux bandes, de la partie basse à trois)
        LR c_lp, c_hp;   // passe-tout de f1 sur la bande haute (trois bandes)
        void reset() noexcept { a_lp.reset(); a_hp.reset(); b_lp.reset(); b_hp.reset(); c_lp.reset(); c_hp.reset(); }
    };

    double clampF1 (double f) const noexcept { return std::clamp (f, 20.0, fs_ * 0.4); }
    double clampF2 (double f1, double f2) const noexcept { return std::clamp (f2, std::min (f1 * 1.1, fs_ * 0.45), fs_ * 0.45); }

    void update() noexcept
    {
        const double t1 = std::log (clampF1 (target1_));
        const double t2 = std::log (clampF2 (std::exp (t1), target2_));
        if (snap_) { log1_ = t1; log2_ = t2; snap_ = false; dirty_ = true; }
        else
        {
            const double n1 = log1_ + (t1 - log1_) * smoothCoef_, n2 = log2_ + (t2 - log2_) * smoothCoef_;
            if (std::fabs (n1 - log1_) > 1e-6 || std::fabs (n2 - log2_) > 1e-6) dirty_ = true;
            log1_ = n1;
            log2_ = n2;
        }
        if (dirty_ || bands_ != designedBands_ || slope_ != designedSlope_)
        {
            design (std::exp (log1_), std::exp (log2_));
            dirty_ = false;
        }
    }
    void design (double f1, double f2) noexcept
    {
        designedBands_ = bands_;
        designedSlope_ = slope_;
        for (auto& c : chan_)
        {
            c.b_lp.design (false, fs_, f1, slope_);
            c.b_hp.design (true, fs_, f1, slope_);
            if (bands_ == 3)
            {
                c.a_lp.design (false, fs_, f2, slope_);
                c.a_hp.design (true, fs_, f2, slope_);
                c.c_lp.design (false, fs_, f1, slope_);
                c.c_hp.design (true, fs_, f1, slope_);
            }
        }
    }

    void split (Chan& c, float x, float& low, float& mid, float& high) noexcept
    {
        const float sign = slope_ == 12 ? -1.0f : 1.0f;   // LR2 : les bandes sont en opposition de phase, on inverse la haute
        if (bands_ == 2)
        {
            low = c.b_lp.process (x);
            high = sign * c.b_hp.process (x);
            mid = 0.0f;
            return;
        }
        const float lowMid = c.a_lp.process (x);
        const float hi0 = sign * c.a_hp.process (x);
        low = c.b_lp.process (lowMid);
        mid = sign * c.b_hp.process (lowMid);
        high = c.c_lp.process (hi0) + sign * c.c_hp.process (hi0);
    }

    double fs_ = 48000.0, smoothCoef_ = 0.1;
    double target1_ = 250.0, target2_ = 3000.0, log1_ = 0.0, log2_ = 0.0;
    int bands_ = 3, slope_ = 24, designedBands_ = 0, designedSlope_ = 0;
    bool snap_ = true, dirty_ = true;
    std::array<Chan, 2> chan_;
};
} // namespace neptune::core::trident
