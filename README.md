# Boolean Satisfiability Problem parallel / MPI Solver

## Instruction:

First generate the problem set by using our generation script:
```bash
python3 SAT_CNF_gen.py
```

This by default generates 1000 literals with 8000 clauses (each with length 2-5) into file `sat_problem.cnf`, which is reasonable runtime to benchmark our solutions

To run our solutions:

```bash
make
./SAT_serial
./SAT_parallel --nThreads 8
mpirun -n 8 ./SAT_MPI
```

Note: The the program will take the input file `sat_problem.cnf` at the root location of the program

## Output format:

Our program assumes our input to be Satisty, so a solution will be output in following example:

```
// If the program is parallel
For each thread, every 1000 tasks it will print the number of assigned variables for stat

SATISFIABLE
Variable 1 = True
Variable 2 = False
...
Variable 999 = False
Variable 1000 = False
Serial / Parallel / MPI execution time used : 10 seconds
// If the program is parallel / MPI
For each thread, it will print the number of task compeleted by each process

All tasks completed. Program terminating.
```


## Evaluation:

The runtime of our solution scales mostly linearly base on the number of threads of MPI used.

For example for a same sample sat_problem.cnf:

```
Runtimes:
SAT_serial                          2.8 seconds
SAT_parallel with 10 threads        0.20 seconds
SAT_MPI with 10 process             0.27 seconds
```