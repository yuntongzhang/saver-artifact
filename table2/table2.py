import os, sys, glob
import time
import json
import logging
import argparse
from easyprocess import EasyProcess
from multiprocessing import Process
from multiprocessing import Pool
import xml.etree.ElementTree as ET

TIME_OUT = 600
ROOT_DIR = os.getcwd()
BENCH_DIR = os.getcwd() + "/benchmarks"
SAVER = "/home/saver/project/saver/bin/infer"
bug_file = open ('resources/bug.json', 'r')
bug_json = json.loads(bug_file.read())

def log_result(log):
    print(log)
    logfile.writelines(log)
    logfile.flush()

def get_status_string(retcode):
    return \
        { 201: ("X", "Fail to generate error report")
        , 101: ("X", "False memory leak")
        , 102: ("X", "Empty source and sinks")
        , 103: ("X", "Empty error nodes")
        , 104: ("X", "Labeling fails")
        , 105: ("X", "Conversion fails")
        , 106: ("O", "Succeed to generate a patch")
        , "Timeout": ("TO", "Timeout") }.get(retcode, ("U", "Uncaught exception"))

def run_process(cmd, dir, timeout=10000):
    logger.info('Running %s at %s' % (cmd, dir))

    start_time = time.time()
    ret = EasyProcess(cmd, cwd=dir).call(timeout)
    elapsed_time = time.time()
    ret.time = elapsed_time - start_time

    if ret.return_code is not 0:
        logger.warning('Running the command was not successful: %s' % cmd)
    else:
        logger.info('%s ... Done!: %f sec. elapsed' % (cmd, elapsed_time))

    logger.handlers[0].flush
    return ret

def init(bug):
    bug_dir = "%s/%s" % (BENCH_DIR, bug)
    
    # Autogen & configure
    autogen_cmd = './autogen.sh'
    run_process(autogen_cmd, bug_dir)

    configure_cmd = './configure CC=clang'
    run_process(configure_cmd, bug_dir)

def pre(bug):
    bug_dir = "%s/%s" % (BENCH_DIR, bug)
    compile_dir = "%s/%s" % (bug_dir, bug_json[bug]["compile-directory"])
    logger.info("== Compiling %s ==" % bug)
    os.chdir(bug_dir)
    
    compile_cmds = bug_json[bug]["compile-commands"]
    # Compile 
    for cmd in compile_cmds:
        logger.info("compile command: %s" % cmd)
        logger.handlers[0].flush
        if cmd.split(" ")[0] == "cd":
            os.chdir(cmd.split(" ")[1])
        elif cmd.split(" ")[0] == "infer":
            ret_compile = run_process(cmd, os.getcwd())
        elif cmd.split(" ")[0] == "sed":
            os.system(cmd)
        else: 
            run_process(cmd, os.getcwd())

    # Preanalysis
    run_process("rm infer-out/.preanalysis", compile_dir)
    ret = run_process("%s saver --pre-analysis-only" % SAVER, compile_dir)
  
    time = ret_compile.time + ret.time
    logstr = "%-15s: %5s sec.\n" % (bug, str(time)[:5])
    log_result(logstr)

def patch(bug):
    bug_dir = "%s/%s" % (BENCH_DIR, bug)
    compile_dir = "%s/%s" % (bug_dir, bug_json[bug]["compile-directory"])
    patch_log = open('%s/results/%s.log' % (ROOT_DIR, bug), 'w')

    # Analysis
    error_json = "%s/resources/error_reports/%s.json" % (ROOT_DIR, bug)
    command = '%s saver --error-report %s --analysis-with-fimem' % (SAVER, error_json)
    ret_saver = run_process(command, compile_dir, timeout=TIME_OUT)
   
    # Print
    patch_log.writelines(ret_saver.stderr)
    patch_log.close()

    retcode = ret_saver.return_code if ret_saver.time < TIME_OUT else "Timeout"
    statusstr = get_status_string(retcode)
    logstr = "%-15s, %2s, %30s, %3s sec.\n" %\
        (bug, statusstr[0], statusstr[1], str(ret_saver.time)[0:3])
    log_result(logstr)

    ret = {}
    ret["type"] = bug.split("-")[-2]
    ret["gen"] = statusstr[0]
    return ret

def saver(pgm, bugs, cpus):
    global logfile
    logfile = open("pre.results", 'a')
    
    for bug in bugs:
        init(bug)
        pre(bug)

    logfile = open("patch.results", 'a')
    patches = [patch(bug) for bug in bugs]

    logfile = open("saver.results", 'a')
    
    uafs = 0
    uaf_gens = 0
    dfs = 0
    df_gens = 0

    for result in patches:
        if result["type"] == "df":
            dfs = dfs + 1
            if result["gen"] == "O":
                df_gens = df_gens + 1
        if result["type"] == "uaf":
            uafs = uafs + 1
            if result["gen"] == "O":
                uaf_gens = uaf_gens + 1

    logstr = "| %-7s | %-6s | %-6s | %-6s | %-6s |\n" % \
        (pgm, uafs, uaf_gens, dfs, df_gens)
    
    logfile.writelines(logstr)
    logfile.flush()


if __name__ == '__main__':
    logger = logging.getLogger()
    logger.setLevel(logging.INFO)
    formatter = logging.Formatter('[%(asctime)s/%(levelname)s]%(processName)s - %(message)s')

    file_handler = logging.FileHandler("log")
    file_handler.setFormatter(formatter)
    logger.addHandler(file_handler)

    parser = argparse.ArgumentParser()
    parser.add_argument("--bug", type=str, default=None, help="specify target dirs. Default is all")
    parser.add_argument("--init", action="store_true", help="configure benchmarks")
    parser.add_argument("--pre", action="store_true", help="compile and execute pre-analysis")
    parser.add_argument("--patch", action="store_true", help="generate patches")
    parser.add_argument("--saver", action="store_true", help="run saver")
    args = parser.parse_args()

    bugs = list(bug_json.keys()) if args.bug is None else [args.bug]
    global logfile
    if args.init:
        with Pool(2) as p:
            p.map(init, bugs)
    elif args.pre:
        logfile = open("pre.results", 'w')
        with Pool(2) as p:
            p.map(pre, bugs)
    elif args.patch:
        logfile = open("patch.results", 'w')
        with Pool(2) as p:
            p.map(patch, bugs)
    elif args.saver:
        logfile = open("saver.results", 'w')
        logstr1 = "| %-7s | %-15s | %-15s |\n" % ("", "Use-After-Free", "Double-Free")
        logstr2 = "| %-7s | %-6s | %-6s | %-6s | %-6s |\n" % \
                ("Program", "#Comm.", "#Fixed", "#Comm.", "#Fixed")
        logfile.write(logstr1)
        logfile.write(logstr2)
        logfile.flush()
        for pgm in ["lxc", "grub", "p11-kit"]:
            bugs_pgm = [bug for bug in bugs if '-'.join(bug.split('-')[:-2]) == pgm]
            saver(pgm, bugs_pgm, 2)
    else:
        print ("No job specified")

