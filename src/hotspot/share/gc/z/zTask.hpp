/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef SHARE_GC_Z_ZTASK_HPP
#define SHARE_GC_Z_ZTASK_HPP

#include "gc/shared/workerThread.hpp"
#include "memory/allocation.hpp"

class ZTask : public StackObj {
private:
  class Task : public WorkerTask {
  private:
    ZTask* const _task;

  public:
    Task(ZTask* task, const char* name);

    virtual void work(uint worker_id);
  };

  Task _worker_task;

public:
  ZTask(const char* name);

  const char* name() const;
  WorkerTask* worker_task();

  virtual void work() = 0;
};

class ZRestartableTask : public ZTask {
public:
  ZRestartableTask(const char* name);
  virtual void resize_workers(uint nworkers);
};

#endif // SHARE_GC_Z_ZTASK_HPP
