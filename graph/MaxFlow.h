#ifndef MAX_FLOW_H
#define MAX_FLOW_H


#include <iostream>
#include <climits>
#include "mtl/Vec.h"
using namespace std;
using namespace Minisat;

class MaxFlow{
public:


	virtual ~MaxFlow(){};

    virtual void setCapacity(int u, int w, int c)=0;

    virtual void setAllEdgeCapacities(int c)=0;
    virtual int maxFlow(int s, int t)=0;


    struct Edge {
    	int u;
    	int v;
    };
    virtual int minCut(int s, int t, vec<Edge> & cut)=0;


};
#endif
