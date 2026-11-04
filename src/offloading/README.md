# Offloading (which server)

Decides **which edge server runs a task**. It gets the task plus, for every
server that can accept it, the server's state (capacity, backlog) and the
link from the task's vehicle to that server (latency, upload rate).

| File | Policy | Looks at |
|---|---|---|
| `eft_policy.h` | **EFT** – earliest estimated finish time | distance *and* load *and* server speed |
| `least_loaded_policy.h` | **Least-loaded** baseline – shortest backlog (in ms) | load only |
| `nearest_server_policy.h` | **Nearest** baseline – lowest link latency | distance only |
| `mlp_policy.h` | **ML** baseline – a small neural network predicts each server's finish time ([details](../../ml/README.md)) | learned from data |
| `mlp_policy.h` | **Random** – uniform choice; only used to collect ML training data | nothing |
| `offloading_policy.h` | Shared types, `IOffloadingPolicy` interface, `estimatedFinishMs()` | |

EFT's estimate, all in milliseconds:

```
upload  = link_latency + data_kb / upload_rate
backlog = pending_work / capacity
finish  = max(upload, backlog) + workload / capacity
```

Upload and the server's existing backlog overlap in time, hence the `max`.

Not modelled: executing a task on the vehicle itself, and downloading the
result back to the vehicle (assumed small).

Tests: `tests/run_tests.cpp` checks that, for a close-but-busy server versus
a far-but-idle fast server, Nearest picks the first while Least-loaded and EFT
pick the second, and that the EFT estimate is computed exactly.
