import argparse
import json
import logging
import os
import shutil
import time
from os.path import join as pjoin

import subprocess


class Program(object):
    def __init__(self, name, config_cmd, build_cmd, subject_dir):
        self.name = name
        self.config_cmd = config_cmd
        self.build_cmd = build_cmd
        self.subject_dir = subject_dir


def log_result(log):
    print(log)
    logfile.writelines(log)
    logfile.flush()


def run_process(cmd, dir, timeout=10000):
    curr_path = os.getcwd()
    start_time = time.time()
    os.chdir(dir)
    cp = subprocess.run(cmd, shell=True, timeout=timeout)
    # ret = EasyProcess(cmd, cwd=dir).call(timeout)
    elapsed = time.time() - start_time
    time_used = elapsed
    rc = cp.returncode
    if rc != 0:
        logger.info("Running the cmd was not successful: %s" % cmd)
    else:
        done_msg = "%s ... Done!: %f sec. elapsed" % (cmd, elapsed)
        logger.info(done_msg)

    os.chdir(curr_path)
    return time_used, rc


def clean_project(dir):
    run_process("make clean", dir)
    shutil.rmtree("%s/infer-out" % dir, ignore_errors=True)


# def footpatch(pgm):
#     cmd = "./run-footpatch-global.sh"
#     pgm_dir = "%s/%s" % (BENCH_DIR, pgm)

#     clean_project(pgm_dir)
#     ret = run_process(cmd, dir=pgm_dir)

#     logstr = "%-15s: %4s sec.\n" % (pgm, str(ret.time)[0:4])
#     log_result(logstr)

#     shutil.move('%s/infer-out/footpatch' % pgm_dir, '%s/results/footpatch/%s' % (ROOT_DIR, pgm))


def setup_all(progs):
    for prog in progs:
        print("Setting up %s" % prog.name)
        setup_cmd = "bash ./subject_setup.sh %s" % prog.subject_dir
        time_used, rc = run_process(setup_cmd, prog.subject_dir)
        logstr = "Download source code: %-15s: %4s sec.\n" % (
            prog.name,
            time_used,
        )
        log_result(logstr)

        src_dir = pjoin(prog.subject_dir, "src")
        time_used, rc = run_process(prog.config_cmd, src_dir)
        logstr = "Configure project: %-15s: %4s sec.\n" % (
            prog.name,
            time_used,
        )
        log_result(logstr)


def repair_all(progs):
    for prog in progs:
        pass


if __name__ == "__main__":
    global logfile

    logger = logging.getLogger()
    logger.setLevel(logging.INFO)
    formatter = logging.Formatter(
        "[%(asctime)s/%(levelname)s]%(processName)s - %(message)s"
    )

    file_handler = logging.FileHandler("log")
    file_handler.setFormatter(formatter)
    logger.addHandler(file_handler)

    parser = argparse.ArgumentParser()
    parser.add_argument("--setup", action="store_true", help="Do set up or run.")
    parser.add_argument("--bench", required=True, help="Path to the benchmark dir.")
    args = parser.parse_args()

    do_setup = args.setup
    bench = args.bench

    file_meta = pjoin(bench, "meta-data.json")
    with open(file_meta) as f:
        meta = json.load(f)

    # collect programs from meta data
    progs = []
    for meta_entry in meta:
        subject = meta_entry["subject"]
        config_cmd = meta_entry["config_command"]
        build_cmd = meta_entry["build_command"]
        subject_dir = pjoin(bench, subject)

        existing_names = [p.name for p in progs]
        if subject in existing_names:
            continue
        new_prog = Program(subject, config_cmd, build_cmd, subject_dir)
        progs.append(new_prog)

    if do_setup:
        logfile = open("setup.results", "w")
        setup_all(progs)
    else:
        logfile = open("footpatch.repair.results", "w")
        repair_all(progs)
