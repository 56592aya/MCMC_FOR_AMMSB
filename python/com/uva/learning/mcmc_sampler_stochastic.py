import sys

from com.uva.timer import Timer
from com.uva.learning.learner import Learner
from sets import Set
import math
from com.uva.source_aware_random import SourceAwareRandom as random
import numpy as np
import copy
# from com.uva.sample_latent_vars import sample_z_ab_from_edge
import cProfile, pstats, StringIO


class MCMCSamplerStochastic(Learner):
    '''
    Mini-batch based MCMC sampler for community overlapping problems. Basically, given a 
    connected graph where each node connects to other nodes, we try to find out the 
    community information for each node. 
    
    Formally, each node can be belong to multiple communities which we can represent it by 
    distribution of communities. For instance, if we assume there are total K communities
    in the graph, then each node a, is attached to community distribution \pi_{a}, where
    \pi{a} is K dimensional vector, and \pi_{ai} represents the probability that node a 
    belongs to community i, and \pi_{a0} + \pi_{a1} +... +\pi_{aK} = 1
    
    Also, there is another parameters called \beta representing the community strength, where 
    \beta_{k} is scalar. 
    
    In summary, the model has the parameters:
    Prior: \alpha, \eta
    Parameters: \pi, \beta
    Latent variables: z_ab, z_ba
    Observations: y_ab for every link. 
    
    And our goal is to estimate the posterior given observations and priors:
    p(\pi,\beta | \alpha,\eta, y). 
    
    Because of the intractability, we use MCMC(unbiased) to do approximate inference. But 
    different from classical MCMC approach, where we use ALL the examples to update the 
    parameters for each iteration, here we only use mini-batch (subset) of the examples.
    This method is great marriage between MCMC and stochastic methods.  
    '''
    def __init__(self, args, graph, compatibility_mode):
        # call base class initialization
        Learner.__init__(self, args, graph, compatibility_mode)

        self._strategy = args.strategy

        self._interval = args.interval
        self.__num_pieces = graph.get_num_pieces()
        
        # step size parameters. 
        # FIXME RFHH make SURE self.__b is initialized to a float. As published,
        # it is an int = 1024, which results in integer step_count / b so
        # eps_t always equals self.__a.
        self.__b = 1.0 * args.b
        self.__c = 1.0 * args.c
        if (args.a == 0.0):
            self.__a = pow(self.__b, -self.__c)
        else:
            self.__a = 1.0 * args.a
        
        # control parameters for learning
        #self.__num_node_sample = int(math.sqrt(self._network.get_num_nodes())) 
        
        # TODO: automative update.....
        # self.__num_node_sample = int(self._N/50)
        self.__num_node_sample = args.num_node_sample
        # model parameters and re-parameterization
        # since the model parameter - \pi and \beta should stay in the simplex, 
        # we need to restrict the sum of probability equals to 1.  The way we
        # restrict this is using re-reparameterization techniques, where we 
        # introduce another set of variables, and update them first followed by 
        # updating \pi and \beta.  
        # self.__theta = random.gamma(100,0.01,(self._K, 2))      # parameterization for \beta
        self.__theta = random.get("theta init").gamma(self._eta[0], self._eta[1], (self._K, 2))      # parameterization for \beta
        self.__phi = random.get("phi init").gamma(1,1,(self._N, self._K))       # parameterization for \pi
        
        # temp = self.__theta/np.sum(self.__theta,1)[:,np.newaxis]
        # self._beta = temp[:,1]
        # self._pi = self.__phi/np.sum(self.__phi,1)[:,np.newaxis]
        self.update_pi_from_phi()
        self.update_beta_from_theta()


    def update_pi_from_phi(self):
        sys.stdout.write("len(phi) %d len(phi[0]) %d\n" % (len(self.__phi), len(self.__phi[0])))
        for p in self.__phi:
            if len(p) != len(self.__phi[0]):
                sys.stderr.write("Havoc, phi length is %d, should be %d\n" % (len(p), len(self.__phi[0])))

        self._pi = self.__phi/np.sum(self.__phi,1)[:,np.newaxis]

    def update_beta_from_theta(self):
        temp = self.__theta/np.sum(self.__theta,1)[:,np.newaxis]
        self._beta = temp[:,1]

    def run(self):
        """ run mini-batch based MCMC sampler, based on the sungjin's note """

        sys.stdout.write("sample strategy %s\n" % self._strategy)
        sys.stdout.write("beta[0] %.12f\n" % self._beta[0])
        sys.stdout.write("phi[0][0] %.12f\n" % self.__phi[0][0])
        sys.stdout.write("pi[0][0] %.12f\n" % self._pi[0][0])
        sys.stdout.write("a %g b %g c %g\n" % (self.__a, self.__b, self.__c))
        sys.stdout.write("minibatch size %d num_node_sample %d\n" % ((self._network.get_num_nodes() / self.__num_pieces), self.__num_node_sample))

        t_outer = Timer("outer");
        t_sample_minibatch = Timer("sample minibatch")
        t_sample_neighbors = Timer("sample neighbors")
        t_update_phi = Timer("update phi");
        t_update_pi = Timer("update pi");
        t_update_beta = Timer("update beta");
        t_perp  = Timer("perplexity");

        start = t_outer.now()

        t_outer.start();

        while self._step_count < self._max_iteration and not self._is_converged():

            if (self._step_count - 1) % self._interval == 0:
                t_perp.start()
                ppx_score = self._cal_perplexity_held_out()
                t_perp.stop()
                sys.stdout.write("step count: %d time: %.3f perplexity for hold out set is: %.12f\n" % (self._step_count, t_outer.now() - start, ppx_score))
                self._ppxs_held_out.append(ppx_score)
           
            (mini_batch, scale) = self._network.sample_mini_batch(self.__num_pieces, self._strategy)
            # latent_vars = {}
            # size = {}
            
            t_sample_minibatch.start()
            nodes_in_mini_batch = list(self.__nodes_in_batch(mini_batch))
            t_sample_minibatch.stop()
            if self._compatibility_mode:  # to be able to replay from C++
                nodes_in_mini_batch = sorted(nodes_in_mini_batch)
            # print "minibatch(" + str(len(mini_batch)) + " " + str(sorted(mini_batch))

            # iterate through each node in the mini batch. 
            for node in nodes_in_mini_batch:
                # sample a mini-batch of neighbors
                t_sample_neighbors.start()
                neighbors = self.__sample_neighbor_nodes(self.__num_node_sample, node)
                t_sample_neighbors.stop()
                t_update_phi.start()
                self.__update_phi(node, neighbors)
                t_update_phi.stop()

            t_update_pi.start()
            self.update_pi_from_phi()
            t_update_pi.stop()
            # update beta
            if self._compatibility_mode:  # to be able to replay from C++
                mini_batch = sorted(mini_batch)
            t_update_beta.start()
            self.__update_beta(mini_batch, scale)
            t_update_beta.stop()
                            
            self._step_count += 1
            
            """
            pr.disable()
            s = StringIO.StringIO()
            sortby = 'cumulative'
            ps = pstats.Stats(pr, stream=s).sort_stats(sortby)
            ps.print_stats()
            print s.getvalue()
            """

        t_outer.stop();

        t_outer.print_header()
        t_outer.report()
        t_sample_minibatch.report()
        t_sample_neighbors.report()
        t_update_phi.report()
        t_update_pi.report()
        t_update_beta.report()
        t_perp.report()


    def __update_beta(self, mini_batch, scale):
        '''
        update beta for mini_batch. 
        '''
            
        grads = np.zeros((self._K, 2))                               # gradients K*2 dimension
        theta_sum = np.sum(self.__theta,1)                                 

        eps_t  = self.__a*((1 + self._step_count/self.__b)**-self.__c)
        
        for  edge in mini_batch:
            y = 0
            if edge in self._network.get_linked_edges():
                y = 1

            i, j = edge
            probs = np.zeros(self._K)
            pi_sum = 0.0
            for k in range(0,self._K):
                pi_sum += self._pi[i][k] * self._pi[j][k]
                probs[k] = self._beta[k] ** y * (1 - self._beta[k]) ** (1 - y) * self._pi[i][k] * self._pi[j][k]

            prob_0 = self._epsilon ** y * (1 - self._epsilon) ** (1 - y) * (1 - pi_sum)
            prob_sum = np.sum(probs) + prob_0
            for k in range(0,self._K):
                grads[k][0] += (probs[k] / prob_sum) * (abs(1-y)/self.__theta[k][0] - 1/ theta_sum[k])
                grads[k][1] += (probs[k] / prob_sum) * (abs(-y)/self.__theta[k][1] - 1/theta_sum[k])
        
        # update theta 
        noise = random.get("beta update").randn(self._K, 2)                          # random noise. 

        for k in range(0,self._K):
            for i in range(0,2):
                self.__theta[k][i] = abs(self.__theta[k][i] + eps_t / 2.0 * (self._eta[i] - self.__theta[k][i] + \
                                    scale * grads[k][i]) + eps_t**.5*self.__theta[k][i] ** .5 * noise[k][i])  
        self.update_beta_from_theta()

    def __update_phi(self, i, neighbors):
        '''
        update phi for current node i. 
        ''' 
        eps_t  = self.__a*((1 + self._step_count/self.__b)**-self.__c)
        phi_i_sum = np.sum(self.__phi[i])
    
        phi_star = copy.copy(self.__phi[i])                              # updated \phi
        phi_i_sum = np.sum(self.__phi[i])                                   
        grads = np.zeros(self._K)
        noise = random.get("phi update").randn(self._K)                                 # random noise. 

        for neighbor in neighbors:
            if neighbor == i:
                continue

            y_ab = 0            # observation
            edge = (min(i, neighbor), max(i, neighbor))
            if edge in self._network.get_linked_edges():
                y_ab = 1

            probs = np.empty(self._K)
            for k in range(0,self._K):
                probs[k] = self._beta[k] ** y_ab * (1 - self._beta[k]) ** (1 - y_ab) * self._pi[i][k] * self._pi[neighbor][k]
                probs[k] += self._epsilon ** y_ab * (1 - self._epsilon) ** (1 - y_ab) * self._pi[i][k] * (1 - self._pi[neighbor][k])

            prob_sum = np.sum(probs)
            for k in range(0,self._K):
                grads[k] += (probs[k] / prob_sum) / self.__phi[i][k] - 1.0 / phi_i_sum
        
        # update phi for node i
        for k in range(0, self._K):
            self.__phi[i][k] = abs(self.__phi[i][k] + eps_t / 2 * (self._alpha - self.__phi[i][k] + (self._N*1.0 / self.__num_node_sample) *grads[k]) + eps_t ** 0.5 * self.__phi[i][k] ** 0.5 *noise[k])
    
        
    def __sample_neighbor_nodes(self, sample_size, nodeId):
        '''
        Sample subset of neighborhood nodes. 
        '''    
        p = sample_size
        neighbor_nodes = Set()
        held_out_set = self._network.get_held_out_set()
        test_set = self._network.get_test_set()
        
        while p > 0:
            nodeList = random.sample_range("neighbor sampler", self._N, sample_size * 2)
            if self._compatibility_mode:  # to be able to replay from C++
                nodeList = sorted(nodeList)
            for neighborId in nodeList:
                    if p < 0:
                        if False:
                            print sys._getframe().f_code.co_name + ": Are you sure p < 0 is a good idea?"
                        break
                    if neighborId == nodeId:
                        continue
                    # check condition, and insert into mini_batch_set if it is valid. 
                    edge = (min(nodeId, neighborId), max(nodeId, neighborId))
                    if edge in held_out_set or edge in test_set or neighborId in neighbor_nodes:
                        continue
                    else:
                        # add it into mini_batch_set
                        neighbor_nodes.add(neighborId)
                        p -= 1
                        
        if self._compatibility_mode:  # to be able to replay from C++
            neighbor_nodes = sorted(neighbor_nodes)
        return neighbor_nodes

    def __nodes_in_batch(self, mini_batch):
        """
        Get all the unique nodes in the mini_batch. 
        """
        node_set = Set()
        for edge in mini_batch:
            node_set.add(edge[0])
            node_set.add(edge[1])
        return node_set
    
    def _save(self):
        f = open('ppx_mcmc.txt', 'wb')
        for i in range(0, len(self._avg_log)):
            f.write(str(math.exp(self._avg_log[i])) + "\t" + str(self._timing[i]) +"\n")
        f.close()
        
    def set_num_node_sample(self, num_node_sample):
         self.__num_node_sample = num_node_sample
    
    """
    def sample_z_ab_from_edge(self, y, pi_a, pi_b, beta, epsilon, K):
        p = np.zeros(K)
   
        tmp = 0.0
    
        for i in range(0, K):
            tmp = beta[i]**y*(1-beta[i])**(1-y)*pi_a[i]*pi_b[i]
            tmp += epsilon**y*(1-epsilon)**(1-y)*pi_a[i]*(1-pi_b[i])
            p[i] = tmp
    
    
        for k in range(1,K):
            p[k] += p[k-1]
    
        r = random.random()
        location = r * p[K-1]
        # get the index of bounds that containing location. 
        for i in range(0, K):
            if location <= p[i]:
                return i
    
        # failed, should not happen!
        return -1    
    """
