# SAVER

SAVER automatically generates safe patches for three kinds of memory-related errors common in C programs: **Memory Leak**, **Use-After-Free**, and **Double-Free**. SAVER works on large C programs up to 320K lines of code while still maintaining the key property of making safe patches. These patches are safe in that if SAVER succeeded in generating patches for a program, applying these patches does not introduce new errors on that program.

## Downloads & Installation
We provide two ways to install SAVER:
* A VirtualBox image containing all resources to reproduce the main results of our paper: [ICSE20_SAVER_artifacts.tar.gz](https://drive.google.com/open?id=1PHUBRDuzSKxHRIbUYNgdNnPamiSp0Fe1)
   * Ubuntu ID/PW: saver/saver
* A full copy of SAVER in its binary executable form: [saver.tar.gz](https://drive.google.com/open?id=1DP-jZBIIRLBRw2rxK2WujHclo1zvGURn)

Please see [INSTALL.md](./INSTALL.md) for full installation instructions and basic usage of SAVER.

## Workflow

The job of SAVER is to generate a patch that fixes memory-errors for a given program and a report that describes the error. The workflow of SAVER can be summarized as follows:

<p align="center"><img src="system.png"/></p>

1. Frontend & Pre-analysis
    * SAVER uses Facebook Infer's frontend to generate an intermediate representation of the given program (i.e., CFG).
    * SAVER conducts flow-insensitive points-to analysis as a pre-analysis to compute data-dependencies and a call-graph of the program.
2. Error Localization
    * Based on the results of the pre-analysis, SAVER identifies which program parts are relevant to the given error.
3. Invariant Generation
    * SAVER conducts path-sensitive heap analysis to compute an object flow graph (OFG) which summarizes heap-related behaviors.
4. Patch Generation
    * Based on the OFG, SAVER finds a patch that safely fixes the given error.

For technical details and algorithms behind SAVER, please consult our paper.

## Artifact Information

The VirtualBox image contains the following contents in the directory `~/ICSE20_artifacts/`
1. `table1` contains all necessary files needed to reproduce Table 1 of our paper.
    1. `benchmarks` contains our benchmarks as whole repositories, which are modified from the original to insert modeling code or to remove test modules of each program. Each of the repositories is equipped with scripts necessary for running FootPatch.
    2. `resources` contains the following:
        1. `benchmarks_org` contains the original versions of repositories in `table1/benchmarks`. We included them for comparison with the modified ones.
        2. `patches` contains manual source-code-level translations of SAVER's patch instructions. We prepared them because SAVER generates patch instructions in IR-level, which can be cumbersome to inspect. 
        3. `error_reports` contains error reports in `.json` files stating where the error starts and ends (i.e. sources and sinks).
        4. `benchmarks.json` records the configuration for each benchmark, the total number of alarms (generated by Infer-0.9.3), and which of them are true.
    3. `table1.py` compiles benchmarks and runs SAVER on them.
 
2. `table2` contains all necessary files needed to reproduce Table 2 in our paper.
    1. `benchmarks` contains our benchmarks as whole repositories and they are separated according to error types and versions, since they are checked out on different commit IDs. They are also modified from the original due to build issues.
    2. `resources` contains the following:
         1. `benchmarks_org` and `patches` as above `(table1)`.
         2. `bug.json` records the following for each benchmark:
            1. the URL of the original repository
            2. the commit ID where the error was fixed
            3. the commit ID by which we checked out when building the program
            4. necessary commands for building the repository
            5. the (sub)directory to start building, i.e. to run the commands
    3. `table2.py` compiles benchmarks and runs SAVER on them.
    
3. `tests` contains toy benchmarks for testing SAVER.

## Reproducing The Results Presented In The Paper
Here we explain how to reproduce our main results (i.e., effectiveness of fixing errors).

### Table 1
1. Go to the directory: `~/ICSE20_artifacts/table1/` and run the script by: ``` python3 table1.py --saver ```
    * This script first configures and compiles each benchmark. Then it runs the pre-analysis of SAVER and generates patches for each benchmark.
    * `saver.results` shows the overall results: the time taken to generate patches and the number of generated patches for each benchmark.
    * Patches will be generated in `results/saver` directories.
        * `.log` files show the running result of SAVER, which include the final patch instructions.
2. (Optional) If you want to reproduce results of FootPatch: ```python3 table1.py --footpatch```
    * A file named `footpatch.result` will be created, and it will show how long it took to generate patch for each error-report.
    * Patches will be generated under `results/footpatch`.
    
We also have written `README.md` in `~/ICSE20_aritacts/table1` for further details of the experiment.

### Table 2
1. Go to the directory: `~/ICSE20_artifacts/table2/` and run the script by: ``` python3 table2.py --saver ```
    * This script first configures and compiles each benchmark. Then it runs the pre-analysis of SAVER and generates patches for each benchmark.
    * `saver.results` shows the overall results, i.e., the Table 2 of the paper.
    * Patches will be generated in `results` directories.
        * `.log` files show the running result of SAVER, which include the final patch instructions.

## Contact Information

See [CONTACT.md](./CONTACT.md) for all contact information.
