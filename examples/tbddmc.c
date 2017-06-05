#include <argp.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef HAVE_PROFILER
#include <gperftools/profiler.h>
#endif

#include <getrss.h>

#include <sylvan.h>
#include <sylvan_int.h>

/* Configuration (via argp) */
static int report_levels = 0; // report states at end of every level
static int report_table = 0; // report table size at end of every level
static int report_nodes = 0; // report number of nodes of TBDDs
static int strategy = 2; // 0 = BFS, 1 = PAR, 2 = SAT, 3 = CHAINING
static int check_deadlocks = 0; // set to 1 to check for deadlocks on-the-fly (only bfs/par)
static int merge_relations = 0; // merge relations to 1 relation
static int print_transition_matrix = 0; // print transition relation matrix
static int workers = 0; // autodetect
static char* model_filename = NULL; // filename of model
#ifdef HAVE_PROFILER
static char* profile_filename = NULL; // filename for profiling
#endif

/* argp configuration */
static struct argp_option options[] =
{
    {"workers", 'w', "<workers>", 0, "Number of workers (default=0: autodetect)", 0},
    {"strategy", 's', "<bfs|par|sat|chaining>", 0, "Strategy for reachability (default=sat)", 0},
#ifdef HAVE_PROFILER
    {"profiler", 'p', "<filename>", 0, "Filename for profiling", 0},
#endif
    {"deadlocks", 3, 0, 0, "Check for deadlocks", 1},
    {"count-nodes", 5, 0, 0, "Report #nodes for TBDDs", 1},
    {"count-states", 1, 0, 0, "Report #states at each level", 1},
    {"count-table", 2, 0, 0, "Report table usage at each level", 1},
    {"merge-relations", 6, 0, 0, "Merge transition relations into one transition relation", 1},
    {"print-matrix", 4, 0, 0, "Print transition matrix", 1},
    {0, 0, 0, 0, 0, 0}
};
static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'w':
        workers = atoi(arg);
        break;
    case 's':
        if (strcmp(arg, "bfs")==0) strategy = 0;
        else if (strcmp(arg, "par")==0) strategy = 1;
        else if (strcmp(arg, "sat")==0) strategy = 2;
        else if (strcmp(arg, "chaining")==0) strategy = 3;
        else argp_usage(state);
        break;
    case 4:
        print_transition_matrix = 1;
        break;
    case 3:
        check_deadlocks = 1;
        break;
    case 1:
        report_levels = 1;
        break;
    case 2:
        report_table = 1;
        break;
    case 5:
        report_nodes = 1;
        break;
    case 6:
        merge_relations = 1;
        break;
#ifdef HAVE_PROFILER
    case 'p':
        profile_filename = arg;
        break;
#endif
    case ARGP_KEY_ARG:
        if (state->arg_num >= 1) argp_usage(state);
        model_filename = arg;
        break;
    case ARGP_KEY_END:
        if (state->arg_num < 1) argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, "<model>", 0, 0, 0, 0 };

/**
 * Types (set and relation)
 */
typedef struct set
{
    TBDD bdd;
    TBDD variables; // all variables in the set (used by satcount)
} *set_t;

typedef struct relation
{
    TBDD bdd;
    TBDD variables; // all variables in the relation (used by relprod)
    int r_k, w_k, *r_proj, *w_proj;
    TBDD satdom;    // the domain of set for relnext, for the saturation strategy
} *rel_t;

static int vectorsize; // size of vector in integers
static int *statebits; // number of bits for each state integer
static int actionbits; // number of bits for action label
static int totalbits;  // total number of bits
static int next_count; // number of partitions of the transition relation
static rel_t *next;    // each partition of the transition relation
static TBDD vectordom; // domain of vector (TBDD variables)

/**
 * Obtain current wallclock time
 */
static double
wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static double t_start;
#define INFO(s, ...) fprintf(stdout, "[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "Abort at line %d!\n", __LINE__); exit(-1); }

static char*
to_h(double size, char *buf)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    int i = 0;
    for (;size>1024;size/=1024) i++;
    sprintf(buf, "%.*f %s", i, size, units[i]);
    return buf;
}

static void
print_memory_usage(void)
{
    char buf[32];
    to_h(getCurrentRSS(), buf);
    INFO("Memory usage: %s\n", buf);
}

/**
 * Load a set from file
 * The expected binary format:
 * - int k : projection size, or -1 for full state
 * - int[k] proj : k integers specifying the variables of the projection
 * - TBDD[1] TBDD (mtbdd binary format)
 */
#define set_load(f) CALL(set_load, f)
TASK_1(set_t, set_load, FILE*, f)
{
    // allocate set
    set_t set = (set_t)malloc(sizeof(struct set));
    set->bdd = tbdd_false;
    set->variables = tbdd_true;
    tbdd_protect(&set->bdd);
    tbdd_protect(&set->variables);

    // read k
    int k;
    if (fread(&k, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");

    if (k == -1) {
        // create variables for a full state vector
        uint32_t vars[totalbits];
        for (int i=0; i<totalbits; i++) vars[i] = 2*i;
        set->variables = tbdd_from_array(vars, totalbits);
    } else {
        // read proj
        int proj[k];
        if (fread(proj, sizeof(int), k, f) != (size_t)k) Abort("Invalid input file!\n");
        // create variables for a short/projected state vector
        uint32_t vars[totalbits];
        uint32_t cv = 0;
        int j = 0, n = 0;
        for (int i=0; i<vectorsize && j<k; i++) {
            if (i == proj[j]) {
                for (int x=0; x<statebits[i]; x++) vars[n++] = (cv += 2) - 2;
                j++;
            } else {
                cv += 2 * statebits[i];
            }
        }
        set->variables = tbdd_from_array(vars, n);
    }

    // read bdd
    if (tbdd_reader_frombinary(f, &set->bdd, 1) != 0) Abort("Invalid input file!\n");

    return set;
}

/**
 * Load a relation from file
 * This part just reads the r_k, w_k, r_proj and w_proj variables.
 */
#define rel_load_proj(f) CALL(rel_load_proj, f)
TASK_1(rel_t, rel_load_proj, FILE*, f)
{
    rel_t rel = (rel_t)malloc(sizeof(struct relation));
    int r_k, w_k;
    if (fread(&r_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    if (fread(&w_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    rel->r_k = r_k;
    rel->w_k = w_k;
    int *r_proj = (int*)malloc(sizeof(int[r_k]));
    int *w_proj = (int*)malloc(sizeof(int[w_k]));
    if (fread(r_proj, sizeof(int), r_k, f) != (size_t)r_k) Abort("Invalid file format.");
    if (fread(w_proj, sizeof(int), w_k, f) != (size_t)w_k) Abort("Invalid file format.");
    rel->r_proj = r_proj;
    rel->w_proj = w_proj;

    rel->bdd = tbdd_false;
    tbdd_protect(&rel->bdd);

    /* Compute a_proj the union of r_proj and w_proj, and a_k the length of a_proj */
    int a_proj[r_k+w_k];
    int r_i = 0, w_i = 0, a_i = 0;
    for (;r_i < r_k || w_i < w_k;) {
        if (r_i < r_k && w_i < w_k) {
            if (r_proj[r_i] < w_proj[w_i]) {
                a_proj[a_i++] = r_proj[r_i++];
            } else if (r_proj[r_i] > w_proj[w_i]) {
                a_proj[a_i++] = w_proj[w_i++];
            } else /* r_proj[r_i] == w_proj[w_i] */ {
                a_proj[a_i++] = w_proj[w_i++];
                r_i++;
            }
        } else if (r_i < r_k) {
            a_proj[a_i++] = r_proj[r_i++];
        } else if (w_i < w_k) {
            a_proj[a_i++] = w_proj[w_i++];
        }
    }
    const int a_k = a_i;

    /* Compute all_variables, which are all variables the transition relation is defined on */
    uint32_t all_vars[totalbits * 2];
    uint32_t curvar = 0; // start with variable 0
    int i=0, j=0, n=0;
    for (; i<vectorsize && j<a_k; i++) {
        if (i == a_proj[j]) {
            for (int k=0; k<statebits[i]; k++) {
                all_vars[n++] = curvar;
                all_vars[n++] = curvar + 1;
                curvar += 2;
            }
            j++;
        } else {
            curvar += 2 * statebits[i];
        }
    }
    rel->variables = tbdd_from_array(all_vars, n);
    tbdd_protect(&rel->variables);

    /* Compute satdom */
    int top_var = all_vars[0]/2;
    assert(top_var*2 == all_vars[0]);
    n = 0;
    for (i=top_var; i<totalbits; i++) all_vars[n++] = i*2;
    rel->satdom = tbdd_from_array(all_vars, n);
    tbdd_protect(&rel->satdom);

    return rel;
}

/**
 * Load a relation from file
 * This part just reads the bdd of the relation
 */
#define rel_load(rel, f) CALL(rel_load, rel, f)
VOID_TASK_2(rel_load, rel_t, rel, FILE*, f)
{
    if (tbdd_reader_frombinary(f, &rel->bdd, 1) != 0) Abort("Invalid file format!\n");
}

/**
 * Print a single example of a set to stdout
 * Assumption: the example is a full vector and variables contains all state variables...
 */
#define print_example(example, variables) CALL(print_example, example, variables)
VOID_TASK_2(print_example, TBDD, example, TBDD, variables)
{
    uint8_t str[totalbits];

    if (example != sylvan_false) {
        tbdd_enum_first(example, variables, str);
        int x=0;
        printf("[");
        for (int i=0; i<vectorsize; i++) {
            uint32_t res = 0;
            for (int j=0; j<statebits[i]; j++) {
                if (str[x++] == 1) res++;
                res <<= 1;
            }
            if (i>0) printf(",");
            printf("%" PRIu32, res);
        }
        printf("]");
    }
}

/**
 * Implement sequential strategy (that performs the relnext operations one by one)
 * This function does one level...
 */
TASK_4(TBDD, go_bfs, TBDD, cur, TBDD, visited, size_t, from, size_t, len)
{
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        TBDD succ = tbdd_relnext(cur, next[from]->bdd, next[from]->variables, vectordom);
        tbdd_refs_push(succ);
        TBDD result = tbdd_diff(succ, visited, vectordom);
        tbdd_refs_pop(1);
        return result;
    } else {
        // Recursively calculate left+right
        TBDD left = CALL(go_bfs, cur, visited, from, len/2);
        tbdd_refs_push(left);
        TBDD right = CALL(go_bfs, cur, visited, from+len/2, len-len/2);
        tbdd_refs_push(right);
        // Merge results of left+right
        TBDD result = tbdd_or(left, right, vectordom);
        tbdd_refs_pop(2);
        return result;
    }
}

/**
 * Implementation of the BFS strategy
 */
VOID_TASK_1(bfs, set_t, set)
{
    /* Prepare variables */
    TBDD visited = set->bdd;
    TBDD front = visited;
    tbdd_refs_pushptr(&visited);
    tbdd_refs_pushptr(&front);

    int iteration = 1;
    do {
        // compute successors
        front = CALL(go_bfs, front, visited, 0, next_count);
        // visited = visited + front
        visited = tbdd_or(visited, front, vectordom);

        INFO("Level %d done", iteration);
        if (report_levels) printf(", %'0.0f states explored", tbdd_satcount(visited, set->variables));
        if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            printf(", table: %0.1f%% full (%'zu nodes)", 100.0*(double)filled/total, filled);
        }
        char buf[32];
        to_h(getCurrentRSS(), buf);
        printf(", rss=%s.\n", buf);
        iteration++;
    } while (front != tbdd_false);

    set->bdd = visited;
    tbdd_refs_popptr(2);
}

/**
 * Implement parallel strategy (that performs the relnext operations in parallel)
 * This function does one level...
 */
TASK_4(TBDD, go_par, TBDD, cur, TBDD, visited, size_t, from, size_t, len)
{
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        TBDD succ = tbdd_relnext(cur, next[from]->bdd, next[from]->variables, vectordom);
        tbdd_refs_push(succ);
        TBDD result = tbdd_diff(succ, visited, vectordom);
        tbdd_refs_pop(1);
        return result;
    } else {
        // Recursively calculate left+right
        tbdd_refs_spawn(SPAWN(go_par, cur, visited, from, len/2));
        TBDD right = CALL(go_par, cur, visited, from+len/2, len-len/2);
        tbdd_refs_push(right);
        TBDD left = tbdd_refs_sync(SYNC(go_par));
        tbdd_refs_push(left);
        // Merge results of left+right
        TBDD result = tbdd_or(left, right, vectordom);
        tbdd_refs_pop(2);
        return result;
    }
}

/**
 * Implementation of the PAR strategy
 */
VOID_TASK_1(par, set_t, set)
{
    /* Prepare variables */
    TBDD visited = set->bdd;
    TBDD front = visited;
    tbdd_refs_pushptr(&visited);
    tbdd_refs_pushptr(&front);

    int iteration = 1;
    do {
        // compute successors
        front = CALL(go_par, front, visited, 0, next_count);
        // visited = visited + front
        visited = tbdd_or(visited, front, vectordom);

        INFO("Level %d done", iteration);
        if (report_levels) printf(", %'0.0f states explored", tbdd_satcount(visited, set->variables));
        if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            printf(", table: %0.1f%% full (%'zu nodes)", 100.0*(double)filled/total, filled);
        }
        char buf[32];
        to_h(getCurrentRSS(), buf);
        printf(", rss=%s.\n", buf);
        iteration++;
    } while (front != tbdd_false);

    set->bdd = visited;
    tbdd_refs_popptr(2);
}

/**
 * Implementation of (parallel) saturation
 * (assumes relations are ordered on first variable)
 */
TASK_2(TBDD, go_sat, TBDD, set, int, idx)
{
    /* Terminal cases */
    if (set == tbdd_false) return tbdd_false;
    if (idx == next_count) return set;

    /* Consult the cache */
    TBDD result;
    const TBDD _set = set;
    if (cache_get3(202LL<<52, _set, idx, 0, &result)) return result;
    tbdd_refs_pushptr(&_set);

    tbddnode_t set_node = TBDD_NOTAG(set) == tbdd_true ? NULL : TBDD_GETNODE(set);
    uint32_t set_var = set_node == NULL ? 0xfffff : tbddnode_getvariable(set_node);
    uint32_t set_tag = TBDD_GETTAG(set);
    const uint32_t rel_var = tbdd_getvar(next[idx]->variables);
    const uint32_t pivot_var = set_tag < rel_var ? set_tag : (set_var < rel_var ? set_var : rel_var);

    /* Check if the relation should be applied */
    if (pivot_var == rel_var) {
        /* Count the number of relations starting here */
        int n = 1;
        while ((idx+n) < next_count && rel_var == tbdd_getvar(next[idx+n]->variables)) n++;
        /*
         * Compute until fixpoint:
         * - SAT deeper
         * - chain-apply all current level once
         */
        TBDD prev = tbdd_false;
        TBDD step = tbdd_false;
        tbdd_refs_pushptr(&set);
        tbdd_refs_pushptr(&prev);
        tbdd_refs_pushptr(&step);
        while (prev != set) {
            prev = set;
            // SAT deeper
            set = CALL(go_sat, set, idx+n);
            // chain-apply all current level once
            for (int i=0; i<n; i++) {
                step = tbdd_relnext(set, next[idx+i]->bdd, next[idx+i]->variables, next[idx+i]->satdom);
                set = tbdd_or(set, step, next[idx+i]->satdom);
                step = tbdd_false; // unset, for gc
            }
        }
        tbdd_refs_popptr(3);
        result = set;
    } else {
        /**
         * Obtain cofactors of set and compute with recursion
         * ASSUMPTION: dom_next_var = pivot_var + 2
         */
        // assert((pivot_var&1) == 0);
        // assert(pivot_var >= set_tag);
        if (pivot_var < set_var) {
            TBDD set0 = tbdd_settag(set, pivot_var + 2);
            result = CALL(go_sat, set0, idx);
            result = tbdd_makenode(pivot_var, result, tbdd_false, pivot_var + 2);
        } else {
            // assert(tbddnode_low(set, set_node) != set);
            // assert(tbddnode_high(set, set_node) != set);
            tbdd_refs_spawn(SPAWN(go_sat, tbddnode_low(set, set_node), idx));
            TBDD high = CALL(go_sat, tbddnode_high(set, set_node), idx);
            tbdd_refs_push(high);
            TBDD low = tbdd_refs_sync(SYNC(go_sat));
            tbdd_refs_pop(1);
            result = tbdd_makenode(pivot_var, low, high, pivot_var + 2);
        }
    }

    /* Store in cache */
    cache_put3(202LL<<52, _set, idx, 0, result);
    tbdd_refs_popptr(1);
    return result;
}

/**
 * Wrapper for the Saturation strategy
 */
VOID_TASK_1(sat, set_t, set)
{
    set->bdd = CALL(go_sat, set->bdd, 0);
}

/**
 * Implementation of the Chaining strategy (does not support deadlock detection)
 */
VOID_TASK_1(chaining, set_t, set)
{
    TBDD visited = set->bdd;
    TBDD next_level = visited;
    TBDD succ = tbdd_false;

    tbdd_refs_pushptr(&visited);
    tbdd_refs_pushptr(&next_level);
    tbdd_refs_pushptr(&succ);

    int iteration = 1;
    do {
        // calculate successors in parallel
        for (int i=0; i<next_count; i++) {
            succ = tbdd_relnext(next_level, next[i]->bdd, next[i]->variables, vectordom);
            next_level = tbdd_or(next_level, succ, vectordom);
            succ = tbdd_false; // reset, for gc
        }

        // new = new - visited
        // visited = visited + new
        next_level = tbdd_diff(next_level, visited, vectordom);
        visited = tbdd_or(visited, next_level, vectordom);

        if (report_table && report_levels) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, %'0.0f states explored, table: %0.1f%% full (%'zu nodes)\n",
                iteration, tbdd_satcount(visited, set->variables),
                100.0*(double)filled/total, filled);
        } else if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, table: %0.1f%% full (%'zu nodes)\n",
                iteration,
                100.0*(double)filled/total, filled);
        } else if (report_levels) {
            INFO("Level %d done, %'0.0f states explored\n", iteration, tbdd_satcount(visited, set->variables));
        } else {
            INFO("Level %d done\n", iteration);
        }
        iteration++;
    } while (next_level != tbdd_false);

    set->bdd = visited;
    tbdd_refs_popptr(3);
}

/**
 * Extend a transition relation to a larger domain (using s=s')
 */
#define extend_relation(rel, vars, dom) CALL(extend_relation, rel, vars, dom)
TASK_3(TBDD, extend_relation, TBDD, relation, TBDD, variables, TBDD, totaldom)
{
    /* first determine which state TBDD variables are in rel */
    int has[totalbits];
    for (int i=0; i<totalbits; i++) has[i] = 0;
    TBDD s = variables;
    while (s != tbdd_true) {
        uint32_t v = tbdd_getvar(s);
        if (v/2 >= (unsigned)totalbits) break; // action labels
        has[v/2] = 1;
        s = tbdd_gethigh(s);
    }

    /* create "s=s'" for all variables not in rel */
    TBDD eq = tbdd_true;
    uint32_t nextvar = 0xFFFFF;
    for (int i=totalbits-1; i>=0; i--) {
        if (!has[i]) {
            TBDD low = tbdd_makenode(2*i+1, eq, tbdd_false, nextvar);
            tbdd_refs_push(low);
            TBDD high = tbdd_makenode(2*i+1, tbdd_false, eq, nextvar);
            tbdd_refs_pop(1);
            nextvar = 2*i+1;
            eq = tbdd_makenode(2*i, low, high, nextvar);
        }
        nextvar = 2*i;
    }

    tbdd_refs_push(eq);

    /* extend domain of relation */
    TBDD result = tbdd_extend_domain(relation, variables, totaldom);
    tbdd_refs_push(result);
    result = tbdd_and(result, eq, totaldom);
    tbdd_refs_pop(2);

    return result;
}

/**
 * Compute \BigUnion ( sets[i] )
 */
#define big_union(first, count) CALL(big_union, first, count)
TASK_2(TBDD, big_union, int, first, int, count)
{
    if (count == 1) return next[first]->bdd;

    tbdd_refs_spawn(SPAWN(big_union, first, count/2));
    TBDD right = tbdd_refs_push(CALL(big_union, first+count/2, count-count/2));
    TBDD left = tbdd_refs_push(tbdd_refs_sync(SYNC(big_union)));
    TBDD result = tbdd_or(left, right, next[first]->variables);
    tbdd_refs_pop(2);
    return result;
}

/**
 * Print one row of the transition matrix (for vars)
 */
static void
print_matrix_row(rel_t rel)
{
    int r_i = 0, w_i = 0;
    for (int i=0; i<vectorsize; i++) {
        int s = 0;
        if (r_i < rel->r_k && rel->r_proj[r_i] == i) {
            s |= 1;
            r_i++;
        }
        if (w_i < rel->w_k && rel->w_proj[w_i] == i) {
            s |= 2;
            w_i++;
        }
        if (s == 0) fprintf(stdout, "-");
        else if (s == 1) fprintf(stdout, "r");
        else if (s == 2) fprintf(stdout, "w");
        else if (s == 3) fprintf(stdout, "+");
    }
}

VOID_TASK_0(gc_start)
{
    char buf[32];
    to_h(getCurrentRSS(), buf);
    INFO("(GC) Starting garbage collection... (rss: %s)\n", buf);
}

VOID_TASK_0(gc_end)
{
    char buf[32];
    to_h(getCurrentRSS(), buf);
    INFO("(GC) Garbage collection done.       (rss: %s)\n", buf);
}

int
main(int argc, char **argv)
{
    /**
     * Parse command line, set locale, set startup time for INFO messages.
     */
    argp_parse(&argp, argc, argv, 0, 0, 0);
    setlocale(LC_NUMERIC, "en_US.utf-8");
    t_start = wctime();

    /**
     * Initialize Lace.
     *
     * First: setup with given number of workers (0 for autodetect) and some large size task queue.
     * Second: start all worker threads with default settings.
     * Third: setup local variables using the LACE_ME macro.
     */
    lace_init(workers, 1000000);
    lace_startup(0, NULL, NULL);
    LACE_ME;

    /**
     * Initialize Sylvan.
     *
     * First: set memory limits
     * - 2 GB memory, nodes table twice as big as cache, initial size halved 6x
     *   (that means it takes 6 garbage collections to get to the maximum nodes&cache size)
     * Second: initialize package and subpackages
     * Third: add hooks to report garbage collection
     */
    sylvan_set_limits(2LL<<30, 1, 6);
    sylvan_init_package();
    sylvan_init_tbdd();
    sylvan_gc_hook_pregc(TASK(gc_start));
    sylvan_gc_hook_postgc(TASK(gc_end));

    /**
     * Read the model from file
     */

    /* Open the file */
    FILE *f = fopen(model_filename, "r");
    if (f == NULL) Abort("Cannot open file '%s'!\n", model_filename);

    /* Read domain data */
    if (fread(&vectorsize, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    statebits = (int*)malloc(sizeof(int[vectorsize]));
    if (fread(statebits, sizeof(int), vectorsize, f) != (size_t)vectorsize) Abort("Invalid input file!\n");
    if (fread(&actionbits, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    totalbits = 0;
    for (int i=0; i<vectorsize; i++) totalbits += statebits[i];

    // create variables for a full state vector
    uint32_t vars[totalbits*2];
    for (int i=0; i<totalbits; i++) vars[i] = 2*i;
    vectordom = tbdd_from_array(vars, totalbits);
    tbdd_protect(&vectordom);

    /* Read initial state */
    set_t states = set_load(f);

    /* Read number of transition relations */
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(sizeof(rel_t) * next_count);

    /* Read transition relations */
    for (int i=0; i<next_count; i++) next[i] = rel_load_proj(f);
    for (int i=0; i<next_count; i++) rel_load(next[i], f);

    /* We ignore the reachable states and action labels that are stored after the relations */

    /* Close the file */
    fclose(f);

    /**
     * Pre-processing and some statistics reporting
     */

    if (strategy == 2 || strategy == 3) {
        // for SAT and CHAINING, sort the transition relations (gnome sort because I like gnomes)
        int i = 1, j = 2;
        rel_t t;
        while (i < next_count) {
            rel_t *p = &next[i], *q = p-1;
            if (tbdd_getvar((*q)->variables) > tbdd_getvar((*p)->variables)) {
                t = *q;
                *q = *p;
                *p = t;
                if (--i) continue;
            }
            i = j++;
        }
    }

    INFO("Read file '%s'\n", model_filename);
    INFO("%d integers per state, %d bits per state, %d transition groups\n", vectorsize, totalbits, next_count);

    /* if requested, print the transition matrix */
    if (print_transition_matrix) {
        for (int i=0; i<next_count; i++) {
            INFO(""); // print time prefix
            print_matrix_row(next[i]); // print row
            fprintf(stdout, "\n"); // print newline
        }
    }

    /* merge all relations to one big transition relation if requested */
    if (merge_relations) {
        uint32_t vars[totalbits*2];
        for (int i=0; i<totalbits*2; i++) vars[i] = i;
        TBDD newvars = tbdd_from_array(vars, totalbits*2);
        tbdd_refs_push(newvars);

        INFO("Extending transition relations to full domain.\n");
        for (int i=0; i<next_count; i++) {
            next[i]->bdd = extend_relation(next[i]->bdd, next[i]->variables, newvars);
            next[i]->variables = newvars;
        }
        tbdd_refs_pop(1);

        INFO("Taking union of all transition relations.\n");
        next[0]->bdd = big_union(0, next_count);

        for (int i=1; i<next_count; i++) {
            next[i]->bdd = tbdd_false;
            next[i]->variables = tbdd_true;
        }
        next_count = 1;
    }

    if (report_nodes) {
        INFO("TBDD nodes:\n");
        INFO("Initial states: %zu TBDD nodes\n", tbdd_nodecount(states->bdd));
        for (int i=0; i<next_count; i++) {
            INFO("Transition %d: %zu TBDD nodes\n", i, tbdd_nodecount(next[i]->bdd));
        }
    }

    print_memory_usage();

#ifdef HAVE_PROFILER
    if (profile_filename != NULL) ProfilerStart(profile_filename);
#endif

    if (strategy == 0) {
        double t1 = wctime();
        CALL(bfs, states);
        double t2 = wctime();
        INFO("BFS Time: %f\n", t2-t1);
    } else if (strategy == 1) {
        double t1 = wctime();
        CALL(par, states);
        double t2 = wctime();
        INFO("PAR Time: %f\n", t2-t1);
    } else if (strategy == 2) {
        double t1 = wctime();
        CALL(sat, states);
        double t2 = wctime();
        INFO("SAT Time: %f\n", t2-t1);
    } else if (strategy == 3) {
        double t1 = wctime();
        CALL(chaining, states);
        double t2 = wctime();
        INFO("CHAINING Time: %f\n", t2-t1);
    } else {
        Abort("Invalid strategy set?!\n");
    }

#ifdef HAVE_PROFILER
    if (profile_filename != NULL) ProfilerStop();
#endif

    // Now we just have states
    INFO("Final states: %'0.0f states\n", tbdd_satcount(states->bdd, states->variables));
    if (report_nodes) {
        INFO("Final states: %'zu TBDD nodes\n", tbdd_nodecount(states->bdd));
    }

    print_memory_usage();

    sylvan_stats_report(stdout);

    return 0;
}
