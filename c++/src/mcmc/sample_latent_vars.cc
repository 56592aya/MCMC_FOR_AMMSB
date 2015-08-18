#include "mcmc/sample_latent_vars.h"

#include "mcmc/np.h"
#include "mcmc/random.h"

namespace mcmc {

/**
 * we need to calculate z_ab. We can use deterministic way to calculate this
 * for each k,  p[k] = p(z_ab=k|*) = \sum_{i}^{} p(z_ab=k, z_ba=i|*)
 * then we simply sample z_ab based on the distribution p.
 * this runs in O(K)
 */
int sample_z_ab_from_edge(bool y, const std::vector<double> &pi_a,
                          const std::vector<double> &pi_b,
                          const std::vector<double> &beta, double epsilon,
                          ::size_t K) {
  std::vector<double> p(K);

#ifdef EFFICIENCY_FOLLOWS_PYTHON
  for (::size_t i = 0; i < K; i++) {
    double tmp = std::pow(beta[i], y) * std::pow(1.0 - beta[i], 1 - y) *
                 pi_a[i] * pi_b[i];
    tmp += std::pow(epsilon, y) * std::pow(1.0 - epsilon, 1 - y) * pi_a[i] *
           (1.0 - pi_b[i]);
    p[i] = tmp;
  }

#else
  if (y == 1) {
    for (::size_t i = 0; i < K; i++) {
      // p_i = b_i * pa_i * pb_i + eps * pa_i * (1 - pb_i)
      //     = pa_i * pb_i * (b_i - eps) + eps * pa_i
      //     = pa_i * (pb_i * (b_i - eps) + eps)
      p[i] = pi_a[i] * (pi_b[i] * (beta[i] - epsilon) + epsilon);
    }
  } else {
    double one_eps = 1.0 - epsilon;
    for (::size_t i = 0; i < K; i++) {
      // p_i = (1 - b_i) * pa_i * pb_i + (1 - eps) * pa_i (1 - pb_i)
      //     = pa_i * pb_i * (1 - b_i - (1 - eps)) + (1 - eps) * pa_i
      //     = pa_i * (pb_i * (1 - b_i - (1 - eps)) + (1 - eps))
      //     = pa_i * (pb_i * (eps - b_i) + (1 - eps))
      p[i] = pi_a[i] * (pi_b[i] * (epsilon - beta[i]) + one_eps);
    }
  }
#endif

  for (::size_t k = 1; k < K; k++) {
    p[k] += p[k - 1];
  }

  double r = Random::random->random();
  double location = r * p[K - 1];
  return np::find_le(p, location);
}

}  // namespace mcmc