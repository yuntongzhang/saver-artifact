import os, sys, glob
import time
import json
import logging
import argparse
from easyprocess import EasyProcess
from multiprocessing import Process
from multiprocessing import Pool
import xml.etree.ElementTree as ET
import shutil
import subprocess

TIME_OUT = 600

ROOT_DIR = os.getcwd()
BENCH_DIR = os.getcwd() + "/benchmarks"

pgm_file = open ('resources/benchmarks.json', 'r')
pgm_json = json.loads(pgm_file.read())

## Paths for binaries
FOOTPATCH = '/home/saver/project/footpatch/infer-linux64-v0.9.3/infer/bin/infer'
SAVER = '/home/saver/project/saver/bin/infer'

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
        , 107: ("X", "No allocsites found for given source")
        , 2: ("X", "Out of memory")
        , "Timeout": ("TO", "Timeout") }.get(retcode, ("U", "Uncaught exception"))

def run_process(cmd, dir, timeout=10000):
    start_time = time.time()
    ret = EasyProcess(cmd, cwd=dir).call(timeout)
    elapsed = time.time() - start_time
    
    ret.time = elapsed
    if ret.return_code is not 0:
        logger.info('Running the cmd was not successful: %s' % cmd)
    else:
        done_msg = "%s ... Done!: %f sec. elapsed" % (cmd, elapsed) 
        logger.info(done_msg)

    return ret

def clean_project(dir):
    run_process("make clean", dir)
    shutil.rmtree('%s/infer-out' % dir, ignore_errors=True)

### Configure each project ###
def init(pgm):
    configure_cmds = pgm_json[pgm]['configure']

    pgm_dir = '%s/%s' % (BENCH_DIR, pgm)
    logger.info("[%s] Configuration begins" % pgm)
    logger.info("-- Project dir: %s" % pgm_dir)
    logger.info("-- configuration cmds: %s" % configure_cmds)

    for cmd in configure_cmds:
        run_process(cmd, pgm_dir)

    logger.info("[%s] Configuration succeeds" % pgm)
    print("[%s] Configuration succeeds\n" % pgm)

### Run FootPatch ###
def footpatch(pgm):
    cmd = "./run-footpatch-global.sh"
    pgm_dir = "%s/%s" % (BENCH_DIR, pgm)
    
    clean_project(pgm_dir)
    ret = run_process(cmd, dir=pgm_dir)
    
    logstr = "%-15s: %4s sec.\n" % (pgm, str(ret.time)[0:4])
    log_result(logstr)

    shutil.move('%s/infer-out/footpatch' % pgm_dir, '%s/results/footpatch/%s' % (ROOT_DIR, pgm))
    
def pre(pgm):
    pgm_dir = '%s/%s' % (BENCH_DIR, pgm)
    cmd_compile = "%s -g --headers --check-nullable-only -- make -j4" % SAVER 
    cmd_preanal = "%s saver --pre-analysis-only" % SAVER

    clean_project(pgm_dir)
    ret_compile = run_process(cmd_compile, dir=pgm_dir)
    ret_preanal = run_process(cmd_preanal, dir=pgm_dir)

    time = float(ret_compile.time) + float(ret_preanal.time)

    if pgm == 'inetutils-1.9.4':
        clean_project('%s/ftpd' % pgm_dir)
        run_process(cmd_compile, dir='%s/ftpd' % pgm_dir)
        run_process(cmd_preanal, dir='%s/ftpd' % pgm_dir)
        
        clean_project('%s/src' % pgm_dir)
        run_process('%s rshd' % cmd_compile, dir='%s/src' % pgm_dir)
        run_process('%s rshd' % cmd_preanal, dir='%s/src' % pgm_dir)
    
    logstr = "%-15s: %3s sec.\n" % (pgm, str(time)[0:3])
    log_result(logstr)
    return time

def run_report(pgm, report):
    error_id = int(os.path.basename(report).split('_')[1].strip('.json'))
    dir = "%s/%s" % (BENCH_DIR, pgm)
    if pgm == 'inetutils-1.9.4':
        if error_id in [1, 2, 3]:
            dir = "%s/%s" % (dir, 'ftpd')
        elif error_id == 8:
            dir = "%s/%s" % (dir, 'src')
    
    cmd = "%s saver --error-report %s" % (SAVER, report)

    patch_log = open ("results/saver/%s-%s.log" % (pgm, error_id), 'w')

    ret_saver = run_process(cmd, dir, timeout=TIME_OUT)
    retcode = ret_saver.return_code if ret_saver.time < TIME_OUT else "Timeout"
    statusstr = get_status_string(retcode)
    logstr = "%-15s, %2d, %2s, %36s, %3s sec.\n" %\
        (pgm, error_id, statusstr[0], statusstr[1], str(ret_saver.time)[0:3])
    log_result(logstr)
    patch_log.writelines(ret_saver.stderr)
    patch_log.close()
    ret = {}
    ret['time'] = ret_saver.time
    ret['gen'] = statusstr[0]
    return ret

def saver(pgm):
    pgm_dir = "%s/%s" % (BENCH_DIR, pgm)
    
    init(pgm)
    global logfile
    logfile = open("pre.results", 'a')
    time_pre = pre(pgm)
    
    logfile = open("patch.results", 'a')
    results = {}
    reports = glob.glob("%s/resources/error_reports/%s_*.json" % (ROOT_DIR, pgm))
    for report in reports:
        report = os.path.abspath(report)
        error_id = int(os.path.basename(report).split('_')[1].strip('.json'))
        results[error_id] = run_report(pgm, report)

    logfile = open("saver.results", 'a')
    
    true_alarms = len(pgm_json[pgm]["truealarms"])
    false_alarms = pgm_json[pgm]["totalalarms"] - true_alarms
    time_fix = 0
    gen_true = 0
    gen_false = 0
    for error_id in results:
        time_fix = time_fix + results[error_id]['time']
        if results[error_id]["gen"] == "O":
            if error_id in pgm_json[pgm]["truealarms"]:
                gen_true = gen_true + 1
            else:
                gen_false = gen_false + 1

    logstr = "| %-15s | %-2s | %-2s | %-5s | %-5s | %-2s | %-2s |\n" % \
            (pgm, true_alarms, false_alarms, str(time_pre)[:5], str(time_fix)[:5], gen_true, gen_false)

    logfile = open("saver.results", 'a')
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
    parser.add_argument("--pgm", type=str, default=None, help="specify target program. Default is all")
    parser.add_argument("--init", action="store_true", help="configure benchmarks")
    parser.add_argument("--pre", action="store_true", help="compile and execute pre-analysis")
    parser.add_argument("--patch", action="store_true", help="generate patches")
    parser.add_argument("--saver", action="store_true", help="run saver")
    parser.add_argument("--footpatch", action="store_true", help="run footpatch")
    args = parser.parse_args()

    pgms = list(pgm_json.keys()) if args.pgm is None else [args.pgm]
    global logfile
    if args.init:
        with Pool(4) as p:
            p.map(init, pgms)
    elif args.pre:
        logfile = open("pre.results", 'w')
        with Pool(2) as p:
            p.map(pre, pgms)
    elif args.patch:
        logfile = open("patch.results", 'w')
        arguments = []
        for pgm in pgms:
            pgm_dir = "%s/%s" % (BENCH_DIR, pgm)
            reports = glob.glob("%s/resources/error_reports/%s_*.json" % (ROOT_DIR, pgm))
            for report in reports:
                arguments.append([pgm, os.path.abspath(report)])
        p.starmap(run_report, arguments)
    elif args.footpatch:
        logfile = open("footpatch.results", 'w')
        for pgm in pgms:
            footpatch(pgm)
    elif args.saver:
        logfile = open("saver.results", 'w')
        logfile.write("======================= Results ======================\n")
        logstr = "| %-15s | %-2s | %-2s | %-5s | %-5s | %-2s | %-2s |\n" % \
                ("Program", "T", "F", "pre", "fix", "GT", "GF")
        logfile.writelines(logstr)
        logfile.close()
        for pgm in pgms:
            saver(pgm)
    else:
        print ("No job specified")

