import argparse
import json
import logging
import os
import shutil
import time
from os.path import join as pjoin

import subprocess

SAVER = "/home/saver/project/saver/bin/infer"


class Program(object):
    def __init__(self, name, config_cmd, build_cmd, subject_dir):
        self.name = name
        self.config_cmd = config_cmd
        self.build_cmd = build_cmd
        self.subject_dir = subject_dir


class Bug(object):
    def __init__(
        self,
        program,
        bug_id,
        error_type,
        source_file,
        source_proc,
        source_line,
        sink_file,
        sink_proc,
        sink_line,
    ):
        self.program = program
        self.bug_id = bug_id
        if error_type == "Memory Leak":
            self.error_type = "MEMORY_LEAK"
        self.source_file = source_file
        self.source_proc = source_proc
        self.source_line = source_line
        self.sink_file = sink_file
        self.sink_proc = sink_proc
        self.sink_line = sink_line
        self.report_path = ""

    def to_error_report(self, report_dir):
        res = dict()
        res["err_type"] = self.error_type
        res["source"] = dict()
        res["sink"] = dict()

        res["source"]["node"] = dict()
        res["source"]["node"]["filename"] = self.source_file
        res["source"]["node"]["procedure"] = self.source_proc
        res["source"]["node"]["line"] = self.source_line
        res["source"]["exp"] = None

        res["sink"]["node"] = dict()
        res["sink"]["node"]["filename"] = self.sink_file
        res["sink"]["node"]["procedure"] = self.sink_proc
        res["sink"]["node"]["line"] = self.sink_line
        res["sink"]["exp"] = None

        report_path = pjoin(report_dir, "%s-%s.json" % (self.program.name, self.bug_id))
        self.report_path = report_path
        with open(report_path, "w") as f:
            json.dump(res, f, indent=4)


def log_result(log):
    print(log)
    logfile.writelines(log)
    logfile.flush()


def run_process(cmd, dir, timeout=10000):
    curr_path = os.getcwd()
    start_time = time.time()
    os.chdir(dir)
    try:
        cp = subprocess.run(cmd, shell=True, timeout=timeout, stderr=subprocess.PIPE)
    except subprocess.TimeoutExpired:
        logger.info("Timeout on command: %s, in dir %s" % (cmd, dir))
        return timeout, -1, ""

    # ret = EasyProcess(cmd, cwd=dir).call(timeout)
    elapsed = time.time() - start_time
    time_used = elapsed
    rc = cp.returncode
    err = cp.stderr
    if rc != 0:
        logger.info("Running the cmd was not successful: %s" % cmd)
    else:
        done_msg = "%s ... Done!: %f sec. elapsed" % (cmd, elapsed)
        logger.info(done_msg)

    os.chdir(curr_path)
    return time_used, rc, err


def clean_project(dir):
    run_process("make clean", dir)
    shutil.rmtree("%s/infer-out" % dir, ignore_errors=True)


def setup_all(progs):
    for prog in progs:
        print("Setting up %s" % prog.name)
        src_dir = pjoin(prog.subject_dir, "src")
        if os.path.isdir(src_dir):
            shutil.rmtree(src_dir)

        setup_cmd = "bash ./subject_setup.sh %s" % prog.subject_dir
        time_used, rc, _ = run_process(setup_cmd, prog.subject_dir)
        logstr = "Download source code: %-15s: %4s sec.\n" % (
            prog.name,
            time_used,
        )
        log_result(logstr)

        time_used, rc, _ = run_process(prog.config_cmd, src_dir)
        logstr = "Configure project: %-15s: %4s sec.\n" % (
            prog.name,
            time_used,
        )
        log_result(logstr)


def saver_pre(prog):
    indiv_pre_log = pjoin(results_dir, "%s.pre.log" % prog.name)
    src_dir = pjoin(prog.subject_dir, "src")
    cmd_compile = "%s -g --headers --check-nullable-only -- make -j4" % SAVER
    # pgm_dir = '%s/%s' % (BENCH_DIR, pgm)
    cmd_compile = "%s -g --headers --check-nullable-only -- make -j4" % SAVER
    cmd_preanal = "%s saver --pre-analysis-only > %s" % (SAVER, indiv_pre_log)

    clean_project(src_dir)
    time_compile, rc_compile, _ = run_process(cmd_compile, src_dir)
    time_pre, rc_pre, _ = run_process(cmd_preanal, src_dir)

    time = float(time_compile) + float(time_pre)

    logstr = "SAVER compile+pre: %-15s: %3s sec.\n" % (prog.name, str(time)[0:3])
    log_result(logstr)


def get_status_string(retcode):
    return {
        201: ("X", "Fail to generate error report"),
        101: ("X", "False memory leak"),
        102: ("X", "Empty source and sinks"),
        103: ("X", "Empty error nodes"),
        104: ("X", "Labeling fails"),
        105: ("X", "Conversion fails"),
        106: ("O", "Succeed to generate a patch"),
        107: ("X", "No allocsites found for given source"),
        2: ("X", "Out of memory"),
        "Timeout": ("TO", "Timeout"),
    }.get(retcode, ("U", "Uncaught exception"))


def saver_patch(bug: Bug):
    src_dir = pjoin(bug.program.subject_dir, "src")
    bug_full_id = "%s-%s" % (bug.program.name, bug.bug_id)
    print("\033[34mRunning patching for %s\033[0m" % bug_full_id)
    cmd_patch = "%s saver --error-report %s" % (SAVER, bug.report_path)
    indiv_result_dir = pjoin(results_dir, bug_full_id)
    os.makedirs(indiv_result_dir, exist_ok=True)
    patch_log_path = pjoin(indiv_result_dir, "patch.log")
    patch_log = open(patch_log_path, "w")
    time_patch, rc_patch, err_content = run_process(cmd_patch, src_dir, timeout=3600)
    status_str = get_status_string(rc_patch)
    logstr = "%s, %s, %2s, %36s, %s sec.\n" % (
        bug.program.name,
        bug.bug_id,
        status_str[0],
        status_str[1],
        str(time_patch),
    )
    log_result(logstr)
    err_content = err_content.decode("utf-8")
    patch_log.writelines(str(err_content))
    patch_log.close()


def saver_pre_analysis_all(progs):
    global logfile
    logpath = pjoin(results_dir, "saver.pre-analysis.results")
    logfile = open(logpath, "w")
    print("\n==== Starting saver pre-analysis phase ====")
    for prog in progs:
        print("\033[34mRunning pre-analysis for %s\033[0m" % prog.name)
        saver_pre(prog)


def saver_patch_all(bugs):
    global logfile
    logpath = pjoin(results_dir, "saver.patch.results")
    logfile = open(logpath, "w")
    print("\n==== Starting saver patching phase ====")
    # first generate error reports for each bug
    err_report_dir = pjoin(results_dir, "error_reports")
    os.makedirs(err_report_dir, exist_ok=True)
    for bug in bugs:
        bug.to_error_report(err_report_dir)
    # next do real patching
    for bug in bugs:
        saver_patch(bug)


def saver_run(progs, bugs):
    global logfile
    logpath = pjoin(results_dir, "saver.pre.patch.results")
    logfile = open(logpath, "w")

    err_report_dir = pjoin(results_dir, "error_reports")
    os.makedirs(err_report_dir, exist_ok=True)

    for prog in progs:
        print("\033[34mRunning pre-analysis for %s\033[0m" % prog.name)
        saver_pre(prog)
        for bug in bugs:
            if bug.program.name == prog.name:
                bug.to_error_report(err_report_dir)
                saver_patch(bug)


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

    progs = []
    bugs = []
    # collect progs and bugs
    for meta_entry in meta:
        subject = meta_entry["subject"]
        # if "linux" in subject:
        #     continue
        # if "snort" in subject:
        #     # we already have results for this
        #     continue
        # if "openssl-3" in subject:
        #     # openssl-3 does not have memory leaks
        #     continue

        # temp
        if "openssl-1" not in subject:
            continue
        config_cmd = meta_entry["config_command"]
        build_cmd = meta_entry["build_command"]
        error_type = meta_entry["bug_type"]
        bug_id = meta_entry["bug_id"]
        source_file = meta_entry["source"]["src-file"]
        source_proc = meta_entry["source"]["procedure"]
        source_line = int(meta_entry["source"]["line"])
        sink_file = meta_entry["sink"]["src-file"]
        sink_proc = meta_entry["sink"]["procedure"]
        if "footpatch-line" in meta_entry["sink"]:
            sink_line = int(meta_entry["sink"]["footpatch-line"])
        else:
            sink_line = int(meta_entry["sink"]["line"])

        # add prog
        subject_dir = pjoin(bench, subject)
        existing_names = [p.name for p in progs]
        new_prog = Program(subject, config_cmd, build_cmd, subject_dir)
        if subject not in existing_names:
            progs.append(new_prog)

        # add bug
        if error_type != "Memory Leak":
            # saver does not support NPE
            continue

        new_bug = Bug(
            new_prog,
            bug_id,
            error_type,
            source_file,
            source_proc,
            source_line,
            sink_file,
            sink_proc,
            sink_line,
        )
        bugs.append(new_bug)

    # do real work
    os.makedirs(results_dir)
    if do_setup:
        # clone, and config each subject
        logpath = pjoin(results_dir, "setup.results")  # will be removed anw
        logfile = open(logpath, "w")
        setup_all(progs)
    else:
        saver_run(progs, bugs)
        # saver_pre_analysis_all(progs)
        # saver_patch_all(bugs)
