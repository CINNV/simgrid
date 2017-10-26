/* Copyright (c) 2010, 2012-2017. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "mc/mc.h"
#include "src/instr/instr_private.hpp"
#include "src/msg/msg_private.hpp"

#include <atomic>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(instr_msg, instr, "MSG instrumentation");

void TRACE_msg_set_task_category(msg_task_t task, const char *category)
{
  xbt_assert(task->category == nullptr, "Task %p(%s) already has a category (%s).",
      task, task->name, task->category);

  //if user provides a nullptr category, task is no longer traced
  if (category == nullptr) {
    xbt_free (task->category);
    task->category = nullptr;
    XBT_DEBUG("MSG task %p(%s), category removed", task, task->name);
    return;
  }

  //set task category
  task->category = xbt_strdup (category);
  XBT_DEBUG("MSG task %p(%s), category %s", task, task->name, task->category);
}

/* MSG_task_create related function*/
void TRACE_msg_task_create(msg_task_t task)
{
  static std::atomic_ullong counter{0};
  task->counter = counter++;
  task->category = nullptr;

  if(MC_is_active())
    MC_ignore_heap(&(task->counter), sizeof(task->counter));

  XBT_DEBUG("CREATE %p, %lld", task, task->counter);
}

/* MSG_task_execute related functions */
void TRACE_msg_task_execute_start(msg_task_t task)
{
  XBT_DEBUG("EXEC,in %p, %lld, %s", task, task->counter, task->category);

  if (TRACE_msg_process_is_enabled()){
    container_t process_container = simgrid::instr::Container::byName(instr_process_id(MSG_process_self()));
    simgrid::instr::StateType* state =
        static_cast<simgrid::instr::StateType*>(process_container->type_->byName("MSG_PROCESS_STATE"));
    simgrid::instr::Value* val = state->getEntityValue("task_execute");
    new simgrid::instr::PushStateEvent(MSG_get_clock(), process_container, state, val);
  }
}

void TRACE_msg_task_execute_end(msg_task_t task)
{
  XBT_DEBUG("EXEC,out %p, %lld, %s", task, task->counter, task->category);

  if (TRACE_msg_process_is_enabled()){
    container_t process_container = simgrid::instr::Container::byName(instr_process_id(MSG_process_self()));
    simgrid::instr::Type* type    = process_container->type_->byName("MSG_PROCESS_STATE");
    new simgrid::instr::PopStateEvent(MSG_get_clock(), process_container, type);
  }
}

/* MSG_task_destroy related functions */
void TRACE_msg_task_destroy(msg_task_t task)
{
  XBT_DEBUG("DESTROY %p, %lld, %s", task, task->counter, task->category);

  //free category
  xbt_free(task->category);
  task->category = nullptr;
}

/* MSG_task_get related functions */
void TRACE_msg_task_get_start()
{
  XBT_DEBUG("GET,in");

  if (TRACE_msg_process_is_enabled()){
    container_t process_container = simgrid::instr::Container::byName(instr_process_id(MSG_process_self()));
    simgrid::instr::StateType* state =
        static_cast<simgrid::instr::StateType*>(process_container->type_->byName("MSG_PROCESS_STATE"));
    simgrid::instr::Value* val    = state->getEntityValue("receive");
    new simgrid::instr::PushStateEvent(MSG_get_clock(), process_container, state, val);
  }
}

void TRACE_msg_task_get_end(double start_time, msg_task_t task)
{
  XBT_DEBUG("GET,out %p, %lld, %s", task, task->counter, task->category);

  if (TRACE_msg_process_is_enabled()){
    container_t process_container = simgrid::instr::Container::byName(instr_process_id(MSG_process_self()));
    simgrid::instr::Type* type    = process_container->type_->byName("MSG_PROCESS_STATE");
    new simgrid::instr::PopStateEvent(MSG_get_clock(), process_container, type);

    std::string key = std::string("p") + std::to_string(task->counter);
    type = simgrid::instr::Type::getRootType()->byName("MSG_PROCESS_TASK_LINK");
    new simgrid::instr::EndLinkEvent(MSG_get_clock(), simgrid::instr::Container::getRootContainer(), type,
                                     process_container, "SR", key);
  }
}

/* MSG_task_put related functions */
int TRACE_msg_task_put_start(msg_task_t task)
{
  XBT_DEBUG("PUT,in %p, %lld, %s", task, task->counter, task->category);

  if (TRACE_msg_process_is_enabled()){
    container_t process_container = simgrid::instr::Container::byName(instr_process_id(MSG_process_self()));
    simgrid::instr::StateType* state =
        static_cast<simgrid::instr::StateType*>(process_container->type_->byName("MSG_PROCESS_STATE"));
    simgrid::instr::Value* val = state->getEntityValue("send");
    new simgrid::instr::PushStateEvent(MSG_get_clock(), process_container, state, val);

    std::string key = std::string("p") + std::to_string(task->counter);
    simgrid::instr::LinkType* type =
        static_cast<simgrid::instr::LinkType*>(simgrid::instr::Type::getRootType()->byName("MSG_PROCESS_TASK_LINK"));
    new simgrid::instr::StartLinkEvent(MSG_get_clock(), simgrid::instr::Container::getRootContainer(), type,
                                       process_container, "SR", key);
  }

  return 1;
}

void TRACE_msg_task_put_end()
{
  XBT_DEBUG("PUT,out");

  if (TRACE_msg_process_is_enabled()){
    container_t process_container = simgrid::instr::Container::byName(instr_process_id(MSG_process_self()));
    simgrid::instr::Type* type    = process_container->type_->byName("MSG_PROCESS_STATE");
    new simgrid::instr::PopStateEvent(MSG_get_clock(), process_container, type);
  }
}
