#include "mcmc/options.h"
#include "mcmc/network.h"
// #include "mcmc/sampler.h"
#include "mcmc/preprocess/data_factory.h"
// #include "mcmc/learning/mcmc_sampler_stochastic.h"
#include "mcmc/learning/variational_inference_stochastic.h"
// #include "mcmc/learning/variational_inference_batch.h"
// #include "mcmc/learning/mcmc_sampler_batch.h"

using namespace mcmc;
using namespace mcmc::preprocess;
using namespace mcmc::learning;

int main(int argc, char *argv[]) {
	Options args(argc, argv);

	DataFactory df("netscience", args.filename);
	const Data *data = df.get_data();
	Network network(data, 0.1);

	// std::cout << "start MCMC batch" << std::endl;
	// MCMCSamplerBatch mcmcSampler(args, network);
	// mcmcSampler.run();

	std::cout << "start variational inference batch" << std::endl;
	SVI sviSampler(args, network);
	sviSampler.run();

	return 0;
}