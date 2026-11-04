# Real-DAG workload (WfCommons)

Besides the synthetic random DAGs, the simulator can use **real workflow
DAG structures** from the public [WfCommons WfInstances](https://github.com/wfcommons/WfInstances)
dataset: execution logs of real scientific workflows run with the Pegasus
and Makeflow workflow systems.

```bash
python data/prepare_wfcommons.py      # downloads ~1.5 MB, writes data/wfcommons_dags.csv
python run_experiment.py --workload wfcommons
```

## What is used

One small execution instance of each of 9 applications, downloaded from a
pinned commit so the data never changes:

| Workflow | Tasks | Shape |
|---|---:|---|
| 1000Genome | 52 | wide, 3 levels |
| BLAST | 43 | fan-out / fan-in |
| BWA | 104 | wide, 3 levels |
| Cycles | 67 | 4 levels |
| Epigenomics | 41 | parallel pipelines, 9 levels |
| Montage | 58 | 8 levels, very uneven task sizes |
| Seismology | 101 | one wide level + merge |
| SoyKB | 96 | deep (11 levels) |
| SRASearch | 22 | fan-out / fan-in |

Each DAG arrival picks one of the 9 workflows at random.

## What is kept and what is changed

- **Kept:** the real dependency structure, and each task's runtime and
  input-file size *relative to the other tasks of the same workflow*.
- **Rescaled:** every workflow is scaled so its average task needs
  300 Mcycles and uploads 225 KB, the same averages as the synthetic
  workload. The originals run for seconds to hours on cluster machines,
  which does not fit a vehicle-to-roadside-server setting.

So these are real *scientific-workflow* DAG shapes and size distributions,
used to check that the conclusions hold beyond small random DAGs. They are
**not** recordings of real vehicle applications.

## Licence

WfInstances is published by the WfCommons project under LGPL-3.0. The data
is downloaded on demand into `data/wfinstances/` and is not stored in this
repository (both that folder and `wfcommons_dags.csv` are gitignored).
