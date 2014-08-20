#ifndef MCMC_VARIATIONAL_INFERENCE_STOCHASTIC_H__
#define MCMC_VARIATIONAL_INFERENCE_STOCHASTIC_H__

#include <unordered_map>
#include <cmath>

#include <boost/math/special_functions/digamma.hpp>

#include "mcmc/np.h"

#include "mcmc/types.h"
#include "mcmc/data.h"
#include "mcmc/random.h"
#include "mcmc/estimate_phi.h"
#include "mcmc/learning/learner.h"

namespace mcmc {
namespace learning {

typedef std::unordered_map<Edge, std::vector<double> >	PhiMap;

/**
 *  Stochastic variational inference for assortive mixture membership stochastic model.
 *  The implementation is based on the paper:
 *      http://www.cs.princeton.edu/~blei/papers/GopalanMimnoGerrishFreedmanBlei2012.pdf
 *
 *  Formally, each node can be belong to multiple communities which we can represent it by
 *  distribution of communities. For instance, if we assume there are total K communities
 *  in the graph, then each node a, is attached to community distribution \pi_{a}, where
 *  \pi{a} is K dimensional vector, and \pi_{ai} represents the probability that node a
 *  belongs to community i, and \pi_{a0} + \pi_{a1} +... +\pi_{aK} = 1
 *
 *  Also, there is another parameters called \beta representing the community strength, where
 *  \beta_{k} is scalar.
 *
 *  In summary, the model has the parameters:
 *  Prior: \alpha, \eta
 *  Parameters: \pi, \beta
 *  Latent variables: z_ab, z_ba
 *  Observations: y_ab for every link.
 *
 *  And our goal is to estimate the posterior given observations and priors:
 *  p(\pi,\beta | \alpha,\eta, y).
 *
 *  Due to the intractability of this posterior, we adopt approximate inference - variational inference
 *  More specifically, using the mean-field variational inference.
 *
 *  In this implementation, we introduce sets of variational parameters.
 *  q(z_ab) ~ Mul(phi_ab)     phi_ab is K dimensional vector, where sum equals to 1
 *  q(z_ba) ~ Mul(phi_ba)     phi_ba is K dimensional vector, where sum equals to 1
 *  q(pi_a) ~ Dir(gamma_a)    gamma_a is K dimensional vector,  for each node a.
 *  q(beta_k) ~ Beta(lamda_k) lamda_k is 2 dimensional vector, each denotes the beta shape parameters.
 *
 *  TODO:  need to create base class for sampler, and MCMC sampler and variational inference should inherit
 *         from that base class.
 */
class SVI : public Learner {

public:
	/**
	 * Initialize the sampler using the network object and arguments (i.e prior)
	 * Arguments:
	 * 	   network:    representation of the graph.
	 * 	   args:       containing priors, control parameters for the model.
	 */
	SVI(const Options &args, const Network &network)
   			: Learner(args, network) {
		// variational parameters.
		lamda = Random::random.gamma(eta[0], eta[1], K, 2);	// variational parameters for beta
		gamma = Random::random.gamma(1, 1, N, K);			// variational parameters for pi
		std::cerr << "gamma.size() " << gamma.size() << " gamma[0].size() " << gamma[0].size() << std::endl;
		update_pi_beta();
		// step size parameters.
		kappa = args.b;
		tao = args.c;

		// control parameters for learning
		online_iterations = 50;
		phi_update_threshold = 0.0001;

		// lift
		log_epsilon = log(epsilon);
		log_1_epsilon = log(1.0 - epsilon);
	}


	virtual ~SVI() {
	}


	virtual void run() {
		/*
		   stochastic variational optimization.
		   while not converge
		   sample mini-batch node pairs E_t  from network (trianing)
		   for each node pair (a,b) in E_t
		   optimize (phi_ab, phi_ba)
		   for n = [0,..,N-1], k=[0,..K-1]
		   calculate the gradient for gamma_nk
		   for k = [0,,,K-1], i=[0,1]
		   calculate the gradient for lammda_ki
		   update (gamma, lamda) using gradient:
		   new_value = (1-p_t)*old_value + p_t * new value.
		 */

		// pr = cProfile.Profile()
		// pr.enable()

		// running until convergence.
		step_count++;

		while (step_count < max_iteration and !is_converged()) {
			// (mini_batch, scale) = network.sample_mini_batch(mini_batch_size, "stratified-random-node")
			EdgeSample edgeSample = network.sample_mini_batch(mini_batch_size, strategy::STRATIFIED_RANDOM_NODE);
			const EdgeSet &mini_batch = *edgeSample.first;
			double scale = edgeSample.second;

			// evaluate model after processing every 10 mini-batches.
			if (step_count % 2 == 1) {
				double ppx_score = cal_perplexity_held_out();
				std::cout << "perplexity for hold out set is: " << ppx_score << std::endl;
				ppxs_held_out.push_back(ppx_score);
				if (ppx_score < 13.0) {
					// we will use different step size schema
					stepsize_switch = true;
				}
			}

			// update (phi_ab, phi_ba) for each edge
			PhiMap phi;	// mapping (a,b) => (phi_ab, phi_ba)
			sample_latent_vars_for_edges(&phi, mini_batch);
			update_gamma_and_lamda(phi, mini_batch, scale);
			update_pi_beta();

			step_count++;
		}

#if 0
		pr.disable()
			s = StringIO.StringIO()
			sortby = 'cumulative'
			ps = pstats.Stats(pr, stream=s).sort_stats(sortby)
			ps.print_stats()
			print s.getvalue()
#endif
	}


protected:
	void sample_latent_vars_for_edges(PhiMap *phi, const EdgeSet &mini_batch) const {
		for (EdgeSet::const_iterator edge = mini_batch.begin();
				 edge != mini_batch.end();
				 edge++) {
			int a = edge->first;
			int b = edge->second;
			//estimate_phi_for_edge(edge, phi)  // this can be done in parallel.

			DoubleVectorPair phi_abba = sample_latent_vars_for_each_pair(a, b, gamma[a], gamma[b],
																		 lamda, K, phi_update_threshold,
																		 epsilon, online_iterations,
																		 network.get_linked_edges());
			(*phi)[Edge(a,b)]=phi_abba.first;
			(*phi)[Edge(b,a)]=phi_abba.second;

			//estimate_phi_for_edge(edge, phi)
		}
	}

	void update_pi_beta() {
#if 0
		pi = gamma/np.sum(gamma,1)[:,np.newaxis];
		temp = lamda/np.sum(lamda,1)[:,np.newaxis];
		beta = temp[:,1];
#else
#if 0
		std::vector<double> gamma_row_sum(gamma.size());
		std::vector<double> lamda_row_sum(lamda.size());
		np::row_sum(&gamma_row_sum, gamma);
		np::row_sum(&lamda_row_sum, lamda);
#endif

		std::cerr << "Ignore, both pi and beta are unused in this learner" << std::endl;
		// throw UnimplementedException(__func__);
#endif
	}


	void update_gamma_and_lamda(PhiMap &phi, const EdgeSet &mini_batch, double scale) {

		bool flag = false;
		// calculate the gradient for gamma
		std::vector<std::vector<double> > grad_lamda(K, std::vector<double>(2, 0.0));
		std::unordered_map<int, std::vector<double> > grad_gamma;	// ie. grad[a] = array[] which is K dimensional vector
		std::unordered_map<int, ::size_t> counter;	// used for scaling
		for (EdgeSet::const_iterator edge = mini_batch.begin();
				 edge != mini_batch.end();
				 edge++) {
			/*
			 * calculate the gradient for gamma
			 */
			int a = edge->first;
			int b = edge->second;
			const std::vector<double> &phi_ab = phi[Edge(a,b)];
			const std::vector<double> &phi_ba = phi[Edge(b,a)];
			if (grad_gamma.find(a) != grad_gamma.end()) {
				for (::size_t k = 0; k < phi_ab.size(); k++) {
					grad_gamma[a][k] += phi_ab[k];
				}
				counter[a]++;
			} else {
				grad_gamma[a].resize(K);
				for (::size_t k = 0; k < phi_ab.size(); k++) {
					grad_gamma[a][k] = phi_ab[k];
				}
				counter[a] = 1;
			}

			if (grad_gamma.find(b) != grad_gamma.end()) {
				for (::size_t k = 0; k < phi_ba.size(); k++) {
					grad_gamma[b][k] += phi_ba[k];
				}
				counter[b]++;
			} else {
				grad_gamma[b].resize(K);
				for (::size_t k = 0; k < phi_ba.size(); k++) {
					grad_gamma[b][k] = phi_ba[k];
				}
				counter[b] = 1;
			}
#if 0
		}

		for (EdgeSet::const_iterator edge = mini_batch.begin();
				 edge != mini_batch.end();
				 edge++) {
#endif
			/*
			 * calculate the gradient for lambda
			 */
			int y = 0;
			if (network.get_linked_edges()->find(*edge) != network.get_linked_edges()->end()) {
				y = 1;
				flag = true;
			}
#if 0
			std::cerr << "RFHH: guess need to again define phi_ab and phi_ba" << std::endl;
			int a = edge->first;
			int b = edge->second;
			const std::vector<double> &phi_ab = phi[Edge(a,b)];
			const std::vector<double> &phi_ba = phi[Edge(b,a)];
#endif
			for (::size_t k = 0; k < K; k++) {
				grad_lamda[k][0] += phi_ab[k] * phi_ba[k] * y;
				grad_lamda[k][1] += phi_ab[k] * phi_ba[k] * (1-y);
			}
		}


		// update gamma, only update node in the grad
		double p_t;
		if (! stepsize_switch) {
			p_t = std::pow(1024 + step_count, -0.5);
		} else {
			p_t = 0.01* std::pow(1+step_count/1024.0, -0.55);
		}

		for (std::unordered_map<int, std::vector<double> >::iterator node = grad_gamma.begin();
			 	node != grad_gamma.end();
				node++) {
			if (counter[node->first] == 0) {
				continue;
			}

			// std::vector<double> gamma_star(K);
			double scale1 = 1.0;
			if (! flag) {
				scale1 = N/(counter[node->first]*1.0);
			}

			if (step_count > 400) {
				for (::size_t k = 0; k < K; k++) {
					// gamma_star[k] = (1-p_t)*gamma[node->first][k] + p_t * (alpha[k] + scale1 * grad_gamma[node->first][k]);
					// gamma[node->first][k] = (1-1.0/(step_count))*gamma[node->first][k] + 1.0/(step_count)*gamma_star[k];
					double gamma_star = (1-p_t)*gamma[node->first][k] + p_t * (alpha + scale1 * grad_gamma[node->first][k]);
					gamma[node->first][k] = (1-1.0/(step_count))*gamma[node->first][k] + 1.0/(step_count)*gamma_star;
				}
			} else {
				for (::size_t k = 0; k < K; k++) {
					gamma[node->first][k]=(1-p_t)*gamma[node->first][k] + p_t * (alpha + scale1 * grad_gamma[node->first][k]);
				}
			}
		}

		// update lamda
		for (::size_t k = 0; k < K; k++) {

			if (step_count > 400) {
				double lamda_star_0 = (1-p_t)*lamda[k][0] + p_t *(eta[0] + scale * grad_lamda[k][0]);
				double lamda_star_1 = (1-p_t)*lamda[k][1] + p_t *(eta[1] + scale * grad_lamda[k][1]);
				lamda[k][0] = (1-1/(step_count)) * lamda[k][0] +1/(step_count)*lamda_star_0;
				lamda[k][1] = (1-1.0/(step_count)) * lamda[k][1] +1.0/(step_count)*lamda_star_1;
			} else {
				lamda[k][0] = (1-p_t)*lamda[k][0] + p_t *(eta[0] + scale * grad_lamda[k][0]);
				lamda[k][1] = (1-p_t)*lamda[k][1] + p_t *(eta[1] + scale * grad_lamda[k][1]);
			}
		}
	}


	void estimate_phi_for_edge(const Edge &edge, PhiMap *phi) {

		/**
		calculate (phi_ab, phi_ba) for given edge : (a,b)
		(a) calculate phi_ab given phi_ba
			if y =0:
		phi_ab[k]=exp(psi(gamma[a][k])+phi_ba[k]*(psi(lamda[k][1])-psi(lambda[k0]+lambda[k][1]))-phi_ba[k]*log(1-epsilon))
			if y=1:
		phi_ab[k]=exp(psi(gamma[a][k])+phi_ba[k]*(psi(lamda[k][0])-psi(lambda[k0]+lambda[k][1]))-phi_ba[k]*log(epsilon))

		(b) calculate phi_ba given phi_ab
			if y =0:
		phi_ba[k]=exp(psi(gamma[b][k])+phi_ab[k]*(psi(lamda[k][1])-psi(lambda[k0]+lambda[k][1]))-phi_ab[k]*log(1-epsilon))
			if y=1:
		phi_ba[k]=exp(psi(gamma[b][k])+phi_ab[k]*(psi(lamda[k][0])-psi(lambda[k0]+lambda[k][1]))-phi_ab[k]*log(epsilon))

		*/

		using ::boost::math::digamma;

		int a = edge.first;
		int b = edge.second;
		// initialize
		std::vector<double> phi_ab(K, 1.0 / K);
		std::vector<double> phi_ba(K, 1.0 / K);

		bool y = false;
		if (network.get_linked_edges()->find(edge) != network.get_linked_edges()->end()) {
			y = true;
		}

		// alternatively update phi_ab and phi_ba, until it converges
		// or reach the maximum iterations.
		for (::size_t i = 0; i < online_iterations; i++) {
			std::vector<double> phi_ab_old(phi_ab);
			std::vector<double> phi_ba_old(phi_ba);

			// first, update phi_ab
			for (::size_t k = 0; k < (K); k++) {
				if (y) {
					double u = -phi_ba[k]* log_epsilon;
					phi_ab[k] = std::exp(digamma(gamma[a][k])+phi_ba[k]*\
										 (digamma(lamda[k][0])-digamma(lamda[k][0]+lamda[k][1]))+u);
				} else {
					double u = -phi_ba[k]* log_1_epsilon;
					phi_ab[k] = std::exp(digamma(gamma[a][k])+phi_ba[k]*\
										 (digamma(lamda[k][1])-digamma(lamda[k][0]+lamda[k][1]))+u);
				}
			}
			double sum_phi_ab = np::sum(phi_ab);
			for (::size_t k = 0; k < phi_ab.size(); k++) {
				phi_ab[k] /= sum_phi_ab;
			}

			// then update phi_ba
			for (::size_t k = 0; k < K; k++) {
				if (y) {
					double u = -phi_ab[k]* log_epsilon;
					phi_ba[k] = std::exp(digamma(gamma[b][k])+phi_ab[k]*
										 (digamma(lamda[k][0])-digamma(lamda[k][0]+lamda[k][1]))+u);
				} else {
					double u = -phi_ab[k]* log_1_epsilon;
					phi_ba[k] = std::exp(digamma(gamma[b][k])+phi_ab[k]*
										 (digamma(lamda[k][1])-digamma(lamda[k][0]+lamda[k][1]))+u);
				}
			}

			double sum_phi_ba = np::sum(phi_ba);
			for (::size_t k = 0; k < phi_ba.size(); k++) {
				phi_ba[k] /= sum_phi_ba;
			}

			// calculate the absolute difference between new value and old value
			// diff1 = np.sum(np.abs(phi_ab - phi_ab_old))
			// diff2 = np.sum(np.abs(phi_ba - phi_ba_old))
			double diff1 = np::sum_abs(phi_ab, phi_ab_old);
			double diff2 = np::sum_abs(phi_ba, phi_ba_old);
			if (diff1 < phi_update_threshold and diff2 < phi_update_threshold) {
				break;
			}
		}

		(*phi)[Edge(a,b)] = phi_ab;
		(*phi)[Edge(b,a)] = phi_ba;
	}

protected:
	std::vector<std::vector<double> > lamda;	// variational parameters for beta
	std::vector<std::vector<double> > gamma;	// variational parameters for pi
	double kappa;
	double tao;

	// control parameters for learning
	::size_t online_iterations;
	double phi_update_threshold;

	double log_epsilon;
	double log_1_epsilon;
};

}	// namespace learning
}	// namespace mcmc

#endif	// ndef MCMC_VARIATIONAL_INFERENCE_STOCHASTIC_H__