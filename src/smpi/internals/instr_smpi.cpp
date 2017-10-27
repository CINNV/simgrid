/* Copyright (c) 2010, 2012-2017. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "private.hpp"
#include <boost/algorithm/string.hpp>
#include <cctype>
#include <cstdarg>
#include <cwchar>
#include <deque>
#include <simgrid/sg_config.h>
#include <string>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(instr_smpi, instr, "Tracing SMPI");

static std::unordered_map<std::string, std::deque<std::string>*> keys;

static const char* smpi_colors[] = {
    "recv",      "1 0 0",       "irecv",         "1 0.52 0.52",    "send",       "0 0 1",
    "isend",     "0.52 0.52 1", "sendrecv",      "0 1 1",          "wait",       "1 1 0",
    "waitall",   "0.78 0.78 0", "waitany",       "0.78 0.78 0.58", "test",       "0.52 0.52 0",

    "allgather", "1 0 0",       "allgatherv",    "1 0.52 0.52",    "allreduce",  "1 0 1",
    "alltoall",  "0.52 0 1",    "alltoallv",     "0.78 0.52 1",    "barrier",    "0 0.78 0.78",
    "bcast",     "0 0.78 0.39", "gather",        "1 1 0",          "gatherv",    "1 1 0.52",
    "reduce",    "0 1 0",       "reducescatter", "0.52 1 0.52",    "scan",       "1 0.58 0.23",
    "exscan",    "1 0.54 0.25", "scatterv",      "0.52 0 0.52",    "scatter",    "1 0.74 0.54",

    "computing", "0 1 1",       "sleeping",      "0 0.5 0.5",

    "init",      "0 1 0",       "finalize",      "0 1 0",

    "put",       "0.3 1 0",     "get",           "0 1 0.3",        "accumulate", "1 0.3 0",
    "win_fence", "1 0 0.3",     "win_post",      "1 0 0.8",        "win_wait",   "1 0.8 0",
    "win_start", "0.8 0 1",     "win_complete",  "0.8 1 0",        nullptr,      nullptr,
};

static const char* instr_find_color(const char* state)
{
  std::string target = std::string(state);
  boost::algorithm::to_lower(target);
  const char* ret     = nullptr;
  unsigned int i      = 0;
  const char* current = smpi_colors[i];
  while (current != nullptr) {
    if (target == current                          // exact match
        || strstr(target.c_str(), current) != 0) { // as substring
      ret = smpi_colors[i + 1];
      break;
    }
    i+=2;
    current = smpi_colors[i];
  }
  return ret;
}

XBT_PRIVATE std::string smpi_container(int rank)
{
  return std::string("rank-") + std::to_string(rank);
}

static std::string TRACE_smpi_put_key(int src, int dst, int tag, int send)
{
  // get the deque for src#dst
  std::string aux =
      std::to_string(src) + "#" + std::to_string(dst) + "#" + std::to_string(tag) + "#" + std::to_string(send);
  auto it = keys.find(aux);
  std::deque<std::string>* d;

  if (it == keys.end()) {
    d         = new std::deque<std::string>;
    keys[aux] = d;
  } else
    d = it->second;

  //generate the key
  static unsigned long long counter = 0;
  counter++;
  std::string key =
      std::to_string(src) + "_" + std::to_string(dst) + "_" + std::to_string(tag) + "_" + std::to_string(counter);

  //push it
  d->push_back(key);

  return key;
}

static std::string TRACE_smpi_get_key(int src, int dst, int tag, int send)
{
  std::string key;
  std::string aux = std::to_string(src) + "#" + std::to_string(dst) + "#" + std::to_string(tag) + "#" +
                    std::to_string(send == 1 ? 0 : 1);
  auto it = keys.find(aux);
  if (it == keys.end()) {
    // first posted
    key = TRACE_smpi_put_key(src, dst, tag, send);
  } else {
    key = it->second->front();
    it->second->pop_front();
  }
  return key;
}

static std::unordered_map<smx_actor_t, std::string> process_category;

static void cleanup_extra_data (instr_extra_data extra){
  if(extra!=nullptr){
    if(extra->sendcounts!=nullptr)
      xbt_free(extra->sendcounts);
    if(extra->recvcounts!=nullptr)
      xbt_free(extra->recvcounts);
    xbt_free(extra);
  }
}

void TRACE_internal_smpi_set_category (const char *category)
{
  if (not TRACE_smpi_is_enabled())
    return;

  //declare category
  TRACE_category (category);

  if (category != nullptr)
    process_category[SIMIX_process_self()] = category;
}

const char *TRACE_internal_smpi_get_category ()
{
  if (not TRACE_smpi_is_enabled())
    return nullptr;

  auto it = process_category.find(SIMIX_process_self());
  return (it == process_category.end()) ? nullptr : it->second.c_str();
}

void TRACE_smpi_alloc()
{
  // for symmetry
}

void TRACE_smpi_release()
{
  for (auto const& elm : keys)
    delete elm.second;
}

void TRACE_smpi_init(int rank)
{
  if (not TRACE_smpi_is_enabled())
    return;

  std::string str = smpi_container(rank);

  container_t father;
  if (TRACE_smpi_is_grouped()){
    father = simgrid::instr::Container::byNameOrNull(sg_host_self_get_name());
  }else{
    father = simgrid::instr::Container::getRootContainer();
  }
  xbt_assert(father != nullptr, "Could not find a parent for mpi rank %s at function %s", str.c_str(), __FUNCTION__);
#if HAVE_PAPI
  container_t container =
#endif
      new simgrid::instr::Container(str, "MPI", father);
#if HAVE_PAPI
  papi_counter_t counters = smpi_process()->papi_counters();

  for (auto const& it : counters) {
    /**
     * Check whether this variable already exists or not. Otherwise, it will be created
     * multiple times but only the last one would be used...
     */
    if (s_type::getOrNull(it.first.c_str(), container->type_) == nullptr) {
      Type::variableNew(it.first.c_str(), "", container->type_);
    }
  }
#endif
}

void TRACE_smpi_finalize(int rank)
{
  if (not TRACE_smpi_is_enabled())
    return;

  container_t container = simgrid::instr::Container::byName(smpi_container(rank));
  container->removeFromParent();
  delete container;
}

void TRACE_smpi_collective_in(int rank, const char *operation, instr_extra_data extra)
{
  if (not TRACE_smpi_is_enabled()) {
    cleanup_extra_data(extra);
    return;
  }

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  const char *color = instr_find_color (operation);
  state->addEntityValue(operation, color);
  state->pushEvent(SIMIX_get_clock(), container, operation, static_cast<void*>(extra));
}

void TRACE_smpi_collective_out(int rank, const char *operation)
{
  if (not TRACE_smpi_is_enabled())
    return;

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->popEvent(SIMIX_get_clock(), container);
}

void TRACE_smpi_computing_init(int rank)
{
 //first use, initialize the color in the trace
 if (not TRACE_smpi_is_enabled() || not TRACE_smpi_is_computing())
   return;

 container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
 simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
 state->addEntityValue("computing", instr_find_color("computing"));
 state->pushEvent(SIMIX_get_clock(), container, "computing");
}

void TRACE_smpi_computing_in(int rank, instr_extra_data extra)
{
  //do not forget to set the color first, otherwise this will explode
  if (not TRACE_smpi_is_enabled() || not TRACE_smpi_is_computing()) {
    cleanup_extra_data(extra);
    return;
  }

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->addEntityValue("computing");
  state->pushEvent(SIMIX_get_clock(), container, "computing", static_cast<void*>(extra));
}

void TRACE_smpi_computing_out(int rank)
{
  if (not TRACE_smpi_is_enabled() || not TRACE_smpi_is_computing())
    return;

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->popEvent(SIMIX_get_clock(), container);
}

void TRACE_smpi_sleeping_init(int rank)
{
  //first use, initialize the color in the trace
  if (not TRACE_smpi_is_enabled() || not TRACE_smpi_is_sleeping())
    return;

  container_t container       = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->addEntityValue("sleeping", instr_find_color("sleeping"));
  state->pushEvent(SIMIX_get_clock(), container, "sleeping");
}

void TRACE_smpi_sleeping_in(int rank, instr_extra_data extra)
{
  //do not forget to set the color first, otherwise this will explode
  if (not TRACE_smpi_is_enabled() || not TRACE_smpi_is_sleeping()) {
    cleanup_extra_data(extra);
    return;
  }

  container_t container       = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->addEntityValue("sleeping");
  state->pushEvent(SIMIX_get_clock(), container, "sleeping", static_cast<void*>(extra));
}

void TRACE_smpi_sleeping_out(int rank)
{
  if (not TRACE_smpi_is_enabled() || not TRACE_smpi_is_sleeping())
    return;

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->popEvent(SIMIX_get_clock(), container);
}

void TRACE_smpi_testing_in(int rank, instr_extra_data extra)
{
  //do not forget to set the color first, otherwise this will explode
  if (not TRACE_smpi_is_enabled()) {
    cleanup_extra_data(extra);
    return;
  }

  container_t container       = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->addEntityValue("test");
  state->pushEvent(SIMIX_get_clock(), container, "test", static_cast<void*>(extra));
}

void TRACE_smpi_testing_out(int rank)
{
  if (not TRACE_smpi_is_enabled())
    return;

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->popEvent(SIMIX_get_clock(), container);
}

void TRACE_smpi_ptp_in(int rank, const char *operation, instr_extra_data extra)
{
  if (not TRACE_smpi_is_enabled()) {
    cleanup_extra_data(extra);
    return;
  }

  container_t container       = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->addEntityValue(operation, instr_find_color(operation));
  state->pushEvent(SIMIX_get_clock(), container, operation, static_cast<void*>(extra));
}

void TRACE_smpi_ptp_out(int rank, int dst, const char *operation)
{
  if (not TRACE_smpi_is_enabled())
    return;

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::StateType* state = static_cast<simgrid::instr::StateType*>(container->type_->byName("MPI_STATE"));
  state->popEvent(SIMIX_get_clock(), container);
}

void TRACE_smpi_send(int rank, int src, int dst, int tag, int size)
{
  if (not TRACE_smpi_is_enabled())
    return;

  std::string key = TRACE_smpi_get_key(src, dst, tag, 1);

  container_t container      = simgrid::instr::Container::byName(smpi_container(rank));
  simgrid::instr::Type* type = simgrid::instr::Type::getRootType()->byName("MPI_LINK");
  XBT_DEBUG("Send tracing from %d to %d, tag %d, with key %s", src, dst, tag, key.c_str());
  new simgrid::instr::StartLinkEvent(SIMIX_get_clock(), simgrid::instr::Container::getRootContainer(), type, container,
                                     "PTP", key, size);
}

void TRACE_smpi_recv(int src, int dst, int tag)
{
  if (not TRACE_smpi_is_enabled())
    return;

  std::string key = TRACE_smpi_get_key(src, dst, tag, 0);

  container_t container      = simgrid::instr::Container::byName(smpi_container(dst));
  simgrid::instr::Type* type = simgrid::instr::Type::getRootType()->byName("MPI_LINK");
  XBT_DEBUG("Recv tracing from %d to %d, tag %d, with key %s", src, dst, tag, key.c_str());
  new simgrid::instr::EndLinkEvent(SIMIX_get_clock(), simgrid::instr::Container::getRootContainer(), type, container,
                                   "PTP", key);
}
