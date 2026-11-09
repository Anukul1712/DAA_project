#pragma once
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "offloading_policy.h"

namespace iov::offloading {

// The six raw inputs the ML model sees for one (task, server) pair. It is
// NOT given the link latency/rate or the EFT estimate -- it has to learn
// how distance, data, work, capacity and backlog turn into time.
inline std::vector<double> mlFeatures(const iov::scheduling::Task& task, const Candidate& c) {
    return {c.distance_m,
            task.data_kb,
            task.workload_mcycles,
            c.server.cpu_capacity,
            c.server.pending_work_mcycles,
            static_cast<double>(c.server.tasks_in_system)};
}

// Small fully connected neural network (multi-layer perceptron), trained in
// Python by ml/train_offload_model.py and loaded from a text file.
//
// File format (whitespace separated):
//   iov-mlp-v1
//   inputs N
//   mean  x1..xN            (feature standardisation: (x - mean) / scale)
//   scale x1..xN
//   target log1p_ms          (the network predicts log(1 + time in ms))
//   layers L
//   then for each layer:  layer IN OUT relu|identity
//                         OUT rows of IN weights, then OUT biases
class MlpModel {
public:
    explicit MlpModel(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot open ML model " + path +
                                          " -- train it with `python ml/train_offload_model.py`");
        std::string tok;
        expect(in, "iov-mlp-v1");
        expect(in, "inputs");
        in >> n_in_;
        expect(in, "mean");
        mean_.resize(n_in_);
        for (auto& v : mean_) in >> v;
        expect(in, "scale");
        scale_.resize(n_in_);
        for (auto& v : scale_) in >> v;
        expect(in, "target");
        in >> tok;
        if (tok != "log1p_ms") throw std::runtime_error("unsupported target " + tok);
        expect(in, "layers");
        int n_layers = 0;
        in >> n_layers;
        int prev = n_in_;
        for (int l = 0; l < n_layers; ++l) {
            Layer L;
            std::string act;
            expect(in, "layer");
            in >> L.in >> L.out >> act;
            if (L.in != prev) throw std::runtime_error("layer size mismatch in " + path);
            L.relu = (act == "relu");
            L.w.resize(static_cast<size_t>(L.in) * L.out);
            for (auto& v : L.w) in >> v;
            L.b.resize(L.out);
            for (auto& v : L.b) in >> v;
            prev = L.out;
            layers_.push_back(std::move(L));
        }
        if (!in || prev != 1) throw std::runtime_error("malformed ML model file " + path);
    }

    // Predicted time from dispatch to finish, in ms.
    double predictMs(const std::vector<double>& x) const {
        std::vector<double> a(n_in_);
        for (int i = 0; i < n_in_; ++i) a[i] = (x[i] - mean_[i]) / scale_[i];
        for (const auto& L : layers_) {
            std::vector<double> z(L.out);
            for (int o = 0; o < L.out; ++o) {
                double s = L.b[o];
                for (int i = 0; i < L.in; ++i) s += L.w[static_cast<size_t>(o) * L.in + i] * a[i];
                z[o] = L.relu ? std::max(0.0, s) : s;
            }
            a.swap(z);
        }
        return std::expm1(a[0]);
    }

    int inputs() const { return n_in_; }

private:
    struct Layer {
        int in = 0, out = 0;
        bool relu = false;
        std::vector<double> w, b;  // w is row-major [out][in]
    };
    static void expect(std::istream& in, const std::string& word) {
        std::string tok;
        in >> tok;
        if (tok != word) throw std::runtime_error("ML model file: expected '" + word + "', got '" + tok + "'");
    }
    int n_in_ = 0;
    std::vector<double> mean_, scale_;
    std::vector<Layer> layers_;
};

// ML offloading: predict each server's finish time with the MLP, pick the lowest.
class MlpPolicy : public IOffloadingPolicy {
public:
    explicit MlpPolicy(const std::string& model_path) : model_(model_path) {
        if (model_.inputs() != 6) throw std::runtime_error("ML model must have 6 inputs");
    }
    std::string name() const override { return "ml"; }

    std::optional<uint64_t> choose(const iov::scheduling::Task& task,
                                   const std::vector<Candidate>& candidates) override {
        std::optional<uint64_t> best;
        double best_ms = std::numeric_limits<double>::infinity();
        for (const auto& c : candidates) {
            double ms = model_.predictMs(mlFeatures(task, c));
            if (ms < best_ms) {
                best_ms = ms;
                best = c.server.server_id;
            }
        }
        return best;
    }

    const MlpModel& model() const { return model_; }

private:
    MlpModel model_;
};

// Random server, chosen with probability proportional to its CPU capacity
// (so slow servers are not overloaded). Used only to collect training data
// for the ML model: it explores every server in every situation.
class RandomPolicy : public IOffloadingPolicy {
public:
    explicit RandomPolicy(unsigned seed) : rng_(seed) {}
    std::string name() const override { return "random"; }
    std::optional<uint64_t> choose(const iov::scheduling::Task& /*task*/,
                                   const std::vector<Candidate>& candidates) override {
        if (candidates.empty()) return std::nullopt;
        std::vector<double> w;
        for (const auto& c : candidates) w.push_back(c.server.cpu_capacity);
        std::discrete_distribution<size_t> pick(w.begin(), w.end());
        return candidates[pick(rng_)].server.server_id;
    }

private:
    std::mt19937 rng_;
};

}  // namespace iov::offloading
