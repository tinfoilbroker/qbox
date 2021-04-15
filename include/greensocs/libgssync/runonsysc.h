/*
 *  Copyright (C) 2020  GreenSocs
 *
 */

#ifndef RUNONSYSTEMC_H
#define RUNONSYSTEMC_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <future>

#include "greensocs/libgssync/async_event.h"

namespace gs {
    class RunOnSysC : public sc_core::sc_module {
    protected:
        class AsyncJob {
        private:
            std::function<void ()> m_job;
            std::packaged_task<void ()> m_task;

            bool m_cancelled = false;

            void job_entry()
            {
                if (m_cancelled) {
                    return;
                }

                m_job();
            }

            void run_job()
            {
                m_task();
            }

        public:
            AsyncJob(std::function<void()> &&job)
                : m_job(std::move(job))
                , m_task(std::bind(&AsyncJob::job_entry, this))
            {}

            AsyncJob(std::function<void()> &job)
                : m_job(job)
                , m_task(std::bind(&AsyncJob::job_entry, this))
            {}

            AsyncJob() = delete;
            AsyncJob(const AsyncJob&) = delete;

            void operator()()
            {
                m_task();
            }

            void cancel()
            {
// A (hung) job can be cancelled by re-running the job in a cancelled state, the
// tasks that wraps the job will then terminate (and any future.wait will terminate).
// Note - if the 'hung' job were to ever re-start, it's side effects would be seen.
                m_cancelled = true;  

                run_job();
            }

            void wait()
            {
                auto future = m_task.get_future();

                future.wait();
                future.get();
            }

            bool is_cancelled() const
            {
                return m_cancelled;
            }
        };

        std::thread::id m_thread_id;

        /* Async job queue */
        std::queue< std::shared_ptr<AsyncJob> > m_async_jobs;
        std::mutex m_async_jobs_mutex;

        async_event m_jobs_handler_event;

        // Process inside a thread incase the job calls wait
        void jobs_handler() {
            std::unique_lock<std::mutex> lock(m_async_jobs_mutex);
            for (;;) {
                while (!m_async_jobs.empty()) {
                    std::shared_ptr<AsyncJob> job = m_async_jobs.front();
                    m_async_jobs.pop();

                    lock.unlock();
                    sc_core::sc_unsuspendable(); // a wait in the job will cause systemc time to advance
                    (*job)();
                    sc_core::sc_suspendable();
                    lock.lock();
                }

                lock.unlock();
                wait(m_jobs_handler_event);
                lock.lock();
            }
        }

    public:
        RunOnSysC(const sc_core::sc_module_name &n = sc_core::sc_module_name("run-on-sysc"))
            : sc_module(n)
            , m_thread_id(std::this_thread::get_id())
            , m_jobs_handler_event(false) // starve if no more jobs provided
        {
            SC_HAS_PROCESS(RunOnSysC);
            SC_THREAD(jobs_handler);
        }

        /**
         * @brief Cancel all pending jobs
         *
         * @detail Cancel all the pending jobs. The callers will be unblocked
         *         if they are waiting for the job.
         */
        void cancel_all()
        {
            std::lock_guard<std::mutex> lock(m_async_jobs_mutex);

            while (!m_async_jobs.empty()) {
                m_async_jobs.front()->cancel();
                m_async_jobs.pop();
            }
        }

        void end_of_simulation()
        {
            cancel_all();
        }

        void fork_on_systemc(std::function<void()> job_entry)
        {
            run_on_sysc(job_entry, false);
        }

        /**
         * @brief Run a job on the SystemC kernel thread
         *
         * @param[in] job_entry The job to run
         * @param[in] wait If true, wait for job completion
         *
         * @return true if the job has been succesfully executed or if `wait`
         *         was false, false if it has been cancelled (see
         *         `RunOnSysC::cancel_all`).
         */
        bool run_on_sysc(std::function<void()> job_entry, bool wait = true)
        {
            if (std::this_thread::get_id() == m_thread_id) {
                job_entry();
                return true;
            } else {
                std::shared_ptr<AsyncJob> job(new AsyncJob(job_entry));

                {
                    std::lock_guard<std::mutex> lock(m_async_jobs_mutex);
                    m_async_jobs.push(job);
                }

                m_jobs_handler_event.async_notify();

                if (wait) {
                    /* Wait for job completion */
                    job->wait();
                    return !job->is_cancelled();
                }

                return true;
            }
        }
    };
}
#endif // RUNONSYSTEMC_H
