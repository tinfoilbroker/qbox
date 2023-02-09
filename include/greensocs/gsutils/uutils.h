/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version, or under the
 * Apache License, Version 2.0 (the "License”) at your discretion.
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA. You may obtain a copy of the Apache License at
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef _GREENSOCS_BASE_COMPONENTS_UUTILS
#define _GREENSOCS_BASE_COMPONENTS_UUTILS

#ifndef WIN32

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <csignal>
#include <signal.h>
#include <cstdlib>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <cassert>
#include <iostream>
#include <thread>
#include <map>
#include <utility>
#include <functional>
#include <vector>
#include <atomic>

#define QUIT_SIG_VAL    -2
#define DEFAULT_SIG_VAL -1

namespace gs {

class SigHandler
{
public:
    static SigHandler& get() {
        static SigHandler sh;
        return sh;
    }

    enum class Handler_CB {
        EXIT, //  exit
        PASS, // run signal_handler
        IGN,  // ignore
        DFL,  // default
    };

    static void exit_sig_handler(int sig) {
        gs::SigHandler::get().set_sig_num(sig);
        std::exit(EXIT_SUCCESS);
    }

    static void pass_sig_handler(int sig) {
        gs::SigHandler::get().set_sig_num(sig);
        char ch[1] = { 's' };
        if (::write(gs::SigHandler::get().get_write_sock_end(), &ch, 1) == -1 && errno != EAGAIN)
            std::exit(EXIT_FAILURE);
    }

    inline void add_sig_handler(int signum, Handler_CB s_cb = Handler_CB::EXIT) {
        auto it = m_signals.find(signum);
        if (it != m_signals.end())
            m_signals.at(signum) = s_cb;
        else {
            auto ret = m_signals.insert(std::make_pair(signum, s_cb));
            if (!ret.second) {
                std::cerr << "Failed to insert signal: " << signum << std::endl;
                exit(EXIT_FAILURE);
            }
        }
        _update_sig_cb(signum, s_cb);
    }

    typedef void (*at_exit_cb)();
    inline void register_on_exit_cb(at_exit_cb e_cb) {
        assert(e_cb);
        std::atexit(e_cb);
    }

    inline void add_to_block_set(int signum) { sigaddset(&m_sigs_to_block, signum); }

    inline void reset_block_set() { sigemptyset(&m_sigs_to_block); }

    inline void block_signal_set() { sigprocmask(SIG_BLOCK, &m_sigs_to_block, NULL); }

    /**
     *  SIGCHLD will not be sent when child stopped or continued,
     *  it will be sent only when killed.
     */
    void set_nosig_chld_stop() { exit_act.sa_flags = SA_NOCLDSTOP; }

    /**
     * Block all the signals previously added to the class instance
     * by default in the class constructor or by calling add_sig_handler().
     * All signals added to the block list previously using add_to_block_set()
     * will be removed first.
     */
    inline void block_curr_handled_signals() {
        reset_block_set();
        for (auto sig_cb_pair : m_signals)
            add_to_block_set(sig_cb_pair.first);
        block_signal_set();
    }

    inline void register_handler(std::function<void(int)> handler) {
        handlers.push_back(handler);
        if (!is_pass_handler_requested) {
            _start_pass_signal_handler();
            is_pass_handler_requested = true;
        }
    }

    inline int get_write_sock_end() const { return self_sockpair_fd[1]; }

    inline void set_sig_num(int val) { sig_num = val; }

    ~SigHandler() {
        close(self_sockpair_fd[0]); // this should terminate the pass_handler thread.
        close(self_sockpair_fd[1]);
        if (pass_handler.joinable())
            pass_handler.join();
    }

private:
    void _start_pass_signal_handler() {
        pass_handler = std::thread([this]() {
            self_pipe_monitor.fd = self_sockpair_fd[0];
            self_pipe_monitor.events = POLLIN;
            int ret;
            while (!stop_running) {
                ret = poll(&self_pipe_monitor, 1, 300);
                if ((ret == -1 && errno == EINTR) || (ret == 0) /*timeout*/)
                    continue;
                else if (self_pipe_monitor.revents & POLLHUP) {
                    return;
                } else if (self_pipe_monitor.revents & POLLIN) {
                    char ch[1];
                    if (read(self_pipe_monitor.fd, ch, 1) == -1) {
                        perror("pipe read");
                        exit(EXIT_FAILURE);
                    }
                    for (auto handler : handlers)
                        handler(sig_num);
                } else {
                    exit(EXIT_FAILURE);
                }
            }
        });
    }

    SigHandler()
        : m_signals{ { SIGINT, Handler_CB::EXIT },  { SIGTERM, Handler_CB::EXIT },
                     { SIGQUIT, Handler_CB::EXIT }, { SIGSEGV, Handler_CB::EXIT },
                     { SIGABRT, Handler_CB::EXIT }, { SIGBUS, Handler_CB::EXIT } }
        , is_pass_handler_requested{ false }
        , stop_running{ false }
        , exit_act{ 0 }
        , pass_act{ 0 }
        , ign_act{ 0 }
        , dfl_act{ 0 } {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, self_sockpair_fd) == -1) {
            perror("SigHnadler socketpair");
            std::exit(EXIT_FAILURE);
        }
        reset_block_set();
        _update_all_sigs_cb();
    }

    SigHandler(const SigHandler&) = delete;
    SigHandler& operator()(const SigHandler&) = delete;

    inline void _update_sig_cb(int signum, Handler_CB s_cb) {
        switch (s_cb) {
        case Handler_CB::EXIT:
            exit_act.sa_handler = SigHandler::exit_sig_handler;
            sigaction(signum, &exit_act, NULL);
            break;
        case Handler_CB::PASS:
            pass_act.sa_handler = SigHandler::pass_sig_handler;
            sigaction(signum, &pass_act, NULL);
            break;
        case Handler_CB::IGN:
            ign_act.sa_handler = SIG_IGN;
            sigaction(signum, &ign_act, NULL);
            break;
        case Handler_CB::DFL:
            dfl_act.sa_handler = SIG_DFL;
            sigaction(signum, &dfl_act, NULL);
            break;
        }
    }

    inline void _update_all_sigs_cb() {
        for (auto sig_cb_pair : m_signals) {
            _update_sig_cb(sig_cb_pair.first, sig_cb_pair.second);
        }
    }

private:
    std::map<int, Handler_CB> m_signals;
    sigset_t m_sigs_to_block;
    struct pollfd self_pipe_monitor;
    std::thread pass_handler;
    std::vector<std::function<void(int)>> handlers;
    bool is_pass_handler_requested;
    std::atomic_bool stop_running;
    std::atomic_int32_t sig_num;
    int self_sockpair_fd[2];
    struct sigaction exit_act;
    struct sigaction pass_act;
    struct sigaction ign_act;
    struct sigaction dfl_act;
};

class ProcAliveHandler
{
public:
    ProcAliveHandler(): m_is_ppid_set{ false }, m_is_parent_setup_called{ false } {}

    ~ProcAliveHandler() {
        close(m_sock_pair_fds[0]);
        close(m_sock_pair_fds[1]);
        if (parent_alive_checker.joinable())
            parent_alive_checker.join();
        if (child_waiter.joinable())
            child_waiter.join();
    }

    void wait_child(pid_t cpid) {
        child_waiter = std::thread(
            [](pid_t cpid) {
                pid_t m_wpid;
                int wstatus;
                for (;;) {
                    m_wpid = waitpid(cpid, &wstatus, 0);
                    if (m_wpid == -1) {
                        exit(EXIT_FAILURE);
                    }
                    if (WIFEXITED(wstatus)) {
                        std::cerr << "process (" << m_wpid
                                  << ") exited, status= " << WEXITSTATUS(wstatus) << std::endl;
                        break;
                    }
                } // end for
            },
            cpid); // end lambda
    }              // wait_child()

    inline void recv_sockpair_fds_from_remote(int fd0, int fd1) {
        m_sock_pair_fds[0] = fd0;
        m_sock_pair_fds[1] = fd1;
    }

    inline int get_sockpair_fd0() const { return m_sock_pair_fds[0]; }

    inline int get_sockpair_fd1() const { return m_sock_pair_fds[1]; }

    inline pid_t get_ppid() const {
        if (m_is_ppid_set)
            return m_ppid;
        else
            return -1;
    }

    inline void init_peer_conn_checker() {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, m_sock_pair_fds) == -1) {
            perror("ProcAliveHandler socketpair");
            std::exit(EXIT_FAILURE);
        }
    }

    inline void setup_parent_conn_checker() {
        m_is_parent_setup_called = true;
        close(m_sock_pair_fds[1]);
    }

    /**
     * same thread parent connected checker
     */
    inline void check_parent_conn_sth(std::function<void()> on_parent_exit) {
        _set_ppid();
        close(m_sock_pair_fds[0]);
        struct pollfd parent_connected_monitor;
        parent_connected_monitor.fd = m_sock_pair_fds[1];
        parent_connected_monitor.events = POLLIN;
        int ret = 0;
        for (;;) {
            ret = poll(&parent_connected_monitor, 1, -1);
            if (ret == -1 && errno == EINTR)
                continue;
            else if (parent_connected_monitor.revents & POLLHUP) {
                on_parent_exit();
                exit(EXIT_SUCCESS);
            } else {
                perror("poll");
                exit(EXIT_FAILURE);
            }
        }
    }

    /**
     * new thread parent connected checker
     */
    void check_parent_conn_nth(std::function<void()> on_parent_exit) {
        parent_alive_checker = std::thread(&ProcAliveHandler::check_parent_conn_sth, this,
                                           on_parent_exit);
    }

private:
    inline void _set_ppid() {
        m_ppid = getppid();
        m_is_ppid_set = true;
    }

private:
    pid_t m_ppid;
    std::thread child_waiter;
    std::thread parent_alive_checker;
    int m_sock_pair_fds[2];
    bool m_is_ppid_set;
    bool m_is_parent_setup_called;
};

} // namespace gs

#endif // WIN32

#endif // _GREENSOCS_BASE_COMPONENTS_UUTILS