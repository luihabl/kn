#include "kn/collisions/reaction.h"

#include "kn/constants/constants.h"
#include "kn/particle/species.h"
#include "kn/random/random.h"

using namespace kn::collisions;

namespace {
void sample_from_sequence(size_t n,
                          size_t range,
                          std::vector<size_t>& sequence,
                          std::unordered_set<size_t>& used) {
    sequence.resize(n);
    used.clear();

    for (size_t i = 0; i < n; i++) {
        size_t num = 0;
        do {
            num = kn::random::uniform(range);
        } while (used.find(num) != used.end());

        used.insert(num);
        sequence[i] = num;
    }
}

template <unsigned NX>
double kinetic_energy_ev(const kn::particle::ChargedSpecies<NX, 3>& p, size_t idx) {
    const auto& v = p.v()[idx];
    return 0.5 * p.m() * (v.x * v.x + v.y * v.y + v.z * v.z) / kn::constants::e;
}

double collision_frequency(double neutral_density,
                           double cross_section,
                           double kinetic_energy,
                           double mass) {
    return neutral_density * cross_section *
           std::sqrt(2.0 * kn::constants::e * kinetic_energy / mass);
}

double calc_p_null(double nu_prime, double dt) {
    return 1.0 - std::exp(-nu_prime * dt);
}

double interpolate_cross_section(const CrossSection& cs, double energy) {
    if (energy <= cs.energy.front())
        return cs.cross_section.front();
    else if (energy >= cs.energy.back())
        return cs.cross_section.back();
    else {
        auto it = std::lower_bound(cs.energy.begin(), cs.energy.end(), energy);

        size_t rhs = it - cs.energy.begin();

        double x0 = cs.energy[rhs - 1];
        double x1 = cs.energy[rhs];
        double y0 = cs.cross_section[rhs - 1];
        double y1 = cs.cross_section[rhs];

        return y0 + (energy - x0) * (y1 - y0) / (x1 - x0);
    }
}

template <unsigned NX, unsigned NV>
double total_cs(double energy, const Reactions<NX, NV>& reactions) {
    double cs = 0.0;
    for (const auto& reaction : reactions)
        cs += interpolate_cross_section(reaction->cross_section, energy);
    return cs;
}

template <unsigned NX, unsigned NV>
double max_sigmav_for_cross_section(const CrossSection& cs,
                                    const Reactions<NX, NV>& reactions,
                                    double projectile_mass,
                                    bool slow_projectiles) {
    double nu_prime = 0.0;
    const double rmc = (slow_projectiles ? 2.0 : 1.0) * kn::constants::e / projectile_mass;

    for (size_t i = 0; i < cs.energy.size(); i++) {
        double energy = cs.energy[i];
        double tcs = total_cs<NX>(energy, reactions);
        double nu = tcs * std::sqrt(2.0 * energy * rmc);
        nu_prime = std::max(nu_prime, nu);
    }

    return nu_prime;
}

template <unsigned NX, unsigned NV>
double max_sigmav(const Reactions<NX, NV>& reactions,
                  kn::particle::ChargedSpecies<NX, NV>& projectile,
                  bool slow_projectiles) {
    double sigmav = 0.0;
    for (const auto& reaction : reactions)
        sigmav = std::max(sigmav, max_sigmav_for_cross_section(reaction->cross_section, reactions,
                                                               projectile.m(), slow_projectiles));
    return sigmav;
}
}  // namespace

template <unsigned NX, unsigned NV>
MCCReactionSet<NX, NV>::MCCReactionSet(particle::ChargedSpecies<NX, NV>& projectile,
                                       ReactionConfig<NX, NV>&& config)
    : m_projectile(projectile), m_config(std::move(config)) {
    m_max_sigmav = max_sigmav(m_config.m_reactions, m_projectile,
                              m_config.m_dyn == RelativeDynamics::SlowProjectile);
};

template <unsigned NX, unsigned NV>
void MCCReactionSet<NX, NV>::react_all() {
    double nu_prime = m_config.m_target->dens_max() * m_max_sigmav;
    double p_null = calc_p_null(nu_prime, m_config.m_dt);

    double n_null_f = p_null * (double)m_projectile.n();
    size_t n_null = (size_t)std::floor(n_null_f);
    n_null = (n_null_f - (double)n_null) > random::uniform() ? n_null + 1 : n_null;

    core::Vec<3> vrand;

    sample_from_sequence(n_null, m_projectile.n(), m_particle_samples, m_used_cache);

    for (size_t i = 0; i < n_null; i++) {
        size_t p_idx = m_particle_samples[i];

        auto& pp = m_projectile.x()[p_idx];
        auto& vp = m_projectile.v()[p_idx];

        if (m_config.m_dyn == RelativeDynamics::SlowProjectile) {
            double vth =
                std::sqrt(kn::constants::kb * m_config.m_target->temperature() / m_projectile.m());

            vrand = {kn::random::normal() * vth, kn::random::normal() * vth,
                     kn::random::normal() * vth};

            vp.x -= vrand.x;
            vp.y -= vrand.y;
            vp.z -= vrand.z;
        }

        double kinetic_energy = kinetic_energy_ev(m_projectile, p_idx);
        double r1 = random::uniform();

        double fr0 = 0.0;
        double fr1 = 0.0;

        bool collided = false;

        double dens_n = m_config.m_target->dens_at(pp);

        for (const auto& reaction : m_config.m_reactions) {
            fr0 = fr1;
            fr1 += collision_frequency(
                       dens_n,
                       interpolate_cross_section(
                           reaction->cross_section,
                           (m_config.m_dyn == RelativeDynamics::SlowProjectile ? 0.5 : 1.0) *
                               kinetic_energy),
                       kinetic_energy, m_projectile.m()) /
                   nu_prime;

            if (reaction->react(m_projectile, p_idx, kinetic_energy))
                break;
        }

        if (m_config.m_dyn == RelativeDynamics::SlowProjectile) {
            vp.x += vrand.x;
            vp.y += vrand.y;
            vp.z += vrand.z;
        }
    }
}

template class kn::collisions::MCCReactionSet<1, 3>;
template class kn::collisions::MCCReactionSet<2, 3>;
template class kn::collisions::MCCReactionSet<3, 3>;