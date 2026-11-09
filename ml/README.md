# ML baseline: learned server selection

A simple machine-learning policy, used as a benchmark for the EFT rule inside QAJS.

## The idea

For a task and a candidate server, a small neural network predicts how long
the task will take from dispatch to finish. The task goes to the server with
the lowest prediction ("learn a cost model, then pick the cheapest option").

The network sees six **raw** inputs and is not given the link formula or
EFT's estimate:

| Input | Meaning |
|---|---|
| `distance_m` | distance from the task's vehicle to the server |
| `data_kb` | input data to upload |
| `workload_mcycles` | work the task needs |
| `cpu_capacity` | server speed |
| `pending_work_mcycles` | work already waiting at the server |
| `tasks_in_system` | tasks already at the server |

## Training (`train_offload_model.py`)

1. **Data:** 96 SUMO runs on the city scenario (16 servers) with a random
   server choice, weighted by server speed so that every server is tried in
   every situation. The runs cover both workloads, loads from 20% to 95%,
   both queue disciplines (FIFO, and DAG-SRPT with priority queues) and
   seeds 1001–1004. These seeds are never used in the evaluation (seeds
   1–10). At most 6,000 tasks are kept per run.
2. **Model:** scikit-learn `MLPRegressor`, two hidden layers of 32 ReLU
   units, predicting log(1 + ms). Trained on seeds 1001–1003 (432,000 examples).
3. **Test:** held-out seed 1004 (144,000 examples).
4. **Export:** `offload_mlp.txt`, a plain-text file that the C++ simulator
   reads (`src/offloading/mlp_policy.h`). The C++ predictions match
   scikit-learn's.

```bash
python ml/train_offload_model.py        # about 15 minutes; overwrites offload_mlp.txt
python run_experiment.py --offloading ml --scheduler fifo --server-queue fifo
```

## Prediction accuracy (held-out runs; `results/ml_accuracy.csv`)

| Queues | Predictor | Median error | R² |
|---|---|---:|---:|
| FIFO | ML | 72 ms | 0.72 |
| FIFO | EFT formula | 46 ms | 0.96 |
| priority | ML | 88 ms | 0.19 |
| priority | EFT formula | 99 ms | 0.12 |

With FIFO queues, EFT's formula is almost an exact model of the simulator,
and the network does not reach it. With priority queues neither predictor
is accurate, because a task's wait depends on future arrivals that jump
ahead of it.

## As a policy (main README)

- With FIFO order and FIFO queues it is 1–18% slower than QAJS on synthetic
  DAGs and 9–78% slower on WfCommons DAGs. It is better than Least-loaded and
  Nearest at most loads.
- Inside the QAJS framework it becomes unstable on WfCommons DAGs: its
  errors make it keep choosing servers that look fast, queues build up, and
  23% of applications are unfinished when the run ends.

## Limits of this comparison

- The simulator uses the same equations as EFT's formula, which gives EFT
  a home advantage. On real radio links, where no exact formula exists, a
  learned model could do relatively better. This benchmark does not test that.
- The model is deliberately simple: supervised learning from random
  exploration, with no reinforcement learning and no retraining on its own decisions.
