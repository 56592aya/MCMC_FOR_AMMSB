#ifndef MCMC_CLSAMPLER_STOCHASTIC_H_
#define MCMC_CLSAMPLER_STOCHASTIC_H_

#include <memory>
#include <algorithm>
#include <chrono>

#include "mcmc_sampler_stochastic.h"

#include "opencl/context.h"

#define PARALLELISM 1

namespace mcmc {
namespace learning {

class MCMCClSamplerStochastic : public MCMCSamplerStochastic {
public:
	MCMCClSamplerStochastic(const Options &args, const Network &graph, const cl::ClContext clContext)
		: MCMCSamplerStochastic(args, graph), clContext(clContext) {

		std::ostringstream opts;
		opts << "-IOpenCL/include"
			 << " -DNEIGHBOR_SAMPLE_SIZE=" << real_num_node_sample()
			 << " -DK=" << K
			 << " -DMAX_NODE_ID=" << N
			 << " -DRAND_MAX=" << std::numeric_limits<uint64_t>::max();
		progOpts = opts.str();

		std::cout << "COMPILE OPTS: " << progOpts << std::endl;

		std::cout << "num_node_sample = " << num_node_sample << std::endl;

		init_graph();

		sampler_program = this->clContext.createProgram("OpenCL/sampler.cl", progOpts);
		sample_latent_vars_and_update_pi_kernel = cl::Kernel(sampler_program, "sample_latent_vars_and_update_pi");
		sample_latent_vars2_kernel = cl::Kernel(sampler_program, "sample_latent_vars2");
		update_beta_calculate_grads_kernel = cl::Kernel(sampler_program, "update_beta_calculate_grads");
		update_beta_calculate_theta_kernel = cl::Kernel(sampler_program, "update_beta_calculate_theta");

		clNodes = cl::Buffer(clContext.context, CL_MEM_READ_ONLY,
				N * sizeof(cl_int) // max: 2 unique nodes per edge in batch
				// FIXME: better estimate for #nodes in mini batch
				);
		clNodesNeighbors = cl::Buffer(clContext.context, CL_MEM_READ_ONLY,
				N * real_num_node_sample() * sizeof(cl_int) // #total_nodes x #neighbors_per_node
				// FIXME: we don't need space for all N elements. Space should be limited to #nodes_in_mini_batch * num_node_sample (DEPENDS ON ABOVE)
				);
		clEdges = cl::Buffer(clContext.context, CL_MEM_READ_ONLY,
				N * sizeof(cl_int2) // #total_nodes x #neighbors_per_node
				// FIXME: should be multiple of mini_batch_size
				);
		clPi = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				N * K * sizeof(cl_double) // #total_nodes x #K
				);
		clPiUpdate = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				N * K * sizeof(cl_double) // #total_nodes x #K
				);
		clPhi = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				N * K * sizeof(cl_double) // #total_nodes x #K
				);
		clBeta = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				K * sizeof(cl_double) // #K
				);
		clTheta = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				2 * K * sizeof(cl_double) // 2 x #K
				);
		clThetaSum = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				K * sizeof(cl_double) // #K
				);
		clZ = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				N * K * sizeof(cl_int) // #total_nodes x #K
				);
		clRandomNK = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				N * K * sizeof(cl_double) // at most #total_nodes
				);
		clScratch = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				std::max(
						N * K * sizeof(cl_double), // #total_nodes x #K
						N * K * sizeof(cl_double3)
					)
				);
		clRandomSeed = cl::Buffer(clContext.context, CL_MEM_READ_WRITE,
				PARALLELISM * sizeof(cl_ulong2)
				);
		for (unsigned i = 0; i < N; ++i) {
			// copy phi
			clContext.queue.enqueueWriteBuffer(clPhi, CL_TRUE,
					i * K * sizeof(cl_double),
					K * sizeof(cl_double),
					phi[i].data());
			// Copy pi
			clContext.queue.enqueueWriteBuffer(clPi, CL_TRUE,
					i * K * sizeof(double),
					K * sizeof(double),
					pi[i].data());
		}

		// copy theta
		std::vector<cl_double2> vclTheta(theta.size());
		std::transform(theta.begin(), theta.end(), vclTheta.begin(), [](const std::vector<double>& in){
			cl_double2 ret;
			ret.s[0] = in[0];
			ret.s[1] = in[1];
			return ret;
		});
		clContext.queue.enqueueWriteBuffer(clTheta, CL_TRUE,
				0, theta.size() * sizeof(cl_double2),
				vclTheta.data());

		std::vector<cl_ulong2> randomSeed(PARALLELISM);
		for (unsigned int i = 0; i < randomSeed.size(); ++i) {
			randomSeed[i].s[0] = 42 + i;
			randomSeed[i].s[1] = 42 + i + 1;
		}
		clContext.queue.enqueueWriteBuffer(clRandomSeed, CL_TRUE,
				0, randomSeed.size() * sizeof(cl_ulong2),
				randomSeed.data());

		info(std::cout);
	}

	virtual void run() {
		using namespace std::chrono;
		/** run mini-batch based MCMC sampler, based on the sungjin's note */

		if (step_count % 1 == 0) {
			double ppx_score = cal_perplexity_held_out();
			std::cout << std::fixed << std::setprecision(15) << "perplexity for hold out set is: " << ppx_score << std::endl;
			ppxs_held_out.push_back(ppx_score);
		}

		STAT g_stat;

		while (step_count < max_iteration && ! is_converged()) {
			auto l1 = std::chrono::system_clock::now();
			time_point<system_clock> t1, t2, t3, t4, t5, t6, t7, t8;
			t1 = system_clock::now();

			// (mini_batch, scale) = self._network.sample_mini_batch(self._mini_batch_size, "stratified-random-node")
			EdgeSample edgeSample = network.sample_mini_batch(mini_batch_size, strategy::STRATIFIED_RANDOM_NODE);
			const OrderedEdgeSet &mini_batch = *edgeSample.first;
			double scale = edgeSample.second;

			t2 = system_clock::now();

			// iterate through each node in the mini batch.
			OrderedVertexSet nodes = nodes_in_batch(mini_batch);

			t3 = system_clock::now();

			sample_latent_vars_and_update_pi(nodes);

			t4 = system_clock::now();

			// sample (z_ab, z_ba) for each edge in the mini_batch.
			// z is map structure. i.e  z = {(1,10):3, (2,4):-1}
			sample_latent_vars2(mini_batch);

			t5 = system_clock::now();

			update_beta(mini_batch, scale);

			t6 = system_clock::now();


			if (step_count % 1 == 0) {
				double ppx_score = cal_perplexity_held_out();
				std::cout << std::fixed << std::setprecision(12) << "perplexity for hold out set is: " << ppx_score << std::endl;
				ppxs_held_out.push_back(ppx_score);
			}

			t7 = system_clock::now();

			delete edgeSample.first;

			step_count++;
			auto l2 = std::chrono::system_clock::now();
			std::cout << "LOOP  = " << (l2-l1).count() << std::endl;

			STAT stat = {
				duration_cast<STAT::tick>(t2-t1),
				duration_cast<STAT::tick>(t3-t2),
				duration_cast<STAT::tick>(t4-t3),
				STAT::tick(0),
				duration_cast<STAT::tick>(t5-t4),
				duration_cast<STAT::tick>(t6-t5),
				duration_cast<STAT::tick>(t7-t6),
			};
			std::cout << stat;
			g_stat += stat;
		}
		std::cout << g_stat;
	}

protected:

	void update_beta(const OrderedEdgeSet &mini_batch, double scale) {
		// copy theta_sum
		std::vector<cl_double> vThetaSum(theta.size());
		std::transform(theta.begin(), theta.end(), vThetaSum.begin(), np::sum<double>);
		clContext.queue.enqueueWriteBuffer(clThetaSum, CL_FALSE,
				0, theta.size() * sizeof(cl_double),
				vThetaSum.data());

		update_beta_calculate_grads_kernel.setArg(0, clGraph);
		update_beta_calculate_grads_kernel.setArg(1, clEdges);
		update_beta_calculate_grads_kernel.setArg(2, (cl_int)mini_batch.size());
		update_beta_calculate_grads_kernel.setArg(3, clZ);
		update_beta_calculate_grads_kernel.setArg(4, clTheta);
		update_beta_calculate_grads_kernel.setArg(5, clThetaSum);
		update_beta_calculate_grads_kernel.setArg(6, clScratch);
		update_beta_calculate_grads_kernel.setArg(7, (cl_double)scale);

		::size_t countPartialSums = std::min(mini_batch.size(), (::size_t)PARALLELISM);

		clContext.queue.finish(); // Wait for sample_latent_vars2

		cl::Event e_grads_kernel;
		clContext.queue.enqueueNDRangeKernel(update_beta_calculate_grads_kernel, cl::NullRange,
				cl::NDRange(countPartialSums), cl::NDRange(1),
				NULL, &e_grads_kernel);

		double eps_t = a * std::pow(1.0 + step_count / b, -c);
		cl_double2 clEta;
		clEta.s[0] = this->eta[0];
		clEta.s[1] = this->eta[1];

		std::vector<std::vector<double> > noise = Random::random->randn(K, 2);	// random noise.
		std::vector<cl_double2> clNoise(noise.size());
		std::transform(noise.begin(), noise.end(), clNoise.begin(), [](const std::vector<double>& n){
			cl_double2 ret;
			ret.s[0] = n[0];
			ret.s[1] = n[1];
			return ret;
		});

		clContext.queue.enqueueWriteBuffer(clRandomNK, CL_TRUE,
				0, clNoise.size()*sizeof(cl_double2),
				clNoise.data());

		e_grads_kernel.wait();

		update_beta_calculate_theta_kernel.setArg(0, clTheta);
		update_beta_calculate_theta_kernel.setArg(1, clRandomNK);
		update_beta_calculate_theta_kernel.setArg(2, clScratch);
		update_beta_calculate_theta_kernel.setArg(3, (cl_double)scale);
		update_beta_calculate_theta_kernel.setArg(4, (cl_double)eps_t);
		update_beta_calculate_theta_kernel.setArg(5, clEta);
		update_beta_calculate_theta_kernel.setArg(6, (int)countPartialSums);

		clContext.queue.enqueueTask(update_beta_calculate_theta_kernel);
		clContext.queue.finish();


		// copy theta
		std::vector<cl_double2> vclTheta(theta.size());
		clContext.queue.enqueueReadBuffer(clTheta, CL_TRUE,
				0, theta.size() * sizeof(cl_double2),
				vclTheta.data());
		std::transform(vclTheta.begin(), vclTheta.end(), theta.begin(), [](const cl_double2 in){
			std::vector<double> ret(2);
			ret[0] = in.s[0];
			ret[1] = in.s[1];
			return ret;
		});

		std::vector<std::vector<double> > temp(theta.size(), std::vector<double>(theta[0].size()));
		np::row_normalize(&temp, theta);
		std::transform(temp.begin(), temp.end(), beta.begin(), np::SelectColumn<double>(1));
	}

	void sample_latent_vars2(const OrderedEdgeSet &mini_batch) {
		std::vector<cl_int2> edges(mini_batch.size());
		// Copy edges
		std::transform(mini_batch.begin(), mini_batch.end(), edges.begin(), [](const Edge& e) {
			cl_int2 E;
			E.s[0] = e.first;
			E.s[1] = e.second;
			return E;
		});
		clContext.queue.enqueueWriteBuffer(clEdges, CL_FALSE,
				0, edges.size()*sizeof(cl_int2),
				edges.data());

		sample_latent_vars2_kernel.setArg(0, clGraph);
		sample_latent_vars2_kernel.setArg(1, clEdges);
		sample_latent_vars2_kernel.setArg(2, (cl_int)edges.size());
		sample_latent_vars2_kernel.setArg(3, clPi);
		sample_latent_vars2_kernel.setArg(4, clBeta);
		sample_latent_vars2_kernel.setArg(5, clZ);
		sample_latent_vars2_kernel.setArg(6, clScratch);
		sample_latent_vars2_kernel.setArg(7, clRandomSeed);

		clContext.queue.finish(); // Wait for clEdges and PiUpdates from sample_latent_vars_and_update_pi

		clContext.queue.enqueueNDRangeKernel(sample_latent_vars2_kernel, cl::NullRange, cl::NDRange(PARALLELISM), cl::NDRange(1));
	}

	::size_t real_num_node_sample() const {
		return num_node_sample + 1;
	}

	void sample_latent_vars_and_update_pi(const OrderedVertexSet& nodes) {

		// Copy sampled node IDs
		std::vector<int> v_nodes(nodes.begin(), nodes.end()); // FIXME: replace OrderedVertexSet with vector
		clContext.queue.enqueueWriteBuffer(clNodes, CL_FALSE, 0, v_nodes.size()*sizeof(int), v_nodes.data());

		// Copy beta
		clContext.queue.enqueueWriteBuffer(clBeta, CL_FALSE, 0, K * sizeof(double), beta.data());

		// Randoms for update pi for each node
		std::unordered_map<int, std::vector<double>> noise;
		int i = 0;
		for (auto node = nodes.begin();
				node != nodes.end();
				++node, ++i) {
			noise[*node] = Random::random->randn(K);
			clContext.queue.enqueueWriteBuffer(clRandomNK, CL_FALSE,
					i * K * sizeof(cl_double),
					K * sizeof(cl_double),
					noise[*node].data());
		}

		int Idx = 0;
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clGraph);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clHeldOutGraph);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clNodes);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_int)nodes.size());
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clNodesNeighbors);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clPi);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clPiUpdate);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clPhi);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clBeta);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_double)epsilon);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clZ);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clRandomNK);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clScratch);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_double)alpha);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_double)a);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_double)b);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_double)c);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_int)step_count);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, (cl_int)N);
		sample_latent_vars_and_update_pi_kernel.setArg(Idx++, clRandomSeed);

		clContext.queue.finish();
		clContext.queue.enqueueNDRangeKernel(sample_latent_vars_and_update_pi_kernel, cl::NullRange, cl::NDRange(PARALLELISM), cl::NDRange(1));
		noise.clear();
		clContext.queue.finish();

		// read Pi again
		for (auto node = nodes.begin();
				node != nodes.end();
				++node) {
			clContext.queue.enqueueReadBuffer(clPiUpdate, CL_FALSE,
					*node * K * sizeof(cl_double),
					K * sizeof(cl_double),
					pi[*node].data());
			clContext.queue.enqueueCopyBuffer(clPiUpdate, clPi,
					*node * K * sizeof(cl_double),
					*node * K * sizeof(cl_double),
					K * sizeof(cl_double));
		}
	}


	void _prepare_flat_cl_graph(cl::Buffer &edges, cl::Buffer &nodes, cl::Buffer &graph, std::map<int, std::vector<int>> linkedMap) {
		const int h_edges_size = 2 * network.get_num_linked_edges();
		std::unique_ptr<cl_int[]> h_edges(new cl_int[h_edges_size]);
		const int h_nodes_size = network.get_num_nodes();
		std::unique_ptr<cl_int2[]> h_nodes(new cl_int2[h_nodes_size]);

		size_t offset = 0;
		for (cl_int i = 0; i < network.get_num_nodes(); ++i) {
			auto it = linkedMap.find(i);
			if (it == linkedMap.end()) {
				h_nodes.get()[i].s[0] = 0;
				h_nodes.get()[i].s[1] = 0;
			} else {
				std::sort(it->second.begin(), it->second.end());
				h_nodes.get()[i].s[0] = it->second.size();
				h_nodes.get()[i].s[1] = offset;
				for (auto viter = it->second.begin();
						viter != it->second.end(); ++viter) {
					h_edges.get()[offset] = *viter;
					++offset;
				}
			}
		}

		edges = cl::Buffer(clContext.context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, h_edges_size*sizeof(cl_int), h_edges.get());
		nodes = cl::Buffer(clContext.context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, h_nodes_size*sizeof(cl_int2), h_nodes.get());
		graph = cl::Buffer(clContext.context, CL_MEM_READ_WRITE, 2*64/8 /* 2 pointers, each is at most 64-bits */);

		graph_init_kernel.setArg(0, graph);
		graph_init_kernel.setArg(1, edges);
		graph_init_kernel.setArg(2, nodes);

		clContext.queue.enqueueTask(graph_init_kernel);
		clContext.queue.finish();
	}

	void init_graph() {
		graph_program = this->clContext.createProgram("OpenCL/graph.cl", progOpts);
		graph_init_kernel = cl::Kernel(graph_program, "graph_init");
		std::map<int, std::vector<int>> linkedMap;

		for (auto e : network.get_linked_edges()) {
			linkedMap[e.first].push_back(e.second);
			linkedMap[e.second].push_back(e.first);
		}
		_prepare_flat_cl_graph(clGraphEdges, clGraphNodes, clGraph, linkedMap);

		linkedMap.clear();

		for (auto e : network.get_held_out_set()) {
			linkedMap[e.first.first].push_back(e.first.second);
			linkedMap[e.first.second].push_back(e.first.first);
		}
		for (auto e : network.get_test_set()) {
			linkedMap[e.first.first].push_back(e.first.second);
			linkedMap[e.first.second].push_back(e.first.first);
		}
		_prepare_flat_cl_graph(clHeldOutGraphEdges, clHeldOutGraphNodes, clHeldOutGraph, linkedMap);


#if MCMC_CL_STOCHASTIC_TEST_GRAPH
		test_graph();
#endif
	}

#if MCMC_CL_STOCHASTIC_TEST_GRAPH // TEST GRAPH ON OPENCL's SIDE
	void test_graph() {
		cl::Program program2 = this->clContext.createProgram("OpenCL/test.cl", progOpts);
		cl::Kernel test_print_peers_of_kernel(program2, "test_print_peers_of");
		test_print_peers_of_kernel.setArg(0, clGraph);
		test_print_peers_of_kernel.setArg(1, 0);

		int n = 800;
		test_print_peers_of_kernel.setArg(1, n);

		std::cout << "LOOKING INTO NODE " << n << ", neighbors = " << linkedMap[n].size() << std::endl;
		for (auto p : linkedMap[n]) {
			std::cout << p << std::endl;
		}

		clContext.queue.enqueueTask(test_print_peers_of_kernel);
		clContext.queue.finish();
	}
#endif

	std::string progOpts;

	cl::ClContext clContext;

	cl::Program graph_program;
	cl::Program sampler_program;

	cl::Kernel graph_init_kernel;
	cl::Kernel sample_latent_vars_and_update_pi_kernel;
	cl::Kernel sample_latent_vars2_kernel;
	cl::Kernel update_beta_calculate_grads_kernel;
	cl::Kernel update_beta_calculate_theta_kernel;

	cl::Buffer clGraphEdges;
	cl::Buffer clGraphNodes;
	cl::Buffer clGraph;

	cl::Buffer clHeldOutGraphEdges;
	cl::Buffer clHeldOutGraphNodes;
	cl::Buffer clHeldOutGraph;

	cl::Buffer clNodes;
	cl::Buffer clNodesNeighbors;
	cl::Buffer clEdges;
	cl::Buffer clPi;
	cl::Buffer clPiUpdate;
	cl::Buffer clPhi;
	cl::Buffer clBeta;
	cl::Buffer clTheta;
	cl::Buffer clThetaSum;
	cl::Buffer clZ;
	cl::Buffer clRandomNK;
	cl::Buffer clScratch;
	cl::Buffer clRandomSeed;
};

}
}


#endif /* MCMC_CLSAMPLER_STOCHASTIC_H_ */
