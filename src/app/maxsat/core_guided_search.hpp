# pragma once
#include "rustsat.h"
#include "app/maxsat/maxsat_instance.hpp"
#include <climits>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>

class CoreGuidedSearch {
public:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;
    MaxSatInstance& _global_instance; // used to calculate costs of solutions
    std::unique_ptr<IncSatController> _stream_wrapper;

    MaxSatInstance _instance; // The working instance (may be incomplete)
    int _formula_pos {0}; // lits already added to _stream_wrapper
    std::vector<std::pair<std::vector<int>, size_t>> _cores {};
    int _last_search_assumption_count;

    std::atomic<size_t> _encoded_cost {0};

    std::atomic<size_t> _best_global_cost { ULONG_MAX };
    std::vector<int> _best_global_solution;
    std::mutex _best_global_mutex {};

    std::atomic<size_t> _new_reformulated_available { false };
    size_t _last_reformulated_at { 0 };
    std::unique_ptr<MaxSatInstance> _reformulated_instance;
    std::mutex _reformulated_instance_mutex {};

    size_t _objective_sum { 0 };
    size_t _core_size_sum { 0 };
    size_t _core_size_count { 0 };

    bool _interrupt = false;

    CoreGuidedSearch(const Parameters& params, APIConnector& api, JobDescription& desc, DTaskTracker& tracker, MaxSatInstance& instance, int update_layer):
        _params(params), _api(api), _desc(desc), _global_instance(instance),
        _stream_wrapper(std::make_unique<IncSatController>(params, api, desc, tracker)),
        _instance(instance.formula.data(), instance.formula.size())  {

        _stream_wrapper->initInteractiveSolving(params.maxSatCoreGuidedDistributed());
        if (params.maxSatCoreGuidedDistributed()) {
            //_stream_wrapper->getMallobProcessor()->setGroupId("consistent-logic-" + std::to_string(update_layer), 1, instance.nbVars);
            _stream_wrapper->getMallobProcessor()->setInitialSize(instance.nbVars, _desc.getAppConfiguration().fixedSizeEntryToInt("__NC"));
        }

        _instance.preprocessLayer = instance.preprocessLayer;
        _instance.objective = instance.objective;
        _instance.nbVars = instance.nbVars;
        _instance.encodedCost = 0;
        _instance.bestCost = ULONG_MAX;
        _objective_sum = instance.sumOfWeights;

        if (instance.bestCost == ULONG_MAX) { // Solve to get initial UB
            _last_search_assumption_count = 1; // prevent thinking this is a solution
            bool ok = _stream_wrapper->solveNextRevisionNonblocking(
                std::vector<int>(_instance.formula.begin() + _formula_pos, _instance.formula.end()),
                std::vector<int>(0)
            );
            assert(ok);
            _formula_pos = _instance.formula.size();
            while (!_interrupt && _stream_wrapper->getStream().isNonblockingSolvePending()) {
                //usleep
            }
            if (!_interrupt) {
                processSolveResult(_stream_wrapper->getStream().getNonblockingSolveResult());
            } else {
                _stream_wrapper->getStream().interrupt();
                _stream_wrapper->finalize();
            }
        }

        if (_params.maxSatCoreGuidedHeuristic() != 0) { // not plain core guided
            saveReformulatedInstance(params.maxSatIntervalSkew());
        }
    }

    ~CoreGuidedSearch() {
        LOG(V2_INFO, "CG: Destructed core-guided search\n");
    }

    size_t getEncodedCost() const {
        return _encoded_cost;
    }

    size_t getBestGlobalCost() const {
        return _best_global_cost;
    }

    std::unique_ptr<MaxSatInstance> getReformulatedInstance() {
        std::lock_guard<std::mutex> lock(_reformulated_instance_mutex);
        assert(_new_reformulated_available);
        _new_reformulated_available = false;
        return std::move(_reformulated_instance);
    }

    std::vector<int> getBestGlobalSolution() {
        std::lock_guard<std::mutex> lock(_best_global_mutex);
        return std::move(_best_global_solution);
    }
    
    bool processSolveResult(std::pair<int, std::vector<int>> result) {
        auto& [resultCode, solution] = result;
        if (resultCode == RESULT_SAT) {
            auto global_cost = _global_instance.getCostOfModel(solution);
            if (global_cost < _best_global_cost) {
                std::lock_guard<std::mutex> lock(_best_global_mutex);
                _best_global_solution = solution;
                _best_global_cost = global_cost;
                _best_global_solution.resize(_global_instance.nbVars+1);
            }
            LOG(V4_VVER, "CG: SAT, cost %lu\n", global_cost);
            relaxCores();
        } else if (resultCode == RESULT_UNSAT) {
            auto weight = processCore(solution);
            _encoded_cost += weight;
            _instance.encodedCost += weight;
            _cores.push_back({solution, weight});
            _core_size_sum += solution.size();
            _core_size_count += 1;
            LOG(V4_VVER, "CG: UNSAT, core size %d\n", solution.size());
        }

        bool need_reformulation = false;
        if (_params.maxSatCoreGuidedHeuristic() == 1) { // every core
            need_reformulation = (resultCode == RESULT_UNSAT);
        } else if (_params.maxSatCoreGuidedHeuristic() == 2) { // every WCE round
            need_reformulation = (resultCode == RESULT_SAT);
        } else if (_params.maxSatCoreGuidedHeuristic() == 3) { // gap improvement
            auto gap = _global_instance.upperBound - _global_instance.lowerBound;
            if (_encoded_cost > _last_reformulated_at + 0.01*_params.maxSatCoreGuidedThreshold()*gap) {
                need_reformulation = true;
            }
        } else if (_params.maxSatCoreGuidedHeuristic() == 4) { //constant time
            static auto last_time = std::chrono::high_resolution_clock::now();
            auto now = std::chrono::high_resolution_clock::now();
            auto duration_ms = (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time)).count();
            if (duration_ms > 1000 * _params.maxSatCoreGuidedThreshold()) {
                last_time = now;
                need_reformulation = true;
            }
        }

        if (need_reformulation) {
            relaxCores();
            saveReformulatedInstance(_params.maxSatIntervalSkew());
        }
        return need_reformulation;
    }

    void run() {
        std::thread([this]() {
            auto _start_time = std::chrono::high_resolution_clock::now();
            while (_encoded_cost < _best_global_cost && !_interrupt) {
                auto assumptions = getObjectiveAssumptions();
                _last_search_assumption_count = assumptions.size();
                bool ok = _stream_wrapper->solveNextRevisionNonblocking(
                    std::vector<int>(_instance.formula.begin() + _formula_pos, _instance.formula.end()),
                    std::move(assumptions)
                );
                assert(ok);
                _formula_pos = _instance.formula.size();

                
                while (!_interrupt && _stream_wrapper->getStream().isNonblockingSolvePending()) {
                    //usleep
                }

                if (!_interrupt) {
                    processSolveResult(_stream_wrapper->getStream().getNonblockingSolveResult());
                }

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - _start_time).count();
                if (_interrupt || _params.maxSatCoreGuidedTimeout() && elapsed > _params.maxSatCoreGuidedTimeout()) {
                    LOG(V2_INFO, "Terminating core-guided search\n");
                    _stream_wrapper->getStream().interrupt();
                    _stream_wrapper->finalize();
                    break;
                }
            }
        }).detach();
    }

private:
    std::vector<int> getObjectiveAssumptions() const {
        std::vector<int> assumptions{};
        for (const auto& term : _instance.objective) {
            if (term.factor > 0) {
                assumptions.push_back(-term.lit);
            }
        }
        return assumptions;
    }

    size_t processCore(const std::vector<int>& lits) {
        size_t weight = ULONG_MAX;
        for (const auto& lit : lits) {
            for (const auto& term : _instance.objective) {
                if (lit == -term.lit) {
                    weight = std::min(weight, term.factor);
                }
            }
        }
        assert(weight != ULONG_MAX);

        for (const auto& lit : lits) {
            for (int i = 0; i < _instance.objective.size(); i++) {
                if (lit == -_instance.objective[i].lit) {
                    _instance.objective[i].factor -= weight;
                    _objective_sum -= weight;
                    break;
                }
            }
        }
        return weight;
    }

    void relaxCoreNaive(const std::vector<int>& coreLits, size_t weight) {
        int N = coreLits.size();
        std::vector<int> os(N-1);
        for (int i = 0; i < os.size(); i++) {
            os[i] = ++_instance.nbVars;
            _instance.objective.push_back({weight, os[i]});
            _objective_sum += weight;
        }

        for (int K = 2; K <= N; K++) {
            std::vector<int> bits(N,0);
            std::fill(bits.begin(), bits.begin() + K, 1);
            do {
                for (int i = 0; i < N; ++i) {
                    if (bits[i]) {
                        _instance.formula.push_back(coreLits[i]);
                    }
                }
                _instance.formula.push_back(os[K-2]);
                _instance.formula.push_back(0);
            } while (std::prev_permutation(bits.begin(), bits.end()));
        }
    }

    static void static_add_literal(int lit, void* inst) {
        ((CoreGuidedSearch*)inst)->_instance.formula.push_back(lit);
    }

    void relaxCoreOLL(std::vector<int>& coreLits, size_t weight) {
        int N = coreLits.size();
        if (N < 2) return;
        
        RustSAT::DbTotalizer* enc = RustSAT::tot_new();
        for (auto& lit : coreLits) {
            RustSAT::tot_add(enc, -lit);
        }

        uint32_t varsUsed = _instance.nbVars;
        RustSAT::tot_encode_ub(enc, 1, N-1, &varsUsed, static_add_literal, this);
        _instance.nbVars = varsUsed;

        for (int K = 2; K <= N; K++) {
            int relax_lit = 0;
            assert(RustSAT::tot_enforce_ub(enc, K-1, &relax_lit) == RustSAT::MaybeError::Ok);
            _instance.objective.push_back({weight, -relax_lit});
            _objective_sum += weight;
        }

        RustSAT::tot_drop(enc);
    }

    void relaxCorePMRES(const std::vector<int>& bs, size_t weight) {
        int N = bs.size();
        std::vector<int> os(N-1);
        std::vector<int> ds(N-1);
        for (int i = 0; i < N-1; i++) {
            os[i] = ++_instance.nbVars;
            ds[i] = ++_instance.nbVars;
            _instance.objective.push_back({weight, os[i]});
            _objective_sum += weight;
        }

        for (int i = 0; i < N-1; i++) {
            if (i != N-2) { // d1 <-> b2 v d2
                _instance.formula.push_back(-ds[i]);
                _instance.formula.push_back(-bs[i+1]); //polarity!
                _instance.formula.push_back(ds[i+1]);
                _instance.formula.push_back(0); // d1 -> b2 v d2
                _instance.formula.push_back(bs[i+1]); //polarity!
                _instance.formula.push_back(ds[i]);
                _instance.formula.push_back(0); // b2 -> d1
                _instance.formula.push_back(-ds[i+1]);
                _instance.formula.push_back(ds[i]);
                _instance.formula.push_back(0); // d2 -> d1
            } else { // d3 <-> b4
                _instance.formula.push_back(-ds[i]);
                _instance.formula.push_back(-bs[i+1]); //polarity!
                _instance.formula.push_back(0); // b3 -> d4
                _instance.formula.push_back(bs[i+1]); //polarity!
                _instance.formula.push_back(ds[i]);
                _instance.formula.push_back(0); // d4 -> b3
            }

            _instance.formula.push_back(bs[i]); //polarity!
            _instance.formula.push_back(-ds[i]);
            _instance.formula.push_back(os[i]);
            _instance.formula.push_back(0); // b1 ^ d1 -> o1
        }
    }

    void relaxCores() {
        for (auto& [lits, weight] : _cores) {
            if (_params.maxSatCoreGuidedRelax() == 0) {
                relaxCoreNaive(lits, weight);
            } else if (_params.maxSatCoreGuidedRelax() == 1) {
                relaxCoreOLL(lits, weight);
            } else if (_params.maxSatCoreGuidedRelax() == 2) {
                relaxCorePMRES(lits, weight);
            }
        }
        LOG(V2_INFO, "CG: Relaxed %d cores\n", _cores.size());
        _cores.clear();

        std::sort(_instance.objective.begin(), _instance.objective.end(),
            [&](const MaxSatInstance::ObjectiveTerm& termLeft, const MaxSatInstance::ObjectiveTerm& termRight) {
            return termLeft.factor < termRight.factor;
        });

        _instance.objective.erase(
            std::remove_if(
                _instance.objective.begin(),
                _instance.objective.end(),
                [](const MaxSatInstance::ObjectiveTerm& term) {
                    return term.factor == 0;
                }
            ),
            _instance.objective.end()
        );

        LOG(V2_INFO, "CG: Formula size is now %d, objective size is %d and sum %lu. Avg core size has been %.2f\n", 
            _instance.formula.size(), _instance.objective.size(), _objective_sum,
            (_core_size_count == 0 ? 0 : ((float)_core_size_sum / _core_size_count))
        );
    }

    void saveReformulatedInstance(float intervalSkew) {
        assert(_cores.empty());
        std::lock_guard<std::mutex> lock(_reformulated_instance_mutex);
        _last_reformulated_at = _encoded_cost;

        _reformulated_instance = std::make_unique<MaxSatInstance>(_instance.formula.data(), _instance.formula.size());
        _reformulated_instance->preprocessLayer = _instance.preprocessLayer;
        
        _reformulated_instance->nbVars = _instance.nbVars;
        _reformulated_instance->sumOfWeights = 0;
        tsl::robin_set<size_t> uniqueFactors;
        for (auto term : _instance.objective) {
            if (term.factor > 0) {
                _reformulated_instance->objective.push_back(term);
                _reformulated_instance->sumOfWeights += term.factor;
                uniqueFactors.insert(term.factor);
            }
        }
        _reformulated_instance->nbUniqueWeights = uniqueFactors.size();
        std::sort(_reformulated_instance->objective.begin(), _reformulated_instance->objective.end(),
            [&](const MaxSatInstance::ObjectiveTerm& termLeft, const MaxSatInstance::ObjectiveTerm& termRight) {
            return termLeft.factor < termRight.factor;
        });

        _reformulated_instance->lowerBound = 0;
        _reformulated_instance->upperBound = _reformulated_instance->sumOfWeights;
        _reformulated_instance->encodedCost = _instance.encodedCost;
        _reformulated_instance->bestCost = ULONG_MAX;

        _reformulated_instance->intervalSearch = std::make_unique<IntervalSearch>(intervalSkew);
        _new_reformulated_available = true;
    }
};