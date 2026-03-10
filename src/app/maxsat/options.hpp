
#pragma once

#include "optionslist.hpp"
#include "util/option.hpp"

// Application-specific program options for SAT solving.
// memberName                               short option name, long option name          default   min  max

OPTION_GROUP(grpAppMaxsat, "app/maxsat", "MaxSAT solving options")

OPT_INT(maxSatCardinalityEncoding, "maxsat-card-encoding", "", 3, 0, 4, "0=Warners, 1=DPW, 2=GTE, 3=heuristic, 4=virtual with theory")
OPT_FLOAT(maxSatFocusPeriod, "maxsat-focus-period", "", 0, 0, 3600, "Time period (s) until the lowest comb searcher is cancelled (0: never cancel)")
OPT_INT(maxSatFocusMin, "maxsat-focus-min", "", 1, 1, LARGE_INT, "Minimum number of comb searchers to keep alive")
OPT_INT(maxSatNumSearchers, "maxsat-searchers", "", 1, 1, LARGE_INT, "Number of searchers to run in parallel")
OPT_FLOAT(maxSatIntervalSkew, "maxsat-interval-skew", "", 0.5, 0, 1, "Skew to cut search intervals with")
OPT_STRING(maxSatSolutionFile, "maxsat-sol-file", "", "", "Path to file to write intermediate solutions to")
OPT_BOOL(maxSatWriteJobLiterals, "maxsat-write-job-lits", "", false, "Output all submitted jobs' literals into files for debugging")

OPT_BOOL(maxSatCoreGuided, "maxsat-cg", "", false, "Use core-guided search")
OPT_INT(maxSatCoreGuidedHeuristic, "maxsat-cg-heuristic", "", 3, 0, 4, "Heuristic for SIS launching: 1=core, 2=WCE round, 3=gap improvement, 4=time")
OPT_INT(maxSatCoreGuidedThreshold, "maxsat-cg-threshold", "", 25, 0, 100, "Threshold for heuristic 3 (4) in % (s)")
OPT_INT(maxSatCoreGuidedRelax, "maxsat-cg-relax", "", 2, 0, 2, "Relaxation method: naive=0, OLL=1, PMRES=2 (default 2)")
OPT_INT(maxSatCoreGuidedLaunches, "maxsat-cg-launches", "", 1, 0, 16, "How many searches to launch per reformulated instance")
OPT_INT(maxSatCoreGuidedFirstLaunches, "maxsat-cg-first-launches", "", 2, 0, 16, "How many searches to launch at the start")
OPT_BOOL(maxSatCoreGuidedDistributed, "maxsat-cg-distributed", "", true, "Use distribute solving for core-guded search")
OPT_INT(maxSatCoreGuidedTimeout, "maxsat-cg-timeout", "", 0, 0, LARGE_INT, "Kill core guided after timeout (in seconds)")
OPT_BOOL(maxSatCoreGuidedKillRedundant, "maxsat-cg-kill-redundant", "", false, "Kill the most redundant SIS search in favor of new reformulated instance")
OPT_INT(maxSatCoreGuidedFocusPeriod, "maxsat-cg-focus-period", "", 0, 0, 3600, "Start focusing on one searcher after x seconds")

#if MALLOB_USE_MAXPRE == 1
OPT_BOOL(maxPre, "maxpre", "", true, "true: use MaxPRE2 preprocessor library to preprocess instance; false: assume appropriately preprocessed file")
OPT_STRING(maxPreTechniques, "maxpre-techniques", "", "[bu]#", "Techniques string to forward to MaxPRE; minimum \"#\", (reasonable) maximum \"[bu]#[buvsrgcHTVGR]\"")
OPT_FLOAT(maxPreTimeout, "maxpre-timeout", "", LARGE_INT, 0, LARGE_INT, "Timeout for MaxPRE in seconds")
OPT_STRING(maxPreTechniquesPost, "maxpre-techniques-post", "", "[bu]#[buvsrgcHTVGR]", "Techniques string to forward to post- MaxPRE; minimum \"#\", (reasonable) maximum \"[bu]#[buvsrgcHTVGR]\"")
OPT_FLOAT(maxPreTimeoutPost, "maxpre-timeout-post", "", 0, 0, LARGE_INT, "Base timeout for post- MaxPRE in seconds (0: don't make improving MaxPRE calls)")
#endif
