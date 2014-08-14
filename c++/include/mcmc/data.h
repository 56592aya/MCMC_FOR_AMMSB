/*
 * Copyright notice goes here
 */

/*
 * @author Rutger Hofman, VU Amsterdam
 * @author Wenzhe Li
 *
 * @date 2014-08-6
 */

#ifndef MCMC_DATA_H__
#define MCMC_DATA_H__

#include <unordered_set>
#include <utility>

namespace mcmc {

typedef typename std::pair<int, int> Edge;

typedef typename std::unordered_set<Edge> EdgeSet;

typedef std::map<Edge, bool>			EdgeMap;


/**
 * Data class is an abstraction for the raw data, including vertices and edges.
 * It's possible that the class can contain some pre-processing functions to clean 
 * or re-structure the data.   
 *             
 * The data can be absorbed directly by sampler.
 */
class Data {
public:
	Data(const void *V, const EdgeSet *E, int N) {
		this->V = V;
		this->E = E;
		this->N = N;
	}

	virtual ~Data() {
		// delete const_cast<void *>(V);
		delete const_cast<EdgeSet *>(E);
	}

public:
	const void *V;	// mapping between vertices and attributes.
	const EdgeSet *E;	// all pair of "linked" edges.
	int N;				// number of vertices
};

}	// namespace mcmc

#endif	// ndef MCMC_DATA_H__
