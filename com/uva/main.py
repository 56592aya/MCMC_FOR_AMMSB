import argparse
from com.uva.network import Network
from com.uva.preprocess.data_factory import DataFactory
from com.uva.learning.mcmc_sampler_stochastic import MCMCSamplerStochastic
from com.uva.learning.variational_inference_stochastic import SVI
import threading
import matplotlib.pyplot as plt

def work (sampler): 
  threading.Timer(5, work, [sampler]).start (); 
  print "held out perplexity: " + str(sampler._cal_perplexity_held_out())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('alpha', type=float, default=0.01)
    parser.add_argument('eta0', type=float, default=1)
    parser.add_argument('eta1', type=float, default=1)
    parser.add_argument('K', type=int, default=100)  
    parser.add_argument('mini_batch_size', type=int, default=500)   # mini-batch size
    parser.add_argument('epsilon', type=float, default=0.05)
    parser.add_argument('max_iteration', type=int, default=10000000)
    
    # parameters for step size
    parser.add_argument('a', type=float, default=0.01)
    parser.add_argument('b', type=float, default=1024)
    parser.add_argument('c', type=float, default=0.55)
    
    parser.add_argument('num_updates', type=int, default=1000)
    parser.add_argument('hold_out_prob', type=float, default=0.1)
    parser.add_argument('output_dir', type=str,default='.')
    args = parser.parse_args()
    
    data = DataFactory.get_data("netscience")
    network = Network(data, 0.1)
    sampler  = MCMCSamplerStochastic(args, network)
    work(sampler)
    sampler.run1()



if __name__ == '__main__':
    main()