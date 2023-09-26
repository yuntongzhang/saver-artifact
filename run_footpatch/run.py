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
    try:
        cp = subprocess.run(cmd, shell=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        logger.info("Timeout on command: %s, in dir %s" % (cmd, dir))
        return timeout, -1

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


def footpatch(prog, result_dir):
    footpatch_script = pjoin(script_dir, "run-footpatch-global.sh")
    src_dir = pjoin(prog.subject_dir, "src")
    # 1 clean
    clean_project(src_dir)
    # 2 copy over record and play to src dir
    shutil.copy(pjoin(script_dir, "RECORD"), src_dir)
    shutil.copy(pjoin(script_dir, "PLAY"), src_dir)
    # 3 run footpatch
    overall_log = pjoin(src_dir, "footpatch.log")
    cmd = "bash %s %s > %s" % (footpatch_script, prog.build_cmd, overall_log)
    time_used, rc = run_process(cmd, src_dir, timeout=3600)
    logstr = "Footpatch: %-15s: %4s sec.\n" % (prog.name, time_used)
    log_result(logstr)
    # 4 move results
    shutil.move("%s/infer-out/footpatch" % src_dir, result_dir)
    shutil.move(overall_log, result_dir)


def setup_all(progs):
    for prog in progs:
        print("Setting up %s" % prog.name)
        src_dir = pjoin(prog.subject_dir, "src")
        if os.path.isdir(src_dir):
            shutil.rmtree(src_dir)

        setup_cmd = "bash ./subject_setup.sh %s" % prog.subject_dir
        time_used, rc = run_process(setup_cmd, prog.subject_dir)
        logstr = "Download source code: %-15s: %4s sec.\n" % (
            prog.name,
            time_used,
        )
        log_result(logstr)

        time_used, rc = run_process(prog.config_cmd, src_dir)
        logstr = "Configure project: %-15s: %4s sec.\n" % (
            prog.name,
            time_used,
        )
        log_result(logstr)


def repair_all(progs):
    os.makedirs(results_dir, exist_ok=True)
    for prog in progs:
        print("\033[34mRunning repair for %s\033[0m" % prog.name)
        prog_result_dir = pjoin(results_dir, prog.name)
        os.makedirs(prog_result_dir, exist_ok=True)
        footpatch(prog, prog_result_dir)


if __name__ == "__main__":
    global logfile

    script_dir = os.path.dirname(os.path.realpath(__file__))
    results_dir = pjoin(script_dir, "results")
    if os.path.isdir(results_dir):
        shutil.rmtree(results_dir)
    if os.path.isfile("log"):
        os.remove("log")

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

        if "linux" in subject:
            continue

        existing_names = [p.name for p in progs]
        if subject in existing_names:
            continue
        new_prog = Program(subject, config_cmd, build_cmd, subject_dir)
        progs.append(new_prog)

    if do_setup:
        logfile = open("setup.results", "w")
        setup_all(progs)
    else:
        logpath = "footpatch.repair.results"
        logfile = open(logpath, "w")
        repair_all(progs)
        shutil.move(logpath, results_dir)
