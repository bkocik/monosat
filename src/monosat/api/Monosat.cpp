/**************************************************************************************************
 The MIT License (MIT)

 Copyright (c) 2016, Sam Bayless

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
 OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 **************************************************************************************************/

#include "monosat/utils/ParseUtils.h"
#include "monosat/utils/Options.h"
#include "monosat/core/Solver.h"
#include "monosat/simp/SimpSolver.h"
#include "monosat/graph/GraphTheory.h"
#include "monosat/geometry/GeometryTheory.h"
#include "monosat/fsm/FSMTheory.h"
#include "monosat/pb/PbTheory.h"
#include "monosat/amo/AMOTheory.h"
#include "Monosat.h"
#include "monosat/core/Dimacs.h"
#include "monosat/bv/BVParser.h"
#include "monosat/graph/GraphParser.h"
#include "monosat/pb/PbParser.h"
#include "monosat/amo/AMOParser.h"
#include "monosat/core/Optimize.h"
#include "monosat/pb/PbSolver.h"
#include "monosat/routing/FlowRouter.h"
#include "monosat/Version.h"
#include "MonosatInternal.h"
#include <csignal>
#include <set>
#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <cstdint>

using namespace Monosat;
using namespace std;

#ifdef __APPLE__
using sighandler_t = sig_t;

#endif

//Supporting function for throwing parse errors
inline void api_errorf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char buf[1000];
	vsnprintf(buf, sizeof buf,fmt, args);
	va_end(args);
	fprintf(stderr,"%s", buf);
	fflush(stderr);
	throw std::runtime_error(buf);

}

namespace APISignal{
//Global time and memory limits shared among all solvers
static  int64_t time_limit=-1;
static int64_t memory_limit=-1;

static bool has_system_time_limit=false;
static rlim_t system_time_limit;
static bool has_system_mem_limit=false;
static rlim_t system_mem_limit;

static std::set<Solver*> solvers;

static sighandler_t system_sigxcpu_handler = nullptr;

//static initializer, following http://stackoverflow.com/a/1681655
namespace {
struct initializer {
	initializer() {
		system_sigxcpu_handler=nullptr;
		time_limit=-1;
		memory_limit=-1;
		has_system_time_limit=false;
		has_system_mem_limit=false;
	}

	~initializer() {
		solvers.clear();
	}
};
static initializer i;
}
void disableResourceLimits();
static void SIGNAL_HANDLER_api(int signum) {
	disableResourceLimits();
	printf("Interupting solver due to resource limit\n");
	fflush(stdout);
	for(Solver* solver:solvers)
		solver->interrupt();

}


void enableResourceLimits(){
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	time_t cur_time = ru.ru_utime.tv_sec;

	rlimit rl;
	getrlimit(RLIMIT_CPU, &rl);
	if(!has_system_time_limit){
		has_system_time_limit=true;
		system_time_limit=rl.rlim_cur;
	}
	if (time_limit < INT32_MAX && time_limit>=0) {
		assert(cur_time>=0);
		int64_t local_time_limit = 	 time_limit+cur_time;//make this a relative time limit
		if(opt_verb>1){
			printf("Limiting cpu time to %" PRId64 "\n",local_time_limit);
		}
		if (rl.rlim_max == RLIM_INFINITY || (rlim_t) local_time_limit < rl.rlim_max) {
			rl.rlim_cur = local_time_limit;
			if (setrlimit(RLIMIT_CPU, &rl) == -1)
				api_errorf("WARNING! Could not set resource limit: CPU-time.\n");
		}
	}else{
		rl.rlim_cur = rl.rlim_max;
		if (setrlimit(RLIMIT_CPU, &rl) == -1)
			api_errorf("WARNING! Could not set resource limit: CPU-time.\n");
	}

	getrlimit(RLIMIT_AS, &rl);
	if(!has_system_mem_limit){
		has_system_mem_limit=true;
		system_mem_limit=rl.rlim_cur;
	}
	// Set limit on virtual memory:
	if (memory_limit < INT32_MAX && memory_limit>=0) {
		rlim_t new_mem_lim = (rlim_t) memory_limit * 1024 * 1024; //Is this safe?
		if(opt_verb>1){
			printf("Limiting virtual memory to %" PRId64 "\n",new_mem_lim);
		}
		if (rl.rlim_max == RLIM_INFINITY || new_mem_lim < rl.rlim_max) {
			rl.rlim_cur = new_mem_lim;
			if (setrlimit(RLIMIT_AS, &rl) == -1)
				fprintf(stderr, "WARNING! Could not set resource limit: Virtual memory.\n");
		}else{
			rl.rlim_cur = rl.rlim_max;
			if (setrlimit(RLIMIT_AS, &rl) == -1)
				fprintf(stderr, "WARNING! Could not set resource limit: Virtual memory.\n");
		}
	}
	sighandler_t old_sigxcpu = signal(SIGXCPU, SIGNAL_HANDLER_api);
	if(old_sigxcpu != SIGNAL_HANDLER_api){
		system_sigxcpu_handler = old_sigxcpu;//store this value for later
	}
}

void disableResourceLimits(){
	rlimit rl;
	getrlimit(RLIMIT_CPU, &rl);
	if(has_system_time_limit){
		has_system_time_limit=false;
		if (rl.rlim_max == RLIM_INFINITY || (rlim_t)system_time_limit < rl.rlim_max) {
			rl.rlim_cur = system_time_limit;
			if (setrlimit(RLIMIT_CPU, &rl) == -1)
				api_errorf("WARNING! Could not set resource limit: CPU-time.\n");
		}else{
			rl.rlim_cur = rl.rlim_max;
			if (setrlimit(RLIMIT_CPU, &rl) == -1)
				api_errorf("WARNING! Could not set resource limit: CPU-time.\n");
		}
	}
	getrlimit(RLIMIT_AS, &rl);
	if(has_system_mem_limit){
		has_system_mem_limit=false;
		if (rl.rlim_max == RLIM_INFINITY || system_mem_limit < rl.rlim_max) {
			rl.rlim_cur = system_mem_limit;
			if (setrlimit(RLIMIT_AS, &rl) == -1)
				fprintf(stderr, "WARNING! Could not set resource limit: Virtual memory.\n");
		}else{
			rl.rlim_cur = rl.rlim_max;
			if (setrlimit(RLIMIT_AS, &rl) == -1)
				fprintf(stderr, "WARNING! Could not set resource limit: Virtual memory.\n");
		}
	}
	if (system_sigxcpu_handler){
		signal(SIGXCPU, system_sigxcpu_handler);
		system_sigxcpu_handler=nullptr;
	}
}

}

//Select which algorithms to apply for graph and geometric theory solvers, by parsing command line arguments and defaults.
void _selectAlgorithms(){
	mincutalg = MinCutAlg::ALG_EDMONSKARP;

	if (!strcasecmp(opt_maxflow_alg, "edmondskarp-adj")) {
		mincutalg = MinCutAlg::ALG_EDKARP_ADJ;
	} else if (!strcasecmp(opt_maxflow_alg, "edmondskarp")) {
		mincutalg = MinCutAlg::ALG_EDMONSKARP;
	} else if (!strcasecmp(opt_maxflow_alg, "edmondskarp-dynamic")) {
		mincutalg = MinCutAlg::ALG_EDKARP_DYN;
	} else if (!strcasecmp(opt_maxflow_alg, "dinics")) {
		//Dinitz is also commonly spelled 'dinics' or 'Dinits', so accept those too...
		mincutalg = MinCutAlg::ALG_DINITZ;
	} else if (!strcasecmp(opt_maxflow_alg, "dinics-linkcut")) {
		mincutalg = MinCutAlg::ALG_DINITZ_LINKCUT;
	} else if (!strcasecmp(opt_maxflow_alg, "dinitz")) {
		mincutalg = MinCutAlg::ALG_DINITZ;
	} else if (!strcasecmp(opt_maxflow_alg, "dinitz-linkcut")) {
		mincutalg = MinCutAlg::ALG_DINITZ_LINKCUT;
	} else if (!strcasecmp(opt_maxflow_alg, "dinits")) {
		//Dinitz is also commonly spelled 'dinics' or 'Dinits', so accept those too...
		mincutalg = MinCutAlg::ALG_DINITZ;
	} else if (!strcasecmp(opt_maxflow_alg, "dinits-linkcut")) {
		mincutalg = MinCutAlg::ALG_DINITZ_LINKCUT;
	} else if (!strcasecmp(opt_maxflow_alg, "kohli-torr")) {
		mincutalg = MinCutAlg::ALG_KOHLI_TORR;
	} else {
		api_errorf( "Error: unknown max-flow/min-cut algorithm %s, aborting\n",
					((string) opt_maxflow_alg).c_str());

	}

	componentsalg = ComponentsAlg::ALG_DISJOINT_SETS;

	if (!strcasecmp(opt_components_alg, "disjoint-sets")) {
		componentsalg = ComponentsAlg::ALG_DISJOINT_SETS;
	} else {
		api_errorf(  "Error: unknown connectivity algorithm %s, aborting\n",
					 ((string) opt_components_alg).c_str());

	}

	cyclealg = CycleAlg::ALG_DFS_CYCLE;

	if (!strcasecmp(opt_cycle_alg, "dfs")) {
		cyclealg = CycleAlg::ALG_DFS_CYCLE;
	}else if (!strcasecmp(opt_cycle_alg, "pk")) {
		cyclealg = CycleAlg::ALG_PK_CYCLE;
	} else {
		api_errorf( "Error: unknown cycle detection algorithm %s, aborting\n",
					((string) opt_cycle_alg).c_str());

	}


	mstalg = MinSpanAlg::ALG_KRUSKAL;

	if (!strcasecmp(opt_mst_alg, "kruskal")) {
		mstalg = MinSpanAlg::ALG_KRUSKAL;
	} else if (!strcasecmp(opt_mst_alg, "prim")) {
		mstalg = MinSpanAlg::ALG_PRIM;
	} else if (!strcasecmp(opt_mst_alg, "spira-pan")) {
		mstalg = MinSpanAlg::ALG_SPIRA_PAN;
	} else {
		api_errorf( "Error: unknown minimum spanning tree algorithm %s, aborting\n",
					((string) opt_mst_alg).c_str());

	}

	reachalg = ReachAlg::ALG_BFS;

	if (!strcasecmp(opt_reach_alg, "dijkstra")) {
		reachalg = ReachAlg::ALG_DIJKSTRA;

	} else if (!strcasecmp(opt_reach_alg, "bfs")) {
		reachalg = ReachAlg::ALG_BFS;

	} else if (!strcasecmp(opt_reach_alg, "dfs")) {
		reachalg = ReachAlg::ALG_DFS;
	} else if (!strcasecmp(opt_reach_alg, "cnf")) {
		reachalg = ReachAlg::ALG_SAT;
	} else if (!strcasecmp(opt_reach_alg, "ramal-reps")) {
		reachalg = ReachAlg::ALG_RAMAL_REPS;
	} else if (!strcasecmp(opt_reach_alg, "ramal-reps-batch")) {
		reachalg = ReachAlg::ALG_RAMAL_REPS_BATCHED;
	} else if (!strcasecmp(opt_reach_alg, "ramal-reps-batch2")) {
        reachalg = ReachAlg::ALG_RAMAL_REPS_BATCHED2;
    } else {
		api_errorf( "Error: unknown reachability algorithm %s, aborting\n", ((string) opt_reach_alg).c_str());

	}

	distalg = DistAlg::ALG_DISTANCE;

	if (!strcasecmp(opt_dist_alg, "dijkstra")) {
		distalg = DistAlg::ALG_DIJKSTRA;

	} else if (!strcasecmp(opt_dist_alg, "bfs")) {
		distalg = DistAlg::ALG_DISTANCE;

	} else if (!strcasecmp(opt_dist_alg, "cnf")) {
		distalg = DistAlg::ALG_SAT;
	} else if (!strcasecmp(opt_dist_alg, "ramal-reps")) {
		distalg = DistAlg::ALG_RAMAL_REPS;
	}else if (!strcasecmp(opt_dist_alg, "ramal-reps-batch")) {
		distalg = DistAlg::ALG_RAMAL_REPS_BATCHED;
	}else if (!strcasecmp(opt_dist_alg, "ramal-reps-batch2")) {
		distalg = DistAlg::ALG_RAMAL_REPS_BATCHED2;
	}   else {
		api_errorf(  "Error: unknown distance algorithm %s, aborting\n", ((string) opt_dist_alg).c_str());

	}

	undirectedalg = ConnectivityAlg::ALG_BFS;

	if (!strcasecmp(opt_con_alg, "dijkstra")) {
		undirectedalg = ConnectivityAlg::ALG_DIJKSTRA;

	} else if (!strcasecmp(opt_con_alg, "bfs")) {
		undirectedalg = ConnectivityAlg::ALG_BFS;

	} else if (!strcasecmp(opt_con_alg, "dfs")) {
		undirectedalg = ConnectivityAlg::ALG_DFS;
	} else if (!strcasecmp(opt_con_alg, "cnf")) {
		undirectedalg = ConnectivityAlg::ALG_SAT;
	} else if (!strcasecmp(opt_con_alg, "thorup")) {
		undirectedalg = ConnectivityAlg::ALG_THORUP;
	}/*else if (!strcasecmp(opt_con_alg,"ramal-reps")){
	 undirectedalg = ConnectivityAlg::ALG_RAMAL_REPS;
	 } */else {
		api_errorf(  "Error: unknown undirected reachability algorithm %s, aborting\n",
					 ((string) opt_reach_alg).c_str());

	}

	allpairsalg = AllPairsAlg::ALG_DIJKSTRA_ALLPAIRS;

	if (!strcasecmp(opt_allpairs_alg, "floyd-warshall")) {
		allpairsalg = AllPairsAlg::ALG_FLOYDWARSHALL;
	} else if (!strcasecmp(opt_allpairs_alg, "dijkstra")) {
		allpairsalg = AllPairsAlg::ALG_DIJKSTRA_ALLPAIRS;

	} else {
		api_errorf(  "Error: unknown allpairs reachability algorithm %s, aborting\n",
					 ((string) opt_allpairs_alg).c_str());

	}

	undirected_allpairsalg = AllPairsConnectivityAlg::ALG_DIJKSTRA_ALLPAIRS;

	if (!strcasecmp(opt_undir_allpairs_alg, "floyd-warshall")) {
		undirected_allpairsalg = AllPairsConnectivityAlg::ALG_FLOYDWARSHALL;
	} else if (!strcasecmp(opt_undir_allpairs_alg, "dijkstra")) {
		undirected_allpairsalg = AllPairsConnectivityAlg::ALG_DIJKSTRA_ALLPAIRS;
	} else if (!strcasecmp(opt_undir_allpairs_alg, "thorup")) {
		undirected_allpairsalg = AllPairsConnectivityAlg::ALG_THORUP;
	} else {
		api_errorf(  "Error: unknown undirected allpairs reachability algorithm %s, aborting\n",
					 ((string) opt_allpairs_alg).c_str());

	}
}
void printStats(SimpSolver* solver) {
	double cpu_time = cpuTime();
	double mem_used = memUsedPeak(); // not available in osx

	solver->printStats(3);
	if (mem_used != 0)
		printf("Memory used           : %.2f MB\n", mem_used);
	printf("CPU time              : %g s\n", cpu_time);
}

inline void write_out(Monosat::SimpSolver * S, const char *fmt, ...) {
	MonosatData * d = (MonosatData*) S->_external_data;
	if (!d || !d->outfile){
		return;
	}

	va_list args;
	va_start(args, fmt);
	if( vfprintf(d->outfile,fmt,args)<0){
		api_errorf("Failed to write output");
	}
	va_end(args);
	fflush(d->outfile);
}
int varToLit(int variable, bool negated){
	return toInt(mkLit(variable,negated));
}
//Convert an external integer representation of a lit,
//into an internal solver literal
Lit internalLit(Monosat::SimpSolver * S, int l){
	return S->mapLit(toLit(l));
}
//Convert an internal solver lit into an external integer representation
int externalLit(Monosat::SimpSolver * S, Lit l){
	return toInt(S->unmap(l));
}

Var internalVar(Monosat::SimpSolver * S, int v){
    return S->mapVar(v);
}
//Convert an internal solver lit into an external integer representation
Var externalVar(Monosat::SimpSolver * S, Var v){
    return S->unmap(v);
}


//Convert an external integer representation of a lit into dimacs format
inline int dimacs(int externalLit) {
	Lit l = toLit(externalLit);
	return sign(l) ? -(var(l) + 1) : (var(l) + 1);
}

inline int dimacs(Monosat::SimpSolver * S, Lit internalLit) {
	Lit l = S->unmap(internalLit);
	return sign(l) ? -(var(l) + 1) : (var(l) + 1);
}

Var internalBV(Monosat::SimpSolver * S, int bvID){
	Monosat::BVTheorySolver<int64_t> *  bv = (Monosat::BVTheorySolver<int64_t> *) S->getBVTheory();
	return bv->mapBV(bvID);
}
Var internalBV(Monosat::BVTheorySolver<int64_t> *  bv, int bvID){
	return bv->mapBV(bvID);
}
//Convert an internal solver lit into an external integer representation
Var externalBV(Monosat::BVTheorySolver<int64_t> *  bv, int bvID){
	return bv->unmapBV(bvID);
}

void setOutputFile(Monosat::SimpSolver * S,const  char * output){
	MonosatData * d = (MonosatData*) S->_external_data;
	assert(d);
	if(d->outfile){
		fclose(d->outfile);
		d->outfile=nullptr;
	}
	if (output && strlen(output)>0) {
		d->outfile = fopen(output, "w");
	}
	write_out(S,"c monosat %s\n",d->args.c_str());
	if(S->const_true!=lit_Undef){
		write_out(S,"%d 0\n",dimacs(S,S->True()));
	}
	d->circuit.setOutputFile(d->outfile);
}

const char * getVersion(){
	return MONOSAT_VERSION_STR;
}

Monosat::SimpSolver * newSolver(){
	return newSolver_arg(nullptr);
}


//adapted from stack overflow, http://stackoverflow.com/a/236803
std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, delim)) {
		elems.push_back(item);
	}
	return elems;
}

std::vector<char*> &split(const std::string &s, char delim, std::vector<char*> &elems) {
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, delim)) {
		const char * arg = item.c_str();
		char * arg_cpy = new char[item.size()+1];
		std::strcpy(arg_cpy,arg);
		elems.push_back(arg_cpy);
	}
	return elems;
}

std::vector<std::string> split(const std::string &s, char delim) {
	std::vector<std::string> elems;
	split(s, delim, elems);
	return elems;
}

Monosat::SimpSolver * newSolver_arg(const char*argv){
	if (argv && strlen(argv)>0){
		std::string args;
		args = argv;
		vector<char*> tokens;
		split(args,' ',tokens);
		Monosat::SimpSolver * s = newSolver_args(tokens.size(),(char **) tokens.data());
		//why is the following causing errors?
		/*while(tokens.size()){
			char * t = tokens.back();
			tokens.pop_back();
			delete[]t;
		}*/
		return s;
	}else{
		return newSolver_args(0,nullptr);
	}
}


Monosat::SimpSolver * newSolver_args(int argc,  char**argv){
	using namespace APISignal;
	string args ="";
	for (int i = 0;i<argc;i++){
		args.append(" ");
		args.append(argv[i]);
	}

	parseOptions(argc, argv, true);
	if (opt_adaptive_conflict_mincut == 1) {
		opt_conflict_min_cut = true;
		opt_conflict_min_cut_maxflow = true;
	}
	Monosat::opt_record=strlen(opt_record_file)>0;
	if(strlen(opt_debug_learnt_clauses)>0){
		opt_write_learnt_clauses=fopen(opt_debug_learnt_clauses,"w");
	}else{
		opt_write_learnt_clauses=nullptr;
	}
	_selectAlgorithms();
	Monosat::SimpSolver * S = new Monosat::SimpSolver();
	solvers.insert(S);//add S to the list of solvers handled by signals


	S->_external_data =(void*)new MonosatData(S);
	((MonosatData*)S->_external_data)->args =args;
	if(!opt_pre){
		S->eliminate(true);//disable preprocessing.
	}
	((MonosatData*)S->_external_data)->pbsolver = new PB::PbSolver(*S);
	S->setPBSolver(((MonosatData*)S->_external_data)->pbsolver);

	{

		Dimacs<StreamBuffer, SimpSolver> * parser = new Dimacs<StreamBuffer, SimpSolver>();
		BVParser<char *, SimpSolver>* bvParser = new BVParser<char *, SimpSolver>();
		parser->addParser(bvParser);

		SymbolParser<char*,SimpSolver>* symbolParser = new SymbolParser<char*,SimpSolver>() ;
		parser->addParser(symbolParser);
		bool precise = true;
		GraphParser<char *, SimpSolver>* graphParser = new GraphParser<char *, SimpSolver>(precise,bvParser->theory);
		parser->addParser(graphParser);

		PBParser<char *, SimpSolver>* pbParser = new PBParser<char *, SimpSolver>(*S);
		parser->addParser(pbParser);

		AMOParser<char *, SimpSolver> * amo = new AMOParser<char *, SimpSolver>();
		parser->addParser(amo);

		((MonosatData*)S->_external_data)->parser = parser;
		S->setVarMap(parser);
	}
	return S ;
}
void deleteSolver (Monosat::SimpSolver * S)
{
	using namespace APISignal;
	S->interrupt();
	solvers.erase(S);//remove S from the list of solvers in the signal handler
	if(S->_external_data){
		MonosatData* data = (MonosatData*) S->_external_data;
		if(data->outfile){
			fclose(data->outfile);
			data->outfile = nullptr;
		}
		delete(data);
		S->_external_data=nullptr;
	}
	delete (S);
}


bool ok(Monosat::SimpSolver * S){
	return S->okay();
}

void clearOptimizationObjectives(Monosat::SimpSolver * S){
	MonosatData * d = (MonosatData*) S->_external_data;
	write_out(S,"clear_opt\n");
	d->optimization_objectives.clear();
}

void maximizeBV(Monosat::SimpSolver *  S,  Monosat::BVTheorySolver<int64_t> *  bv, int bvID){
	MonosatData * d = (MonosatData*) S->_external_data;
	write_out(S,"maximize bv %d\n", bvID);
	if(!S->getBVTheory()){
		api_errorf("No bitvector theory created (call initBVTheory())!");
	}
	if(! ((Monosat::BVTheorySolver<int64_t> *) S->getBVTheory())->hasBV(bvID)){
		api_errorf("Minimization bitvector %d is not allocated",bvID);
	}
	d->optimization_objectives.push(Objective(bvID,true));
}

void minimizeBV(Monosat::SimpSolver *  S,  Monosat::BVTheorySolver<int64_t> *  bv, int bvID){
	MonosatData * d = (MonosatData*) S->_external_data;
	write_out(S,"minimize bv %d\n", bvID);
	if(!S->getBVTheory()){
		api_errorf("No bitvector theory created (call initBVTheory())!");
	}
	if(! ((Monosat::BVTheorySolver<int64_t> *) S->getBVTheory())->hasBV(bvID)){
		api_errorf("Minimization bitvector %d is not allocated",bvID);
	}
	d->optimization_objectives.push(Objective(bvID,false));
}

void maximizeLits(Monosat::SimpSolver *  S, int * lits, int n_lits){
	if(n_lits<=0)
		return;
	MonosatData * d = (MonosatData*) S->_external_data;
	static vec<Lit> lits_opt;
	lits_opt.clear();
	for (int i = 0;i<n_lits;i++){
		lits_opt.push(internalLit(S,lits[i]));
	}
	write_out(S,"maximize lits %d ",lits_opt.size());
	for(Lit l:lits_opt){
		write_out(S,"%d ",dimacs(S,l));
	}
	write_out(S,"\n");

	d->optimization_objectives.push(Objective(lits_opt,true));
}
void minimizeLits(Monosat::SimpSolver *  S, int * lits, int n_lits){
	if(n_lits<=0)
		return;
	MonosatData * d = (MonosatData*) S->_external_data;
	static vec<Lit> lits_opt;
	lits_opt.clear();
	for (int i = 0;i<n_lits;i++){
		lits_opt.push(internalLit(S,lits[i]));
	}
	write_out(S,"minimize lits %d ",lits_opt.size());
	for(Lit l:lits_opt){
		write_out(S,"%d ",dimacs(S,l));
	}
	write_out(S,"\n");

	d->optimization_objectives.push(Objective(lits_opt,false));
}
void maximizeWeightedLits(Monosat::SimpSolver *  S, int * lits, int * weights, int n_lits){
	if(n_lits<=0)
		return;
	MonosatData * d = (MonosatData*) S->_external_data;
	static vec<Lit> lits_opt;
	static vec<int> weights_opt;
	lits_opt.clear();
	for (int i = 0;i<n_lits;i++){
		lits_opt.push(internalLit(S,lits[i]));
	}
	weights_opt.clear();
	for (int i = 0;i<n_lits;i++){
		weights_opt.push(weights[i]);
	}
    while(weights_opt.size()>lits_opt.size()){
        weights_opt.pop();
    }
    while(weights_opt.size()<lits_opt.size()){
        weights_opt.push(1);
    }

	write_out(S,"maximize lits %d ",lits_opt.size());
	for(Lit l:lits_opt){
		write_out(S,"%d ",dimacs(S,l));
	}
	for(int w:weights_opt){
		write_out(S,"%d ",w);
	}
	write_out(S,"0\n");

	d->optimization_objectives.push(Objective(lits_opt,weights_opt,true));
}
void minimizeWeightedLits(Monosat::SimpSolver *  S, int * lits, int * weights, int n_lits){
	if(n_lits<=0)
		return;
	MonosatData * d = (MonosatData*) S->_external_data;
	static vec<Lit> lits_opt;
	static vec<int> weights_opt;
	lits_opt.clear();
	for (int i = 0;i<n_lits;i++){
		lits_opt.push(internalLit(S,lits[i]));
	}
	weights_opt.clear();
	for (int i = 0;i<n_lits;i++){
		weights_opt.push(weights[i]);
	}
    while(weights_opt.size()>lits_opt.size()){
        weights_opt.pop();
    }
    while(weights_opt.size()<lits_opt.size()){
        weights_opt.push(1);
    }
	write_out(S,"minimize lits %d ",lits_opt.size());
	for(Lit l:lits_opt){
		write_out(S,"%d ",dimacs(S,l));
	}
	for(int w:weights_opt){
		write_out(S,"%d ",w);
	}
	write_out(S,"0\n");
	d->optimization_objectives.push(Objective(lits_opt,weights_opt,false));
}

int minimizeUnsatCore(Monosat::SimpSolver * S, int * unsat_assumptions, int n_lits){

	MonosatData * d = (MonosatData*) S->_external_data;
	d->last_solution_optimal=true;
	d->has_conflict_clause_from_last_solution=false;
	vec<Lit> assumptions;
	write_out(S,"minimize_core ");
	for (int n = 0;n<n_lits;n++){
		Lit l = internalLit(S,unsat_assumptions[n]);
		write_out(S,"%d ", dimacs(S,l));
		assumptions.push(l);
	}
	write_out(S,"\n");

	APISignal::enableResourceLimits();

	S->cancelUntil(0);
	S->preprocess();//do this _even_ if sat based preprocessing is disabled! Some of the theory solvers depend on a preprocessing call being made!



	//vec<Objective> & objectives = d->optimization_objectives;//bit vectors to minimize
	if (d->pbsolver) {
		d->pbsolver->convert();
	}
	//lbool minimizeUnsatCore(SimpSolver & S,vec<Lit> & assumptions,bool do_simp){
	lbool r = minimizeCore(*S,assumptions,opt_pre);
	assert(assumptions.size()<=n_lits);
	//store the minimized conflict back in the unsat assumptions pointer
	for(int i = 0;i<assumptions.size();i++){
		unsat_assumptions[i]=externalLit(S,assumptions[i]);
	}
	d->last_solution_optimal=r!=l_Undef;
	if(r!=l_True){
		d->has_conflict_clause_from_last_solution=true;
		S->conflict.clear();
		//store the minimized conflict back in the solvers conflict set
		for(int i = 0;i<assumptions.size();i++){
			assert(assumptions[i]!=lit_Undef);
			S->conflict.insert(~assumptions[i]);
		}
	}else{
		d->has_conflict_clause_from_last_solution=false;
	}
	if (opt_verb >= 1) {
		printStats(S);

	}
	APISignal::disableResourceLimits();
	return assumptions.size();
}

void minimizeConflictClause(Monosat::SimpSolver * S){
	MonosatData * d = (MonosatData*) S->_external_data;
	if(d && d->has_conflict_clause_from_last_solution){
		vec<int> assumptions;
		for(Lit l:S->conflict){
			assumptions.push(externalLit(S,~l));
		}
		int size = minimizeUnsatCore(S,&assumptions[0],assumptions.size());
		assert(size<=assumptions.size());
		assert(S->conflict.size()==size);
	}
}


void readGNF(Monosat::SimpSolver * S, const char  * filename){

	gzFile in = gzopen(filename, "rb");
	if (in == nullptr)
		throw std::runtime_error("ERROR! Could not open file");
	try {
		MonosatData *d = (MonosatData *) S->_external_data;
		auto &parser = *d->parser;
		StreamBuffer strm(in);
		vec<int> assumps;
		bool ran_last_solve = false;
		d->optimization_objectives.clear();
		while (parser.parse(strm, *S)) {
			assumps.clear();
			for (Lit l:parser.assumptions) {
				assumps.push(externalLit(S, l));
			}
			d->optimization_objectives.clear();
			for (Objective &o:parser.objectives) {
				d->optimization_objectives.push(o);
			}

			solveAssumptions(S, &assumps[0], assumps.size());
			if (*strm == EOF) {
				ran_last_solve = true;
			}
		}
		assert(*strm == EOF);
		if (!ran_last_solve) {
			for (Lit l:parser.assumptions) {
				assumps.push(externalLit(S, l));
			}
			d->optimization_objectives.clear();
			for (Objective &o:parser.objectives) {
				d->optimization_objectives.push(o);
			}
			solveAssumptions(S, &assumps[0], assumps.size());
		}
		d->optimization_objectives.clear();
	}catch(...)
	{
		gzclose(in);
		throw;
	}
}

Monosat::GraphTheorySolver<int64_t> *  newGraph(Monosat::SimpSolver * S){
	return newGraph_Named(S,"",-2);
}

Monosat::GraphTheorySolver<int64_t> *  newGraph_Named(Monosat::SimpSolver * S, const char * _name, int bitwidth){
    MonosatData * d = (MonosatData*) S->_external_data;
    std::string name = (_name==nullptr)?std::string(""):std::string(_name);
    Monosat::GraphTheorySolver<int64_t> *graph = new Monosat::GraphTheorySolver<int64_t>(S,name,bitwidth);

    d->graphs.push(graph);
    if( d->bv_theory){
        graph->setBVTheory(d->bv_theory);
    }
	write_out(S,"digraph 0 0 %d %d",graph->getTheoryIndex(), graph->getEdgeWeightBitWidth());
    if (name.size()>0){
		write_out(S," %s\n",name.c_str());
    }else{
		write_out(S,"\n");
    }

    return graph;
}

Monosat::GraphTheorySolver<int64_t> *  getGraph(Monosat::SimpSolver * S, const char * _name){
	std::string name = (_name==nullptr)?std::string(""):std::string(_name);
	if(name.size()==0){
		return nullptr;
	}
	Theory * theory = S->getTheory(name);
	if(theory==nullptr){
		return nullptr;
	}
	assert(theory->getName()==name);
	return dynamic_cast<Monosat::GraphTheorySolver<int64_t>*>(theory);
}

const char * getGraphName(SolverPtr S, GraphTheorySolver_long G){
	return G->getName().c_str();
}

int getGraphWidth(SolverPtr S, GraphTheorySolver_long G){
	return G->getEdgeWeightBitWidth();
}

void backtrack(Monosat::SimpSolver * S){
	S->cancelUntil(0);
}
Monosat::BVTheorySolver<int64_t> * initBVTheory(Monosat::SimpSolver * S){
	MonosatData * d = (MonosatData*) S->_external_data;
	if(d->bv_theory)
		return d->bv_theory;

	Monosat::BVTheorySolver<int64_t> * bv = new Monosat::BVTheorySolver<int64_t>(S);

	d->bv_theory=bv;
	for (auto graph:d->graphs)
		graph->setBVTheory(bv);

	return bv;
}
bool solve(Monosat::SimpSolver * S){
	return solveAssumptions(S,nullptr,0);
}



void setTimeLimit(Monosat::SimpSolver * S,int seconds){
	using namespace APISignal;
	// Set limit on CPU-time:
	time_limit=seconds;
}
void setMemoryLimit(Monosat::SimpSolver * S,int mb){
	using namespace APISignal;
	memory_limit=mb;
}
void setConflictLimit(Monosat::SimpSolver * S,int num_conflicts){
	S->setConfBudget(num_conflicts);
}
void setPropagationLimit(Monosat::SimpSolver * S,int num_propagations){
	S->setPropBudget(num_propagations);
}



int _solve(Monosat::SimpSolver * S,int * assumptions, int n_assumptions){
	bool found_optimal=true;
	MonosatData * d = (MonosatData*) S->_external_data;
	d->last_solution_optimal=true;
	d->has_conflict_clause_from_last_solution=false;

	write_out(S,"solve");
	for(int i = 0;i<n_assumptions;i++){
		Lit l =internalLit(S, assumptions[i]);
		write_out(S," %d",dimacs(S,l));
	}
	write_out(S,"\n");

	APISignal::enableResourceLimits();

	S->cancelUntil(0);
	S->preprocess();//do this _even_ if sat based preprocessing is disabled! Some of the theory solvers depend on a preprocessing call being made!

	vec<Monosat::Lit> assume;
	for (int i = 0;i<n_assumptions;i++){
		Lit l =internalLit(S, assumptions[i]);
		if (var(l)>=S->nVars()){
			api_errorf("Assumption literal %d is not allocated",dimacs(S,l));
		}
		assume.push(l);
		//S->setFrozen(v,true); //this is done in the solve() call
	}

/*	  if (opt_pre){
		S->eliminate(false);//should this really be set to disable future preprocessing here?
	 }*/

	vec<Objective> & objectives = d->optimization_objectives;//bit vectors to minimize
	if (d->pbsolver) {
		d->pbsolver->convert();
	}
	lbool r = optimize_and_solve(*S, assume,objectives,opt_pre,found_optimal);
	d->last_solution_optimal=found_optimal;
	if(r==l_False){
		d->has_conflict_clause_from_last_solution=true;
	}
	if (opt_verb >= 1) {
		printStats(S);

	}
	APISignal::disableResourceLimits();

	return toInt(r);
}

int solveLimited(Monosat::SimpSolver * S){
	return solveAssumptionsLimited(S,nullptr,0);
}

int solveAssumptionsLimited(Monosat::SimpSolver * S,int * assumptions, int n_assumptions){
	return _solve(S, assumptions,  n_assumptions);
	//return solveAssumptionsLimited_MinBVs(S,assumptions,n_assumptions,nullptr,0);
}

bool solveAssumptions(Monosat::SimpSolver * S,int * assumptions, int n_assumptions){
	return _solve(S,assumptions,n_assumptions) == toInt(l_True);
}
bool lastSolutionWasOptimal(Monosat::SimpSolver * S){
	MonosatData * d = (MonosatData*) S->_external_data;
	if(d){
		return d->last_solution_optimal;
	}else{
		return false;
	}
}


int getConflictClause(Monosat::SimpSolver * S, int * store_clause, int max_store_size){
	MonosatData * d = (MonosatData*) S->_external_data;
	if(d && d->has_conflict_clause_from_last_solution){
		int size =  S->conflict.size();
		for (int i =0;i<size && i<max_store_size;i++){
			Lit l = S->conflict[i];
			store_clause[i]=externalLit(S,l);
		}
		return size;
	}else{
		return -1;
	}
}


/*
bool solveAssumptions_MinBVs(Monosat::SimpSolver * S,int * assumptions, int n_assumptions, int * minimize_bvs, int n_minimize_bvs){
	APISignal::disableResourceLimits();
	S->budgetOff();
	lbool ret = toLbool(_solve(S,assumptions, n_assumptions, minimize_bvs, n_minimize_bvs));
	if(ret==l_True){
		return true;
	}else if (ret==l_False){
		return false;
	}else{
		throw std::runtime_error("Failed to solve!");
	}
}
int solveAssumptionsLimited_MinBVs(Monosat::SimpSolver * S,int * assumptions, int n_assumptions, int * minimize_bvs, int n_minimize_bvs){
	int r = _solve(S, assumptions,  n_assumptions,  minimize_bvs,  n_minimize_bvs);
	return r;
}*/


int newVar(Monosat::SimpSolver * S){
	return externalVar(S, S->newVar());
}
//Create a new named variable, only if this name is unique and valid (or if it is empty).
//If the name is not empty, and is non-unique or invalid, an exception will be thrown and no variable will be created at all.
int newNamedVar(Monosat::SimpSolver * S,const char  * varname){
    if(varname != nullptr && strlen(varname)>0) {
        std::string name(varname);
        if (S->hasVariable(name)){
            throw std::invalid_argument("All variable names must be unique.");
        }else{
            //check if any chars of name are illegal
            for(char c:name){
                if(!isascii(c) || !isprint(c) || isspace(c)){
                    throw std::invalid_argument(std::string("Variable names must consist only of printable, non-whitespace ASCII. Invalid character in variable name: ") + name);
                }
            }
        }
        Var v = newVar(S);
        setVariableName(S,v,varname);
        return v;
    }else{
        return newVar(S);
    }

}


void setVariableName(Monosat::SimpSolver * S, int variable, const char  * varname){
	if(varname==nullptr){
		S->setVariableName(internalVar(S,variable), "");
	}else{
		std::string name(varname);
		S->setVariableName(internalVar(S,variable), varname);
		write_out(S, "symbol %d %s\n", variable + 1, varname);
	}
}

bool variableHasName(Monosat::SimpSolver * S, int variable){
	return S->hasName(internalVar(S,variable));
}
bool hasVariableWithName(Monosat::SimpSolver * S, const char * name){
	return S->hasVariable(name);
}
Var getVariable(Monosat::SimpSolver * S, const char * varname){
	return externalVar(S,S->getVariable(varname));
}
//Return the name associated with this string, or the empty string if there is no name associated with this string.
const char * getVariableName(Monosat::SimpSolver * S, int variable){
	return S->getVariableName(internalVar(S,variable)).c_str();
}

Var getNamedVariableN(Monosat::SimpSolver * S,int n){
    return externalVar(S,S->namedVariables()[n]);
}

int nNamedVariables(Monosat::SimpSolver * S){
	return S->namedVariables().size();
}


int getNamedBitvectorN(Monosat::SimpSolver * S,Monosat::BVTheorySolver<int64_t> * bv,int n){
    return bv->namedBitvectors()[n];
}

int nNamedBitvectors(Monosat::SimpSolver * S,Monosat::BVTheorySolver<int64_t> * bv){
	return bv->namedBitvectors().size();
}

void releaseLiteral(Monosat::SimpSolver * S, int literal){
    assert(literal>=0);
    S->releaseVar(internalLit(S,literal));
}

bool disallowLiteralSimplification(Monosat::SimpSolver * S, int literal){
	if(S->isEliminated(var(internalLit(S,literal)))){
		fprintf(stderr,"Warning: Literal %d has already been eliminated by the pre-processor\n", dimacs(S,internalLit(S,literal)));
		return false;
	}else
		S->setFrozen(var(internalLit(S,literal)),true);
	return true;
}
void disablePreprocessing(Monosat::SimpSolver * S){
	S->disablePreprocessing();
}

void setDecisionVar(Monosat::SimpSolver * S,int variable,bool decidable){
	if(S->isDecisionVar(internalVar(S,variable))!=decidable) {
		write_out(S, "decision %d %d\n", variable + 1, decidable);//add 1 for dimacs
		S->setDecisionVar(internalVar(S,variable), decidable);
	}
}
void setDecisionPriority(Monosat::SimpSolver * S,int variable, int priority){
	if(S->getDecisionPriority(internalVar(S,variable))!=priority) {
		write_out(S, "priority %d %d\n", variable + 1, priority);//add 1 for dimacs
		S->setDecisionPriority(internalVar(S,variable), priority);
	}
}
bool isDecisionVar(Monosat::SimpSolver * S,int variable){
	return S->isDecisionVar(internalVar(S,variable));
}
int getDecisionPriority(Monosat::SimpSolver * S,int variable){
	return S->getDecisionPriority(internalVar(S,variable));
}

void setDecisionPolarity(Monosat::SimpSolver * S,Var variable, bool b){
	S->setPolarity(internalVar(S,variable),b);
}

bool getDecisionPolarity(Monosat::SimpSolver * S,Var variable){
	return S->getPolarity(internalVar(S,variable));
}


int nVars(Monosat::SimpSolver * S){
    //Return the number of _externally visible_ variables.
    return S->nMappedVars();
	//return S->nVars();
}

int nClauses(Monosat::SimpSolver * S){
	return S->nClauses();
}
int nBitvectors(Monosat::SimpSolver * S,Monosat::BVTheorySolver<int64_t> * bv){
	return bv->nBitvectors();
}

int true_lit(Monosat::SimpSolver * S){
	bool needs_record=S->const_true==lit_Undef;

	Lit l = S->True();
	if(needs_record){
		write_out(S,"%d 0\n",dimacs(S,l));
	}
	return externalLit(S,l);
}
bool addClause(Monosat::SimpSolver * S,int * lits, int n_lits){
	static vec<Lit> clause;
	clause.clear();
	for (int i = 0;i<n_lits;i++){
		clause.push(internalLit(S,lits[i]));
	}

	for(Lit l:clause){
		write_out(S,"%d ",dimacs(S,l));
	}
	write_out(S,"0\n");
	return S->addClause(clause);
}
bool addUnitClause(Monosat::SimpSolver * S,int lit){

	write_out(S,"%d 0\n",dimacs(S,internalLit(S,lit)));

	return S->addClause(internalLit(S,lit));
}
bool addBinaryClause(Monosat::SimpSolver * S,int lit1, int lit2){
	write_out(S,"%d %d 0\n",dimacs(S,internalLit(S,lit1)), dimacs(S,internalLit(S,lit2)));
	return S->addClause(internalLit(S,lit1),internalLit(S,lit2));
}

void addBinaryClauses(Monosat::SimpSolver * S,int * first_args, int * second_args, int n_pairs){
    assert(first_args);
    assert(second_args);
    assert(n_pairs>=0);
    for(int i = 0;i<n_pairs;i++){
        addBinaryClause(S,first_args[i],second_args[i]);
    }
}

bool addTertiaryClause(Monosat::SimpSolver * S,int lit1, int lit2, int lit3){
	write_out(S,"%d %d %d 0\n",dimacs(S,internalLit(S,lit1)), dimacs(S,internalLit(S,lit2)), dimacs(S,internalLit(S,lit3)));
	return S->addClause(internalLit(S,lit1),internalLit(S,lit2),internalLit(S,lit3));
}

//theory interface for bitvectors
int newBitvector_anon(Monosat::SimpSolver * S,Monosat::BVTheorySolver<int64_t> * bv, int bvWidth){
	int bvID =bv->newBitvector_Anon(-1,bvWidth).getID();
    bvID = externalBV(bv,bvID);
	write_out(S,"bv anon %d %d\n",bvID, bvWidth);
	return bvID;
}
int newBitvector_const(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvWidth, int64_t constval){
	int bvID = bv->newBitvector(-1,bvWidth,constval).getID();
    bvID = externalBV(bv,bvID);
	write_out(S,"bv const %d %d %" PRId64 "\n",bvID, bvWidth, constval);
	return bvID;
}


int newBitvector(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int * bits, int n_bits){
	static vec<Var> lits;
	lits.clear();
	for (int i = 0;i<n_bits;i++){
		lits.push(Var(bits[i]));
	}
	int bvID = bv->nBitvectors();
	bv->newBitvector(bvID,lits);
	bvID = externalBV(bv,bvID);
	write_out(S,"bv %d %d",bvID, n_bits);
	for (int i = 0;i<n_bits;i++){
		write_out(S," %d",dimacs(S,mkLit(lits[i])));
	}
	write_out(S,"\n");
	return bvID;
}

void setBitvectorName(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, const char * name){
	if(name != nullptr && strlen(name)>0) {
		bv->setSymbol(internalBV(bv,bvID), name);
		write_out(S, "bv symbol %d %s\n", bvID, name);
	}
}

bool bitvectorHasName(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID){
    return bv->bitvectorHasName(internalBV(bv,bvID));
}

bool hasBitvectorWithName(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, const char * name){
	return bv->hasBitVector(name);
}

const char * getBitvectorName(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID){
	return bv->getSymbol(internalBV(bv,bvID)).c_str();
}

/*
 * The width of the bitvector.
 */
int bv_width(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int bvID){
	return bv->getWidth(internalBV(bv,bvID));
}
/*
 * The number of defined literals making up the bits of this bitvector.
 * May be equal to the width of the bitvector, or exactly 0.
 */
int bv_nBits(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int bvID){
	return bv->getBits(internalBV(bv,bvID)).size();
}

/*
 * Retrieve the nth bit of this bitvector.
 * Note: 'bit' must  be less than the number of bits in the bitvector.
 * Some bitvectors have 0 defined bits, even if they have non-zero width.
 */
int bv_bit(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int bvID, int bit){
	if(bit<0|| bit>=bv_nBits(S,bv,internalBV(bv,bvID))){
		throw std::runtime_error("BV bit out of range");
	}
	return externalLit(S,bv->toSolver(bv->getBits(internalBV(bv,bvID))[bit]));
}

int getBitvector(SolverPtr S, BVTheoryPtr bv, const char * name){
    return externalBV(bv, bv->getBitvector(name));
}

int newBVComparison_const_lt(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int64_t weight){
	//Var v = newVar(S);
	//Lit l =mkLit(v);
	Lit l =bv->toSolver(bv->newComparison(Monosat::Comparison::lt,internalBV(bv,bvID),weight));
	write_out(S,"bv const < %d %d %" PRId64 "\n",dimacs(S,l),bvID, weight);

	return externalLit(S,l);
}
int newBVComparison_bv_lt(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int compareID){
	//Var v = newVar(S);
	//Lit l =mkLit(v);
	Lit l =bv->toSolver( bv->newComparisonBV(Monosat::Comparison::lt,internalBV(bv,bvID),compareID));
	write_out(S,"bv < %d %d %d\n",dimacs(S,l),bvID, compareID);

	return externalLit(S,l);
}
int newBVComparison_const_leq(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int64_t weight){
	//Var v = newVar(S);
	//Lit l =mkLit(v);

	Lit l =bv->toSolver(bv->newComparison(Monosat::Comparison::leq,internalBV(bv,bvID),weight));
	//printf("Const bv leq bv %d to %" PRId64 " = var %d\n",bvID, weight, dimacs(S,l));
	write_out(S,"bv const <= %d %d %" PRId64 "\n",dimacs(S,l),bvID, weight);
	return externalLit(S,l);
}
int newBVComparison_bv_leq(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int compareID){
	//Var v = newVar(S);
	// Lit l =mkLit(v);
	Lit l =bv->toSolver(bv->newComparisonBV(Monosat::Comparison::leq,internalBV(bv,bvID),compareID));
	write_out(S,"bv <= %d %d %d\n",dimacs(S,l),bvID, compareID);
	return externalLit(S,l);
}

int newBVComparison_const_gt(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int64_t weight){
	// Var v = newVar(S);
	// Lit l =mkLit(v);
	Lit l =bv->toSolver( bv->newComparison(Monosat::Comparison::gt,internalBV(bv,bvID),weight));
	write_out(S,"bv const > %d %d %" PRId64 "\n",dimacs(S,l),bvID, weight);
	return externalLit(S,l);
}
int newBVComparison_bv_gt(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int compareID){
	//Var v = newVar(S);
	//Lit l =mkLit(v);
	Lit l =bv->toSolver( bv->newComparisonBV(Monosat::Comparison::gt,internalBV(bv,bvID),compareID));
	write_out(S,"bv > %d %d %d\n",dimacs(S,l),bvID, compareID);
	return externalLit(S,l);
}
int newBVComparison_const_geq(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int64_t weight){
	//Var v = newVar(S);
	//Lit l =mkLit(v);
	Lit l =bv->toSolver(bv->newComparison(Monosat::Comparison::geq,internalBV(bv,bvID),weight));
	//printf("Const bv geq bv %d to %" PRId64 " = var %d\n",bvID, weight, dimacs(S,l));
	write_out(S,"bv const >= %d %d %" PRId64 "\n",dimacs(S,l),bvID, weight);
	return externalLit(S,l);
}
int newBVComparison_bv_geq(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, int compareID){
	//Var v = newVar(S);
	//Lit l =mkLit(v);
	Lit l =bv->toSolver( bv->newComparisonBV(Monosat::Comparison::geq,internalBV(bv,bvID),compareID));
	write_out(S,"bv >= %d %d %d\n",dimacs(S,l),bvID, compareID);
	return externalLit(S,l);
}

int newBVComparison_const_eq(Monosat::SimpSolver * S,  Monosat::BVTheorySolver<int64_t> *  bv, int bvID, Weight weight){
    Lit a = internalLit(S,newBVComparison_const_geq(S,bv,bvID,weight));
    Lit b = internalLit(S,newBVComparison_const_gt(S,bv,bvID,weight));
    Lit c = mkLit(S->newVar());S->disableElimination(var(c));
    S->addClause(a, ~c);
    S->addClause(~b, ~c);
    S->addClause(c, ~a, b);
    return externalLit(S,c);
}
int newBVComparison_bv_eq(Monosat::SimpSolver * S,  Monosat::BVTheorySolver<int64_t> *  bv, int bvID, int compareID){

    Lit a = internalLit(S,newBVComparison_bv_geq(S,bv,bvID,compareID));
    Lit b = internalLit(S,newBVComparison_bv_gt(S,bv,bvID,compareID));
    Lit c = mkLit(S->newVar());S->disableElimination(var(c));
    S->addClause(a, ~c);
    S->addClause(~b, ~c);
    S->addClause(c, ~a, b);
    //add redundant (but likely helpful) bit level implications
    vec<Lit> & bits1 = bv->getBits(internalBV(bv,bvID));
    vec<Lit> & bits2 = bv->getBits(internalBV(bv,compareID));
    if(bits1.size()==bits2.size()) {
        //watch out for anonymous bitvectors
        for (int i = 0; i < bits1.size(); i++) {
            Lit l1 = bv->toSolver(bits1[i]);
            Lit l2 = bv->toSolver(bits2[i]);
            //either each bit is equal, or c is false
            S->addClause(l1, ~l2, ~c);
            S->addClause(~l1, l2, ~c);
        }
    }
    return externalLit(S,c);
}
int newBVComparison_const_neq(Monosat::SimpSolver * S,  Monosat::BVTheorySolver<int64_t> *  bv, int bvID, Weight weight){
    return externalLit(S,~internalLit(S,newBVComparison_const_eq(S,bv,bvID,weight)));
}
int newBVComparison_bv_neq(Monosat::SimpSolver * S,  Monosat::BVTheorySolver<int64_t> *  bv, int bvID, int compareID){
    return externalLit(S,~internalLit(S,newBVComparison_bv_eq(S,bv,bvID,compareID)));
}

void bv_min(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int* args, int n_args,int resultID){
	vec<int> m_args;
	for (int i = 0;i<n_args;i++)
		m_args.push(args[i]);
	write_out(S,"bv min %d %d",resultID, n_args);
	for(int i = 0;i<n_args;i++){
		write_out(S," %d",args[i]);
	}
	write_out(S,"\n");
	bv->newMinBV(internalBV(bv,resultID), m_args);
}
void bv_max(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,  int* args,int n_args, int resultID){
	vec<int> m_args;
	for (int i = 0;i<n_args;i++)
		m_args.push(args[i]);
	write_out(S,"bv max %d %d",resultID, n_args);
	for(int i = 0;i<n_args;i++){
		write_out(S," %d",args[i]);
	}
	write_out(S,"\n");
	bv->newMaxBV(internalBV(bv,resultID), m_args);
}
void bv_popcount(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,  int* args,int n_args, int resultID){
	vec<int> m_args;
	for (int i = 0;i<n_args;i++){
		Lit l = internalLit(S,args[i]);
		if(sign(l)){
			api_errorf("Popcount arguments must all be positive literals");
		}
		m_args.push(var(l));
	}
	write_out(S,"bv popcount %d %d",resultID, n_args);
	for(int i = 0;i<n_args;i++){
		write_out(S," %d",dimacs(S,mkLit(m_args[i])));
	}
	write_out(S,"\n");
	bv->newPopCountBV(internalBV(bv,resultID), m_args);
}

void bv_unary(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,  int* args,int n_args, int resultID){
    vec<Lit> m_args;
    for (int i = 0;i<n_args;i++){
        Lit l = internalLit(S,args[i]);
        if(sign(l)){
            api_errorf("Unary arguments must all be positive literals");
        }
        m_args.push(l);
    }
    for(int i = 1;i<m_args.size();i++){
        if(var(m_args[i])!= var(m_args[i-1])+1){
            api_errorf("Unary arguments must be sequential");
        }
    }
    write_out(S,"bv unary %d %d",resultID, n_args);
    for(int i = 0;i<n_args;i++){
        write_out(S," %d",dimacs(S,m_args[i]));
    }
    write_out(S,"\n");
    //bv->newPopCountBV(resultID, m_args);

    bv->getUnary(internalBV(bv,resultID), m_args);


}
void bv_addition( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID1, int bvID2, int resultID){
	write_out(S,"bv + %d %d %d\n",resultID,bvID1, bvID2);
	bv->newAdditionBV(internalBV(bv,resultID),internalBV(bv,bvID1),internalBV(bv,bvID2));

}
void bv_subtraction( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID1, int bvID2, int resultID){
	write_out(S,"bv - %d %d %d\n",resultID,bvID1, bvID2);
	bv->newSubtractionBV(internalBV(bv,resultID),internalBV(bv,bvID1),internalBV(bv,bvID2));
}
void bv_multiply( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID1, int bvID2, int resultID){
	write_out(S,"bv * %d %d %d\n",resultID,bvID1, bvID2);
	bv->newMultiplicationBV(internalBV(bv,resultID),internalBV(bv,bvID1),internalBV(bv,bvID2));
}
void bv_divide( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID1,  int bvID2, int resultID){
	write_out(S,"bv / %d %d %d\n",resultID,bvID1, bvID2);
	bv->newDivisionBV(internalBV(bv,resultID),internalBV(bv,bvID1),internalBV(bv,bvID2));
}

void bv_ite( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int condition_lit,int bvThenID, int bvElseID, int bvResultID){
	Lit l = internalLit(S,condition_lit);

	write_out(S,"bv_ite %d %d %d %d\n",dimacs(S,mkLit(condition_lit)),bvThenID,bvElseID,bvResultID);
	bv->newConditionalBV(l,internalBV(bv,bvThenID),internalBV(bv,bvElseID),internalBV(bv,bvResultID));
}

void bv_not(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int a,  int out){
	//return bv->bitwiseAnd(bv->getBV(a),bv->getBV(b)).getID();
	write_out(S,"bv not %d %d\n",a, out);
	bv->bitwiseNot(bv->getBV(internalBV(bv,a)),bv->getBV(internalBV(bv,out)));
}

void bv_and(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int a, int b, int out){
	//return bv->bitwiseAnd(bv->getBV(a),bv->getBV(b)).getID();
	write_out(S,"bv and %d %d %d \n",a,b, out);
	bv->bitwiseAnd(bv->getBV(internalBV(bv,a)),bv->getBV(internalBV(bv,b)),bv->getBV(internalBV(bv,out)));
}
void bv_nand( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int a, int b, int out){
	//return bv->bitwiseNand(bv->getBV(a),bv->getBV(b)).getID();
	write_out(S,"bv nand %d %d %d \n",a,b, out);
	bv->bitwiseNand(bv->getBV(internalBV(bv,a)),bv->getBV(internalBV(bv,b)),bv->getBV(internalBV(bv,out)));
}
void bv_or( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int a, int b, int out){
	//return bv->bitwiseOr(bv->getBV(a),bv->getBV(b)).getID();
	write_out(S,"bv or %d %d %d \n",a,b, out);
	bv->bitwiseOr(bv->getBV(internalBV(bv,a)),bv->getBV(internalBV(bv,b)),bv->getBV(internalBV(bv,out)));
}
void bv_nor( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int a, int b, int out){
	//return bv->bitwiseNor(bv->getBV(a),bv->getBV(b)).getID();
	write_out(S,"bv nor %d %d %d \n",a,b, out);
	bv->bitwiseNor(bv->getBV(internalBV(bv,a)),bv->getBV(internalBV(bv,b)),bv->getBV(internalBV(bv,out)));
}
void bv_xor( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int a, int b, int out){
	//return bv->bitwiseXor(bv->getBV(a),bv->getBV(b)).getID();
	write_out(S,"bv xor %d %d %d \n",a,b, out);
	bv->bitwiseXor(bv->getBV(internalBV(bv,a)),bv->getBV(internalBV(bv,b)),bv->getBV(internalBV(bv,out)));
}
void bv_xnor( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int a, int b, int out){
	//return bv->bitwiseXnor(bv->getBV(a),bv->getBV(b)).getID();
	write_out(S,"bv xnor %d %d %d \n",a,b, out);
	bv->bitwiseXnor(bv->getBV(internalBV(bv,a)),bv->getBV(internalBV(bv,b)),bv->getBV(internalBV(bv,out)));
}

void bv_concat( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int aID, int bID, int resultID){
	write_out(S,"bv concat %d %d %d \n",aID,bID, resultID);
	bv->concat(bv->getBV(internalBV(bv,aID)), bv->getBV(internalBV(bv,bID)),bv->getBV(internalBV(bv,resultID)));
}

void bv_slice( Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv,int aID, int lower, int upper, int resultID){
	write_out(S,"bv slice %d %d %d %d\n",aID,lower,upper, resultID);
	bv->slice(bv->getBV(internalBV(bv,aID)),lower,upper,bv->getBV(internalBV(bv,resultID)));
}

//Convert the specified bitvector, as well as any other bitvectors in its cone of influence, into pure CNF
void bv_bitblast(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID){
	S->cancelUntil(0);
	write_out(S,"bv bitblast %d\n",bvID);
	bv->bitblast(internalBV(bv,bvID));
}

//simple at-most-one constraint: asserts that at most one of the set of variables (NOT LITERALS) may be true.
//for small numbers of variables, consider using a direct CNF encoding instead
void at_most_one(Monosat::SimpSolver * S, int * vars, int n_vars){
	if(n_vars>1){
		write_out(S,"amo");
		for(int i = 0;i<n_vars;i++){
			write_out(S," %d",dimacs(S,mkLit(vars[i])));
		}

		write_out(S," 0\n");
		AMOTheory* amo = new  AMOTheory(S);
		for(int i = 0;i<n_vars;i++){
			Var v = internalVar(S,vars[i]);
			amo->addVar(v);
		}
	}
}

void flushPB(Monosat::SimpSolver * S){
	MonosatData *d = (MonosatData *) S->_external_data;

	if (d->pbsolver) {
		d->pbsolver->convert();
	}
}

const char * ineqToStr(PB::Ineq ineq){
	switch(ineq){
		case PB::LT:
			return "<";
		case PB::LEQ:
			return "<=";
		case PB::EQ:
			return "==";
		case PB::GEQ:
			return ">=";
		case PB::GT:
			return ">";
		default:
			return "!=";//not supported yet
	}
}

void assertPB(Monosat::SimpSolver * S, int _rhs, int n_args, int * literals, int * coefficients, PB::Ineq ineq){
	if(n_args>0) {

		MonosatData *d = (MonosatData *) S->_external_data;

		//could try to detect if the pb constraint is trivially false or true before creating the pbsolver here...
		write_out(S,"pb %s %d %d ", ineqToStr(ineq), _rhs, n_args);


		if (!d->pbsolver) {
			d->pbsolver = new PB::PbSolver(*S);
		}

		static vec<Lit> lits;
		lits.clear();
		for (int i = 0; i < n_args; i++) {
			Lit l = internalLit(S,literals[i]);
			lits.push(l);
			write_out(S,"%d ", dimacs(S,l));
		}
		write_out(S,"%d ", n_args);
		static vec<PB::Int> coefs;
		coefs.clear();
		for (int i = 0; i < n_args; i++) {
			coefs.push(PB::Int(coefficients[i]));
			write_out(S,"%d ", coefficients[i]);
		}
		PB::Int rhs = _rhs;

		write_out(S,"\n");

		d->pbsolver->addConstr(lits, coefs, rhs, ineq);
	}
}
void assertPB_lt(Monosat::SimpSolver * S, int _rhs, int n_args, int * literals, int * coefficients){
	assertPB(S,_rhs,n_args,literals,coefficients,PB::Ineq::LT);
}
void assertPB_leq(Monosat::SimpSolver * S, int _rhs, int n_args, int * literals, int * coefficients){
	assertPB(S,_rhs,n_args,literals,coefficients,PB::Ineq::LEQ);
}
void assertPB_eq(Monosat::SimpSolver * S, int _rhs, int n_args, int * literals, int * coefficients){
	assertPB(S,_rhs,n_args,literals,coefficients,PB::Ineq::EQ);
}
void assertPB_geq(Monosat::SimpSolver * S, int _rhs, int n_args, int * literals, int * coefficients){
	assertPB(S,_rhs,n_args,literals,coefficients,PB::Ineq::GEQ);
}
void assertPB_gt(Monosat::SimpSolver * S, int _rhs, int n_args, int * literals, int * coefficients){
	assertPB(S,_rhs,n_args,literals,coefficients,PB::Ineq::GT);
}
//theory interface for graphs

int newNode(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G){
	return newNode_Named(S,G,"");
}
int newNode_Named(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, const char * name){
	if(name != nullptr && strlen(name)>0 && G->hasNamedNode(name)) {
		throw std::invalid_argument("All nodes in a given graph must have unique names (or empty names).");
	}
	int n = G->newNode();
	write_out(S,"node %d %d", G->getGraphID(),n);
	if(name != nullptr && strlen(name)>0) {
		G->setNodeName(n, name);
		write_out(S," %s", name);
	}
	write_out(S,"\n");
	return n;
}
bool hasNamedNode(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, const char * name){
	if(name != nullptr && strlen(name)>0) {
		return G->hasNamedNode(name);
	}else{
		return false;
	}
}

const char * getNodeName(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, int node){
	return G->getNodeName(node).c_str();
}

int newEdge(Monosat::SimpSolver * S, Monosat::GraphTheorySolver<int64_t> *G,int from,int  to,  int64_t weight){
	Var v = newVar(S);
	Lit l =mkLit(v);

	write_out(S,"edge %d %d %d %d %" PRId64 "\n",G->getGraphID(),from,to, dimacs(S,l),weight);
	G->newEdge( from,  to, v,  weight );
	return externalLit(S,l);
}
int nNodes(Monosat::SimpSolver * S, Monosat::GraphTheorySolver<int64_t> *G){
	return G->nNodes();
}
int nEdges(Monosat::SimpSolver * S, Monosat::GraphTheorySolver<int64_t> *G){
	return G->nEdges();
}


int newEdge_double(Monosat::SimpSolver * S, Monosat::GraphTheorySolver<double> *G,int from,int  to,  double weight){
	Var v = newVar(S);
	Lit l =mkLit(v);
	write_out(S,"edge %d %d %d %d %f\n",G->getGraphID(),from,to, dimacs(S,l),weight);
	G->newEdge( from,  to, v,  weight );
	return externalLit(S,l);
}
int newEdge_bv(Monosat::SimpSolver * S, Monosat::GraphTheorySolver<int64_t> *G,int from,int  to, int bvID){
	Var v = newVar(S);
	Lit l =mkLit(v);
	write_out(S,"edge_bv %d %d %d %d %d\n",G->getGraphID(),from,to, dimacs(S,l),bvID);
	G->newEdgeBV( from,  to, v,  internalBV(S,bvID) );
	return externalLit(S,l);
}

int reaches(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to){
	if(G->hasReach(from,to,-1)){
		Lit l = G->reaches(from, to, var_Undef, -1);
		return externalLit(S, l);
	}else {
		Lit l = G->reaches(from, to, var_Undef, -1);
		write_out(S, "reach %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l));
		return externalLit(S, l);
	}
}
int reachesBackward(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to){
	if(G->hasReachBackward(from,to,-1)){
		Lit l = G->reachesBackward(from, to);
		return externalLit(S, l);
	}else {
		Lit l = G->reachesBackward(from, to);
		write_out(S, "reach_backward %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l));
		return externalLit(S, l);
	}
}
int onPath(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int nodeOnPath,int from, int to){
    if(G->hasOnPath(nodeOnPath,from,to)){
        Lit l = G->onPath(nodeOnPath, from, to);
        return externalLit(S, l);
    }else {
        Lit l = G->onPath(nodeOnPath, from, to);
        write_out(S, "on_path %d %d %d %d %d\n", G->getGraphID(), nodeOnPath, from, to, dimacs(S, l));
        return externalLit(S, l);
    }
}
int shortestPathUnweighted_lt_const(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to, int steps){
    if(G->hasReach(from,to,steps-1)){
        Lit l = G->reaches(from, to, var_Undef, steps - 1);
        return externalLit(S, l);
    }else {
        Lit l = G->reaches(from, to, var_Undef, steps - 1);
        write_out(S, "distance_lt %d %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l), steps);
        return externalLit(S, l);
    }
}


int shortestPathUnweighted_lt_bv(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to, int bvID){
    if(G->hasDistanceBV(from,to,internalBV(S,bvID),false)){
        Lit l = G->distanceBV(from, to, internalBV(S, bvID), false);
        return externalLit(S, l);
    }else {
        Lit l = G->distanceBV(from, to, internalBV(S, bvID), false);
        write_out(S, "distance_bv_lt %d %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l), bvID);
        return externalLit(S, l);
    }
}
int shortestPathUnweighted_leq_const(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to, int steps){
    if(G->hasReach(from,to,steps)){
        Lit l = G->reaches(from, to, var_Undef, steps);
        return externalLit(S, l);
    }else {
        Lit l = G->reaches(from, to, var_Undef, steps);
        write_out(S, "distance_leq %d %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l), steps);
        return externalLit(S, l);
    }
}
int shortestPath_lt_const(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to, int64_t dist){
    if(G->hasDistance(from,to,dist,false)){
        Lit l = G->distance(from, to, dist, false);
        return externalLit(S, l);
    }else {
        Lit l = G->distance(from, to, dist, false);
        write_out(S, "weighted_distance_lt %d %d %d %d %" PRId64 "\n", G->getGraphID(), from, to, dimacs(S, l), dist);
        return externalLit(S, l);
    }
}
int shortestPath_leq_const(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to, int64_t dist){
    if(G->hasDistance(from,to,dist,true)) {
        Lit l = G->distance(from, to, dist, true);
        return externalLit(S, l);
    }else{
        Lit l = G->distance(from, to, dist, true);
        write_out(S, "weighted_distance_leq %d %d %d %d %" PRId64 "\n", G->getGraphID(), from, to, dimacs(S, l), dist);
        return externalLit(S, l);
    }
}

int shortestPath_lt_bv(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to, int bvID){
    if(G->hasDistanceBV(from,to,internalBV(S,bvID),false)){
        Lit l = G->distanceBV(from, to, internalBV(S, bvID), false);
        write_out(S, "weighted_distance_bv_lt %d %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l), bvID);
        return externalLit(S, l);
    }else {
        Lit l = G->distanceBV(from, to, internalBV(S, bvID), false);
        write_out(S, "weighted_distance_bv_lt %d %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l), bvID);
        return externalLit(S, l);
    }
}
int shortestPath_leq_bv(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int from, int to, int bvID){
    if(G->hasDistanceBV(from,to,internalBV(S,bvID),true)){
        Lit l = G->distanceBV(from, to, internalBV(S, bvID), true);
        return externalLit(S, l);
    }else {
        Lit l = G->distanceBV(from, to, internalBV(S, bvID), true);
        write_out(S, "weighted_distance_bv_leq %d %d %d %d %d\n", G->getGraphID(), from, to, dimacs(S, l), bvID);
        return externalLit(S, l);
    }
}
int maximumFlow_geq(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int source, int sink, int64_t weight){
	if(G->hasMaxflow(source,sink,weight,true)){
		Lit l = G->maxflow(source, sink, weight,true);
		return externalLit(S,l);
	}else{
		Lit l = G->maxflow(source, sink, weight,true);
		write_out(S,"maximum_flow_geq %d %d %d %d %" PRId64 "\n",G->getGraphID(),source,sink, dimacs(S,l),weight);
		return externalLit(S,l);
	}

}
int maximumFlow_gt(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int source, int sink, int64_t weight){
	if(G->hasMaxflow(source,sink,weight,false)){
		Lit l = G->maxflow(source, sink, weight, false);
		return externalLit(S, l);
	}else {
		Lit l = G->maxflow(source, sink, weight, false);
		write_out(S, "maximum_flow_gt %d %d %d %d %" PRId64 "\n", G->getGraphID(), source, sink, dimacs(S, l), weight);
		return externalLit(S, l);
	}
}
int maximumFlow_geq_bv(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int source, int sink, int bvID){
    if(G->hasMaxflowBV(source,sink,internalBV(S,bvID),true)){
        Lit l = G->maxflowBV(source, sink,  internalBV(S,bvID),true);
        return externalLit(S,l);
    }else{
        Lit l = G->maxflowBV(source, sink,  internalBV(S,bvID),true);
        write_out(S,"maximum_flow_bv_geq %d %d %d %d %d\n",G->getGraphID(),source,sink, dimacs(S,l),bvID);
        return externalLit(S,l);
    }

}
int maximumFlow_gt_bv(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int source, int sink, int bvID){
    if(G->hasMaxflowBV(source,sink,internalBV(S,bvID),false)){
        Lit l = G->maxflowBV(source, sink,  internalBV(S,bvID),false);
        return externalLit(S,l);
    }else {
        Lit l = G->maxflowBV(source, sink, internalBV(S, bvID), false);
        write_out(S, "maximum_flow_bv_gt %d %d %d %d %d\n", G->getGraphID(), source, sink, dimacs(S, l), bvID);
        return externalLit(S, l);
    }
}
int minimumSpanningTree_leq(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, int64_t weight){
	Var v = newVar(S);
	Lit l =mkLit(v);
	write_out(S,"mst_weight_leq %d %d %d %d %d %" PRId64 "\n",G->getGraphID(), dimacs(S,l),weight);
	G->minimumSpanningTree(v, weight,true);

	return externalLit(S,l);
}
int minimumSpanningTree_lt(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, int64_t weight){
	Var v = newVar(S);
	Lit l =mkLit(v);
	write_out(S,"mst_weight_lt  %d %d %d %d %d %" PRId64 "\n",G->getGraphID(), dimacs(S,l),weight);
	G->minimumSpanningTree(v, weight,false);

	return externalLit(S,l);
}
int acyclic_undirected(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G){
    if(G->hasAcyclic(false)){
		Lit l = G->acyclic(var_Undef, false);
		return externalLit(S, l);
    }else {
		Lit l = G->acyclic(var_Undef, false);
		write_out(S, "forest %d %d \n", G->getGraphID(), dimacs(S, l));
		return externalLit(S, l);
	}
}
int acyclic_directed(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G){
	if(G->hasAcyclic(true)){
		Lit l = G->acyclic(var_Undef, true);
		return externalLit(S, l);
	}else {
		Lit l = G->acyclic(var_Undef, true);
		write_out(S, "acyclic %d %d \n", G->getGraphID(), dimacs(S, l));
		return externalLit(S, l);
	}
}


void newEdgeSet(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int * edges, int n_edges, bool enforceEdgeAssignment){
	static vec<int> edge_set;
	edge_set.clear();
	write_out(S,"edge_set %d %d", G->getGraphID(), n_edges);
	for (int i = 0;i<n_edges;i++){
		Var outer=var(internalLit(S,edges[i]));
		write_out(S," %d",dimacs(S,mkLit(outer)));
		if(outer>=S->nVars()){
			api_errorf("Bad edge set variable %d",outer+1);
		}
		if(!S->hasTheory(outer)){
			api_errorf("Bad edge set variable %d",outer+1);
		}
		if(S->getTheoryID(outer)!=G->getTheoryIndex()){
			api_errorf("Wrong graph (%d) for variable %d",G->getTheoryIndex(),outer+1);
		}
		Var v = S->getTheoryVar(outer);
		if(!G->isEdgeVar(v)){
			api_errorf("Variable %d is not an edge variable",outer+1);
		}
		edge_set.push(G->getEdgeID(v));
	}
	write_out(S,"\n");

	static vec<Lit> edge_lits;
	edge_lits.clear();
	for(int edgeID:edge_set){
		edge_lits.push(mkLit(G->toSolver(G->getEdgeVar(edgeID))));
	}
	//enforce that _exactly_ one edge from this edge set is assigned in the SAT solver
	if(enforceEdgeAssignment) {
		S->addClause(edge_lits);
		AMOTheory *amo = new AMOTheory(S);
		for (Lit l:edge_lits) {
			Var v = S->newVar();
			G->makeEqualInSolver(mkLit(v), l);
			amo->addVar(v);
		}
	}
}

void graph_setAssignEdgesToWeight(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, int64_t weight){
    write_out(S,"graph_assign_edges_to_weight %d %" PRId64 "\n", G->getGraphID(),weight);
    G->setAssignEdgesToWeight(weight);
}

//flow routing interface

Monosat::FlowRouter<int64_t> * createFlowRouting(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, int sourceNode,int destNode,int maxflowLit){
    FlowRouter<int64_t>  * r = new FlowRouter<int64_t>(S,G,sourceNode,destNode,internalLit(S,maxflowLit));
    write_out(S,"f_router %d %d %d %d %d\n", G->getGraphID(),  r->getRouterID(), sourceNode, destNode, dimacs(S,internalLit(S,maxflowLit)));
    return r;
}

void addRoutingNet(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G, Monosat::FlowRouter<int64_t> * router, int disabledEdge, int n_members, int * edge_lits, int * reach_lits){
    vec<Lit> dest_edge_lits;
	vec<Lit> net_reach_lits;
    write_out(S,"f_router_net %d %d %d %d", G->getGraphID(), router->getRouterID(), dimacs(S,internalLit(S,disabledEdge)), n_members);
    for(int i = 0;i<n_members;i++){
		dest_edge_lits.push(internalLit(S,edge_lits[i]));
		net_reach_lits.push(internalLit(S,reach_lits[i]));
        write_out(S," %d %d",dimacs(S,internalLit(S,edge_lits[i])),dimacs(S,internalLit(S,reach_lits[i])));
    }
    write_out(S,"\n");
    router->addNet(internalLit(S,disabledEdge),dest_edge_lits,net_reach_lits);
}



//FSM Interface

Monosat::FSMTheorySolver * initFSMTheory(Monosat::SimpSolver * S){
	MonosatData * d = (MonosatData*) S->_external_data;
	if(d->fsm_theory)
		return d->fsm_theory;

	Monosat::FSMTheorySolver  * theory = new Monosat::FSMTheorySolver(S);

	d->fsm_theory=theory;
	return theory;
}
int newFSM(Monosat::SimpSolver * S, Monosat::FSMTheorySolver *  fsmTheory, int inputAlphabet, int outputAlphabet){
	if(!fsmTheory){
		fsmTheory = initFSMTheory(S);
	}

	int fsmID = fsmTheory->newFSM();
	fsmTheory->setAlphabets(fsmID,inputAlphabet,outputAlphabet);
	write_out(S,"fsm %d 0 0\n", fsmID);

	return fsmID;
}
int newState(Monosat::SimpSolver * S, Monosat::FSMTheorySolver *  fsmTheory, int fsmID){
	if(!fsmTheory){
		fsmTheory = initFSMTheory(S);
	}
	return fsmTheory->newNode(fsmID);
}

int newTransition(Monosat::SimpSolver * S, Monosat::FSMTheorySolver * fsmTheory, int fsmID, int fromNode, int toNode,int inputLabel, int outputLabel){
	if(!fsmTheory){
		fsmTheory = initFSMTheory(S);
	}
	Var v = newVar(S);
	Lit l =mkLit(v);
	fsmTheory->newTransition(fsmID,fromNode,toNode,inputLabel,outputLabel,v);
	write_out(S,"transition %d %d %d %d %d %d\n", fsmID,fromNode,toNode,inputLabel,outputLabel,dimacs(S,l));
	return externalLit(S,l);
}
int newString(Monosat::SimpSolver * S, Monosat::FSMTheorySolver *  fsmTheory, int * str,int len){
	if(!fsmTheory){
		fsmTheory = initFSMTheory(S);
	}

	vec<int> string;
	for(int i = 0;i<len;i++){
		int label = str[i];
		if(label<=0){
			api_errorf("String must consist of positive integers, found %d at position %d in string %d",label,i,fsmTheory->nStrings());
		}
		string.push(label);
	}
	int strID = fsmTheory->newString(string);

	write_out(S,"str %d",strID);
	for(int i = 0;i<len;i++) {
		int label = str[i];
		write_out(S," %d",label);
	}
	write_out(S,"\n");
	return strID;
}
int fsmAcceptsString(Monosat::SimpSolver * S, Monosat::FSMTheorySolver *  fsmTheory, int fsmID, int startNode, int acceptNode,int stringID){
	if(!fsmTheory){
		fsmTheory = initFSMTheory(S);
	}

	Var v = newVar(S);
	Lit l =mkLit(v);
	fsmTheory->addAcceptLit(fsmID,startNode,acceptNode,stringID,v);
	write_out(S,"accepts %d %d %d %d %d\n",fsmID,startNode, acceptNode, stringID, dimacs(S,l));
	return externalLit(S,l);
}
int fsmCompositionAccepts(Monosat::SimpSolver * S, Monosat::FSMTheorySolver *  fsmTheory,   int fsmGeneratorID,int fsmAcceptorID, int gen_startNode, int gen_acceptNode, int acceptor_startNode, int acceptor_acceptNode,int stringID){
	if(!fsmTheory){
		fsmTheory = initFSMTheory(S);
	}
	Var v = newVar(S);
	Lit l =mkLit(v);
	fsmTheory->addComposeAcceptLit(fsmGeneratorID,fsmAcceptorID,gen_startNode,gen_acceptNode,acceptor_startNode,acceptor_acceptNode, stringID,v);
	write_out(S,"accepts_composition %d %d %d %d %d %d %d %d\n",fsmGeneratorID,fsmAcceptorID,gen_startNode,gen_acceptNode,acceptor_startNode,acceptor_acceptNode, stringID, dimacs(S,l));
	return externalLit(S,l);
}

//model query
//Returns true if the solver has a model (in which case it is safe to query the model), false otherwise
bool hasModel(Monosat::SimpSolver * S){
	return S->hasModel();
}

//Returns 0 for true, 1 for false, 2 for unassigned.
int getModel_Literal(Monosat::SimpSolver * S,int lit){
	Lit l = internalLit(S,lit);

    if(var(l)<0 || var(l)>=S->nVars())
		api_errorf("Variable %d is undefined",dimacs(S,l));
    else if (var(l) >= S->model.size()){
        return toInt(l_Undef);
    }
	//return toInt(l_Undef);
	lbool val = S->model[var(l)];
	assert(val==l_True || val==l_False || val==l_Undef);
	if (sign(l)){
		if (val==l_True)
			val=l_False;
		else if (val==l_False){
			val=l_True;
		}
	}
	return toInt(val);
}
//Returns 0 for true, 1 for false, 2 for unassigned, at level 0.
int getConstantModel_Literal(Monosat::SimpSolver * S,int lit){
    Lit l = internalLit(S,lit);

    if(var(l)<0 || var(l)>=S->nVars())
        api_errorf("Variable %d is undefined",dimacs(S,l));

    if(!S->isConstant(var(l)))
        return toInt(l_Undef);

    lbool val = S->value(l);
    assert(val==l_True || val==l_False || val==l_Undef);
    return toInt(val);
}

int64_t getModel_BV(Monosat::SimpSolver * S, Monosat::BVTheorySolver<int64_t> * bv, int bvID, bool getMaximumValue){
	if(getMaximumValue){
		return bv->getOverApprox( internalBV(S,bvID));
	}else{
		return bv->getUnderApprox( internalBV(S,bvID));
	}

}
//graph queries:
int getModel_Path_Nodes_Length(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int reach_or_distance_literal){
	Lit l = internalLit(S,reach_or_distance_literal);
	std::vector<int> store_path;
	if(! G->getModel_Path(l,store_path)){
		return -1;
	}else{
		return store_path.size();
	}
}
int getModel_Path_Nodes(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int reach_or_distance_literal, int store_length, int * store){
	Lit l = internalLit(S,reach_or_distance_literal);
	std::vector<int> store_path;
	if(! G->getModel_Path(l,store_path)){
		return -1;
	}else if (store_length<store_path.size()) {
		return store_path.size();
	}else{
		for(int i = 0;i<store_path.size();i++){
			store[i]=store_path[i];
		}
		return store_path.size();
	}
}
int getModel_Path_EdgeLits_Length(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int reach_or_distance_literal){
	Lit l = internalLit(S,reach_or_distance_literal);
	std::vector<Lit> store_path;
	if(! G->getModel_PathByEdgeLit(l,store_path)){
		return -1;
	}else{
		return store_path.size();
	}
}
int getModel_Path_EdgeLits(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int reach_or_distance_literal, int store_length, int * store){
	Lit l = internalLit(S,reach_or_distance_literal);
	std::vector<Lit> store_path;
	if(! G->getModel_PathByEdgeLit(l,store_path)){
		return -1;
	}else if (store_length<store_path.size()) {
		return store_path.size();
	}else{
		for(int i = 0;i<store_path.size();i++){
			store[i]=externalLit(S,store_path[i]);
		}
		return store_path.size();
	}
}
int64_t getModel_MaxFlow(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int maxflow_literal){
	Lit l = internalLit(S,maxflow_literal);
	G->checkGraphLit(l,false);
	return G->getModel_MaximumFlow(S->getTheoryLit(l));
}
int64_t getModel_EdgeFlow(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int maxflow_literal, int edge_assignment_literal){
	Lit l = internalLit(S,maxflow_literal);
	Lit e = internalLit(S,edge_assignment_literal);
	G->checkGraphLit(l,false);
	G->checkGraphLit(e,true);
	return G->getModel_MaximumFlow_EdgeFlow(S->getTheoryLit(l),S->getTheoryLit(e));
}
int64_t getModel_AcyclicEdgeFlow(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int maxflow_literal, int edge_assignment_literal){
	Lit l = internalLit(S,maxflow_literal);
	Lit e = internalLit(S,edge_assignment_literal);
	G->checkGraphLit(l,false);
	G->checkGraphLit(e,true);
	return G->getModel_MaximumFlow_AcyclicEdgeFlow(S->getTheoryLit(l),S->getTheoryLit(e));
}

int64_t getModel_MinimumSpanningTreeWeight(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int spanning_tree_literal){
	Lit l = internalLit(S,spanning_tree_literal);
	G->checkGraphLit(l,false);
	return G->getModel_MinimumSpanningWeight(S->getTheoryLit(l));
}
/* //Get the length of a valid path (from a reachability or shortest path constraint)
 int64_t getModel_PathLength(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int reach_or_shortest_path_lit){
	 Lit l = internalLit(S,reach_or_shortest_path_lit);
	 return G->getModel_PathLength(S->getTheoryLit(l));
 }
 //Get a valid path (from a reachability or shortest path constraint)
 //store_path must point to an array of ints of sufficient length to store the path (the path length can be optained by a call to getModel_PathLength)
 void getModel_Path(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int reach_or_shortest_path_lit, int * store_path){
	 Lit l = internalLit(S,reach_or_shortest_path_lit);
	 std::vector<int> path;
	 G->getModel_Path(S->getTheoryLit(l),path);
	 for (int i = 0;i<path->size();i++){
		 store_path[i]=path[i];
	 }
	 //Returns the number of nodes in the path length for this reachability or shortest path literal (1+number of edges)
	   int getModel_PathLength(Monosat::SimpSolver * S,Monosat::GraphTheorySolver<int64_t> *G,int reach_or_shortest_path_lit){

	   }

		bool getModel_Path(Lit theoryLit,  int * store_path){

		 }
		 //Get a valid path, in terms of edges, (from a reachability or shortest path constraint)
		 //store_path must point to an array of ints of sufficient length to store the path (the path length can be optained by a call to getModel_PathLength)
		//Or, return false if there is no such path
		bool getModel_PathByEdgeLit(Lit theoryLit, int * store_path){

		}
 }

*/
