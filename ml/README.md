# ML baseline: learned offloading

A simple machine-learning policy, used as a benchmark against our
hand-designed EFT policy.

## The idea

For a task and a candidate server, a small neural network predicts how
long the task will take from dispatch to finish. The ML policy sends the
task to the server with the lowest prediction. This is the usual
"learn a cost model, then pick the cheapest option" approach.

The network sees only six **raw** inputs:

| Input | Meaning |
|---|---|
| `distance_m` | distance from the task's vehicle to the server |
| `data_kb` | input data to upload |
| `workload_mcycles` | work the task needs |
| `cpu_capacity` | server speed |
| `pending_work_mcycles` | work already waiting at the server |
| `tasks_in_system` | tasks already at the server |

It is **not** given the link-latency/upload-rate formula or EFT's estimate.
It has to learn how these numbers turn into time from data.

## Training (`train_offload_model.py`)

1. **Data:** 32 simulation runs against SUMO with a *random* offloading
   policy, so every server gets tried in light and heavy situations:
   both workloads (synthetic and WfCommons), loads 20/40/60/80%, seeds
   1001–1004. These seeds are separate from the evaluation seeds (1–10).
   Each finished task gives one example: the six inputs for the server it
   went to, and the real time it took.
2. **Model:** scikit-learn `MLPRegressor`, 2 hidden layers of 32 ReLU units,
   predicting log(1 + ms), trained on seeds 1001–1003 (163k examples).
3. **Test:** prediction error on the held-out seed-1004 runs (52k examples).
4. **Export:** weights to `offload_mlp.txt`, a plain-text format that the
   C++ simulator reads (`src/offloading/mlp_policy.h`). The C++ predictions
   match scikit-learn to within 0.0005%.

```bash
python ml/train_offload_model.py        # about 1 minute; overwrites offload_mlp.txt
python run_experiment.py --offloading ml
```

The trained `offload_mlp.txt` (20 KB) is committed, so `--offloading ml`
works without retraining. The training data (`ml/data/`) is not committed.

## How well it predicts (held-out runs)

| | Median error | Mean error | R² |
|---|---:|---:|---:|
| ML model | 70 ms | 308 ms | 0.996 |
| EFT formula | 8 ms | 221 ms | 0.998 |

The network learns the relationship well (R² 0.996), but EFT's formula is
more accurate. That is expected here: EFT's formula uses the same link and
server equations as the simulator, so in this simulator it is almost an
exact model (median error 0 ms on the synthetic workload). Its remaining
errors come from things it ignores, such as later tasks whose upload
finishes sooner overtaking a task in the server queue.

## How it performs as an offloading policy

In the benchmark (main [README](../README.md#results), 10 evaluation seeds
per setting), the ML policy beats Least-loaded and Nearest everywhere, but
is slower than EFT on the same seed: by 0.3–2% on synthetic DAGs and by
4–7.5% on the real workflow DAGs. EFT was better in 79 of 80 paired runs.

## Limits of this comparison

- The simulator *is* EFT's model, which gives EFT a home advantage. On real
  hardware and real radio links, where no exact formula exists, a learned
  model could do relatively better. This benchmark does not test that.
- The ML policy is deliberately simple: one network and supervised
  learning from random exploration. No reinforcement learning, and no
  retraining on the states its own decisions create.
