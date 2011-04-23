#include "perfmon.hpp"
#include "concurrency/multi_wait.hpp"
#include "arch/arch.hpp"
#include "utils.hpp"
#include <stdarg.h>

/* The var list keeps track of all of the perfmon_t objects. */

intrusive_list_t<perfmon_t> &get_var_list() {
    /* Getter function so that we can be sure that var_list is initialized before it is needed,
    as advised by the C++ FAQ. Otherwise, a perfmon_t might be initialized before the var list
    was initialized. */
    
    static intrusive_list_t<perfmon_t> var_list;
    return var_list;
}

/* The var lock protects the var list when it is being modified. In theory, this should all work
automagically because the constructor of every perfmon_t calls get_var_lock(), causing the var lock
to be constructed before the first perfmon, so it is destroyed after the last perfmon. */

spinlock_t &get_var_lock() {
    /* To avoid static initialization fiasco */
    
    static spinlock_t lock;
    return lock;
}


/* This is the function that actually gathers the stats. It is illegal to create or destroy
perfmon_t objects while perfmon_get_stats is active. */

void co_perfmon_visit(int thread, const std::vector<void*> &data, multi_wait_t *multi_wait) {
    {
        on_thread_t moving(thread);
        int i = 0;
        for (perfmon_t *p = get_var_list().head(); p; p = get_var_list().next(p)) {
            p->visit_stats(data[i++]);
        }
    }
    multi_wait->notify();
}


void perfmon_get_stats(perfmon_stats_t *dest) {
    std::vector<void*> data;
    data.reserve(get_var_list().size());
    for (perfmon_t *p = get_var_list().head(); p; p = get_var_list().next(p)) {
        data.push_back(p->begin_stats());
    }
    int threads = get_num_threads();
    multi_wait_t *multi_wait = new multi_wait_t(threads);
    for (int i = 0; i < threads; i++) {
        coro_t::spawn(boost::bind(co_perfmon_visit, i, data, multi_wait));
    }
    coro_t::wait();
    int i = 0;
    for (perfmon_t *p = get_var_list().head(); p; p = get_var_list().next(p)) {
        p->end_stats(data[i++], dest);
    }
}

/* Constructor and destructor register and deregister the perfmon. */

perfmon_t::perfmon_t()
{
    spinlock_acq_t acq(&get_var_lock());
    get_var_list().push_back(this);
}

perfmon_t::~perfmon_t() {
    spinlock_acq_t acq(&get_var_lock());
    get_var_list().remove(this);
}

bool global_full_perfmon = false;

/* perfmon_counter_t */

perfmon_counter_t::perfmon_counter_t(std::string name)
    : name(name)
{
    for (int i = 0; i < MAX_THREADS; i++) values[i].value = 0;
}

int64_t &perfmon_counter_t::get() {
    return values[get_thread_id()].value;
}

void *perfmon_counter_t::begin_stats() {
    return new cache_line_padded_t<int64_t>[get_num_threads()];
}

void perfmon_counter_t::visit_stats(void *data) {
    ((cache_line_padded_t<int64_t> *)data)[get_thread_id()].value = get();
}

void perfmon_counter_t::end_stats(void *data, perfmon_stats_t *dest) {
    int64_t value = 0;
    for (int i = 0; i < get_num_threads(); i++) value += ((cache_line_padded_t<int64_t> *)data)[i].value;
    (*dest)[name] = format(value);
    delete[] (cache_line_padded_t<int64_t> *)data;
}

/* perfmon_sampler_t */

perfmon_sampler_t::perfmon_sampler_t(std::string name, ticks_t length, bool include_rate)
    : name(name), length(length), include_rate(include_rate) { }

void perfmon_sampler_t::expire() {
    ticks_t now = get_ticks();
    std::deque<sample_t> &queue = values[get_thread_id()];
    while (!queue.empty() && queue.front().timestamp + length < now) queue.pop_front();
}

void perfmon_sampler_t::record(value_t v) {
    expire();
    values[get_thread_id()].push_back(sample_t(v, get_ticks()));
}

struct perfmon_sampler_step_t {
    uint64_t counts[MAX_THREADS];
    perfmon_sampler_t::value_t values[MAX_THREADS], mins[MAX_THREADS], maxes[MAX_THREADS];
};

void *perfmon_sampler_t::begin_stats() {
    return new perfmon_sampler_step_t;
}

void perfmon_sampler_t::visit_stats(void *data) {
    perfmon_sampler_step_t *d = (perfmon_sampler_step_t *)data;
    expire();
    d->values[get_thread_id()] = 0;
    d->counts[get_thread_id()] = 0;
    for (std::deque<perfmon_sampler_t::sample_t>::iterator it = values[get_thread_id()].begin();
         it != values[get_thread_id()].end(); it++) {
        d->values[get_thread_id()] += (*it).value;
        if (d->counts[get_thread_id()] > 0) {
            d->mins[get_thread_id()] = std::min(d->mins[get_thread_id()], (*it).value);
            d->maxes[get_thread_id()] = std::max(d->maxes[get_thread_id()], (*it).value);
        } else {
            d->mins[get_thread_id()] = (*it).value;
            d->maxes[get_thread_id()] = (*it).value;
        }
        d->counts[get_thread_id()]++;
    }
}

void perfmon_sampler_t::end_stats(void *data, perfmon_stats_t *dest) {
    perfmon_sampler_step_t *d = (perfmon_sampler_step_t *)data;
    perfmon_sampler_t::value_t value = 0;
    uint64_t count = 0;
    perfmon_sampler_t::value_t min = 0, max = 0;   /* Initializers to make GCC shut up */
    bool have_any = false;
    for (int i = 0; i < get_num_threads(); i++) {
        value += d->values[i];
        count += d->counts[i];
        if (d->counts[i]) {
            if (have_any) {
                min = std::min(d->mins[i], min);
                max = std::max(d->maxes[i], max);
            } else {
                min = d->mins[i];
                max = d->maxes[i];
                have_any = true;
            }
        }
    }
    if (have_any) {
        (*dest)[name + "_avg"] = format(value / count);
        (*dest)[name + "_min"] = format(min);
        (*dest)[name + "_max"] = format(max);
    } else {
        (*dest)[name + "_avg"] = "-";
        (*dest)[name + "_min"] = "-";
        (*dest)[name + "_max"] = "-";
    }
    if (include_rate) {
        (*dest)[name + "_persec"] = format(count / ticks_to_secs(length));
    }
    delete d;
};

/* perfmon_function_t */

perfmon_function_t::internal_function_t::internal_function_t(perfmon_function_t *p)
    : parent(p)
{
    parent->funs[get_thread_id()].push_back(this);
}

perfmon_function_t::internal_function_t::~internal_function_t() {
    parent->funs[get_thread_id()].remove(this);
}

void *perfmon_function_t::begin_stats() {
    return reinterpret_cast<void*>(new std::string());
}

void perfmon_function_t::visit_stats(void *data) {
    std::string *string = reinterpret_cast<std::string*>(data);
    for (internal_function_t *f = funs[get_thread_id()].head(); f; f = funs[get_thread_id()].next(f)) {
        if (string->size() > 0) (*string) += ", ";
        (*string) += f->compute_stat();
    }
}

void perfmon_function_t::end_stats(void *data, perfmon_stats_t *dest) {
    std::string *string = reinterpret_cast<std::string*>(data);
    if (string->size()) (*dest)[name] = *string;
    else (*dest)[name] = "N/A";
    delete string;
}
