import numpy as np
import pyopencl as cl

class ClSampler(object):
    
    def __init__(self, K, num_nodes, num_edges, link_map, batch_size, sample_size):
        sample_size += 1
        self.K = K
        self.batch_size = batch_size
        self.ctx = cl.create_some_context()
        self.max_nodes_in_batch = 2*batch_size
        print self.ctx.devices
        #self.ctx = cl.Context(dev_type=cl.device_type.GPU)
        self.queue = cl.CommandQueue(self.ctx)
        self._init_graph(num_nodes, num_edges, link_map)
        # create grid for all num_nodes (not batch_size) so mapping of nodeId works directly
        self.np_sample_neighbor_nodes = np.empty((num_nodes, sample_size), dtype=np.int32)
        self.np_Zs = np.empty((num_nodes, self.K), dtype=np.float64)
        #
        self.cl_sample_neighbor_nodes = cl.Buffer(self.ctx, cl.mem_flags.READ_WRITE, size=self.np_sample_neighbor_nodes.nbytes)
        self.cl_Zs = cl.Buffer(self.ctx, cl.mem_flags.READ_WRITE, size=self.np_Zs.nbytes);
        self.cl_nodes = cl.Buffer(self.ctx, cl.mem_flags.READ_ONLY, size=self.max_nodes_in_batch*4) # at most: 2 unique nodes per edge, 4-bytes each
        self.cl_pi = cl.Buffer(self.ctx, cl.mem_flags.READ_WRITE, size=num_nodes*self.K*8)# (N, K) double
        self.cl_beta = cl.Buffer(self.ctx, cl.mem_flags.READ_WRITE, size=self.K*8) # (K) double
        self.cl_p = cl.Buffer(self.ctx, cl.mem_flags.READ_WRITE, size=self.max_nodes_in_batch*self.K*8) # at most: 2 unique nodes per edge, K*8-bytes each
        self.cl_bounds = cl.Buffer(self.ctx, cl.mem_flags.READ_WRITE, size=self.max_nodes_in_batch*self.K*8)
        gopts = ['-IOpenCL/include']
        sampler_opts = gopts + ['-DK=%d' % self.K, '-DNEIGHBOR_SAMPLE_SIZE=%d' % (sample_size)]
        self.gprog = cl.Program(self.ctx, ''.join(open('OpenCL/graph.cl', 'r').readlines())).build(options=gopts)
        print 'DONE'
        self.prog = cl.Program(self.ctx, ''.join(open('OpenCL/sampler.cl', 'r').readlines())).build(options=sampler_opts)
        self.gprog.graph_init(self.queue, (1, 1), None, self.cl_graph, self.cl_edges, self.cl_node_edges)
        self.queue.finish()
        print self.prog.sample_latent_vars.get_work_group_info(cl.kernel_work_group_info.PRIVATE_MEM_SIZE, self.ctx.devices[0])
        print 'DONE'

    def _init_graph(self, num_nodes, num_edges, link_map):
        self.np_edges = np.empty(num_edges*2, dtype=np.int32)
        self.np_node_edges = np.zeros( (num_nodes, 2) , dtype=np.int32) # (num links, offset) per node
        offset = 0
        for n, links in link_map.items():
            self.np_node_edges[n] = [len(links), offset]
            for d in links:
                self.np_edges[offset] = d
                offset += 1
        # create CL buffers for graph
        self.cl_graph = cl.Buffer(self.ctx, cl.mem_flags.READ_WRITE, size=2*64/8) # 2 pointers, each is at most 64-bits        
        self.cl_edges = cl.Buffer(self.ctx, cl.mem_flags.READ_ONLY|cl.mem_flags.COPY_HOST_PTR, hostbuf=self.np_edges)
        self.cl_node_edges = cl.Buffer(self.ctx, cl.mem_flags.READ_ONLY|cl.mem_flags.COPY_HOST_PTR, hostbuf=self.np_node_edges)

    def update_node_neighbors(self, nodeId, neighbor_nodes):
        self.np_sample_neighbor_nodes[nodeId] = neighbor_nodes

    def sample_latent_vars(self, nodes, pi, beta, epsilon):
        g_items = len(nodes)
        l_items = 32
        if g_items % l_items:
            g_items += l_items - (g_items % l_items)
        print g_items, l_items
        np_nodes = np.array(nodes, dtype=np.int32)
        cl.enqueue_copy(self.queue, self.cl_nodes, np_nodes)
        cl.enqueue_copy(self.queue, self.cl_pi, pi)
        cl.enqueue_copy(self.queue, self.cl_beta, beta.copy())
        self.prog.sample_latent_vars(self.queue, (g_items, 1, 1), (l_items, 1, 1),
                                    self.cl_graph,
                                    self.cl_nodes,
                                    np.int32(len(nodes)),
                                    self.cl_sample_neighbor_nodes,
                                    self.cl_pi,
                                    self.cl_beta,
                                    np.float64(epsilon),
                                    self.cl_Zs,
                                    self.cl_p,
                                    self.cl_bounds)
        self.queue.finish()
        cl.enqueue_copy(self.queue, self.np_Zs, self.cl_Zs)
        return self.np_Zs.copy()
