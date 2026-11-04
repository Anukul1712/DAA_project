#pragma once
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "dag_generator.h"
#include "task_scheduler.h"

namespace iov::scheduling {

// Where new DAGs come from.
class IDagSource {
public:
    virtual ~IDagSource() = default;
    virtual std::string name() const = 0;
    virtual Dag make(int dag_id, double arrival_s, const std::string& owner, uint64_t& next_task_id,
                     std::mt19937& rng) = 0;
    // Expected total work of one DAG (Mcycles); used to turn a target
    // utilisation (--load) into an arrival rate.
    virtual double meanDagWorkMcycles() const = 0;
};

// Random DAGs from dag_generator.h.
class SyntheticDagSource : public IDagSource {
public:
    explicit SyntheticDagSource(DagGeneratorParams p = {}) : p_(p) {}
    std::string name() const override { return "synthetic"; }
    Dag make(int dag_id, double arrival_s, const std::string& owner, uint64_t& next_task_id,
             std::mt19937& rng) override {
        return makeRandomDag(dag_id, arrival_s, owner, next_task_id, rng, p_);
    }
    double meanDagWorkMcycles() const override {
        double tasks = (p_.min_tasks + p_.max_tasks) / 2.0;
        double work = (p_.min_workload + p_.max_workload) / 2.0;
        return tasks * work;
    }

private:
    DagGeneratorParams p_;
};

// Real DAG structures read from a CSV made by data/prepare_wfcommons.py.
// Columns: workflow,task,parents,workload_mcycles,data_kb[,...]
// Tasks of a workflow must be listed in topological order (task = 0..n-1).
// Each arrival picks one workflow uniformly at random.
class TraceDagSource : public IDagSource {
public:
    explicit TraceDagSource(const std::string& csv_path) {
        std::ifstream in(csv_path);
        if (!in)
            throw std::runtime_error("cannot open " + csv_path +
                                     " -- run `python data/prepare_wfcommons.py` first");
        std::string line;
        std::getline(in, line);  // header
        std::map<std::string, size_t> index;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto f = split(line, ',');
            if (f.size() < 5) throw std::runtime_error("bad line in " + csv_path + ": " + line);
            auto it = index.find(f[0]);
            if (it == index.end()) {
                it = index.emplace(f[0], templates_.size()).first;
                templates_.push_back({});
            }
            auto& wf = templates_[it->second];
            Proto t;
            t.index = std::stoi(f[1]);
            if (t.index != static_cast<int>(wf.size()))
                throw std::runtime_error("tasks of " + f[0] + " are not numbered 0..n-1 in order");
            if (!f[2].empty())
                for (const auto& p : split(f[2], ';')) {
                    int pi = std::stoi(p);
                    if (pi >= t.index) throw std::runtime_error("parent after child in " + f[0]);
                    t.parents.push_back(pi);
                }
            t.work = std::stod(f[3]);
            t.data = std::stod(f[4]);
            wf.push_back(t);
        }
        if (templates_.empty()) throw std::runtime_error(csv_path + " has no workflows");
        double total = 0.0;
        for (const auto& wf : templates_)
            for (const auto& t : wf) total += t.work;
        mean_work_ = total / static_cast<double>(templates_.size());
    }

    std::string name() const override { return "wfcommons"; }

    Dag make(int dag_id, double arrival_s, const std::string& owner, uint64_t& next_task_id,
             std::mt19937& rng) override {
        std::uniform_int_distribution<size_t> pick(0, templates_.size() - 1);
        const auto& wf = templates_[pick(rng)];
        Dag d;
        d.id = dag_id;
        d.arrival_s = arrival_s;
        d.owner_vehicle = owner;
        uint64_t base = next_task_id;
        next_task_id += wf.size();
        for (const auto& p : wf) {
            Task t;
            t.id = base + static_cast<uint64_t>(p.index);
            t.workload_mcycles = p.work;
            t.data_kb = p.data;
            for (int q : p.parents) t.dependencies.push_back(base + static_cast<uint64_t>(q));
            d.tasks.push_back(t);
        }
        return d;
    }

    double meanDagWorkMcycles() const override { return mean_work_; }
    size_t numWorkflows() const { return templates_.size(); }

private:
    struct Proto {
        int index = 0;
        std::vector<int> parents;
        double work = 0, data = 0;
    };

    static std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, sep)) out.push_back(item);
        if (!s.empty() && s.back() == sep) out.push_back("");
        return out;
    }

    std::vector<std::vector<Proto>> templates_;
    double mean_work_ = 0.0;
};

}  // namespace iov::scheduling
