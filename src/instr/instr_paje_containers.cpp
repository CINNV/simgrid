/* Copyright (c) 2010-2017. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "simgrid/s4u/Engine.hpp"
#include "simgrid/s4u/Host.hpp"
#include "src/instr/instr_private.hpp"
#include "surf/surf.h"
#include <unordered_map>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY (instr_paje_containers, instr, "Paje tracing event system (containers)");

extern FILE* tracing_file;
extern std::map<container_t, FILE*> tracing_files; // TI specific
double prefix = 0.0;                               // TI specific

static container_t rootContainer = nullptr;    /* the root container */
static std::unordered_map<std::string, container_t> allContainers; /* all created containers indexed by name */
std::set<std::string> trivaNodeTypes;           /* all host types defined */
std::set<std::string> trivaEdgeTypes;           /* all link types defined */

long long int instr_new_paje_id ()
{
  static long long int type_id = 0;
  return type_id++;
}


namespace simgrid {
namespace instr {

container_t Container::getRootContainer()
{
  return rootContainer;
}

NetZoneContainer::NetZoneContainer(std::string name, unsigned int level, NetZoneContainer* father)
    : Container::Container(name, "", father)
{
  netpoint_ = simgrid::s4u::Engine::getInstance()->getNetpointByNameOrNull(name);
  xbt_assert(netpoint_, "Element '%s' not found", name.c_str());
  if (father_) {
    type_ = father_->type_->getOrCreateContainerType(std::string("L") + std::to_string(level));
    father_->children_.insert({getName(), this});
    logCreation();
  } else {
    type_ = Type::createRootType();
    rootContainer = this;
  }
}

RouterContainer::RouterContainer(std::string name, Container* father) : Container::Container(name, "ROUTER", father)
{
  xbt_assert(father, "Only the Root container has no father");

  netpoint_ = simgrid::s4u::Engine::getInstance()->getNetpointByNameOrNull(name);
  xbt_assert(netpoint_, "Element '%s' not found", name.c_str());

  trivaNodeTypes.insert(type_->getName());
}

HostContainer::HostContainer(simgrid::s4u::Host& host, NetZoneContainer* father)
    : Container::Container(host.getCname(), "HOST", father)
{
  xbt_assert(father, "Only the Root container has no father");

  netpoint_ = host.pimpl_netpoint;
  xbt_assert(netpoint_, "Element '%s' not found", host.getCname());

  trivaNodeTypes.insert(type_->getName());
}

Container::Container(std::string name, std::string type_name, Container* father) : name_(name), father_(father)
{
  static long long int container_id = 0;
  id_                               = container_id; // id (or alias) of the container
  container_id++;

  if (father_) {
    XBT_DEBUG("new container %s, child of %s", name.c_str(), father->name_.c_str());

    if (not type_name.empty()) {
      type_ = father_->type_->getOrCreateContainerType(type_name);
      father_->children_.insert({name_, this});
      logCreation();
    }
  }

  //register all kinds by name
  if (not allContainers.emplace(name_, this).second) {
    THROWF(tracing_error, 1, "container %s already present in allContainers data structure", name_.c_str());
  }

  XBT_DEBUG("Add container name '%s'", name_.c_str());

  //register NODE types for triva configuration
  if (type_name == "LINK")
    trivaNodeTypes.insert(type_->getName());
}

Container::~Container()
{
  XBT_DEBUG("destroy container %s", name_.c_str());
  // Begin with destroying my own children
  for (auto child : children_) {
    delete child.second;
  }

  // obligation to dump previous events because they might reference the container that is about to be destroyed
  TRACE_last_timestamp_to_dump = surf_get_clock();
  TRACE_paje_dump_buffer(true);

  // trace my destruction
  if (not TRACE_disable_destroy() && this != Container::getRootContainer()) {
    // do not trace the container destruction if user requests or if the container is root
    logDestruction();
  }

  // remove me from the allContainers data structure
  allContainers.erase(name_);
}

Container* Container::byNameOrNull(std::string name)
{
  auto cont = allContainers.find(name);
  return cont == allContainers.end() ? nullptr : cont->second;
}

Container* Container::byName(std::string name)
{
  Container* ret = Container::byNameOrNull(name);
  if (ret == nullptr)
    THROWF(tracing_error, 1, "container with name %s not found", name.c_str());

  return ret;
}

void Container::removeFromParent()
{
  if (father_) {
    XBT_DEBUG("removeChildContainer (%s) FromContainer (%s) ", name_.c_str(), father_->name_.c_str());
    father_->children_.erase(name_);
  }
}

void Container::logCreation()
{
  double timestamp = SIMIX_get_clock();
  std::stringstream stream;

  XBT_DEBUG("%s: event_type=%d, timestamp=%f", __FUNCTION__, simgrid::instr::PAJE_CreateContainer, timestamp);

  if (instr_fmt_type == instr_fmt_paje) {
    stream << std::fixed << std::setprecision(TRACE_precision());
    stream << simgrid::instr::PAJE_CreateContainer;
    stream << " ";
    /* prevent 0.0000 in the trace - this was the behavior before the transition to c++ */
    if (timestamp < 1e-12)
      stream << 0;
    else
      stream << timestamp;
    stream << " " << id_ << " " << type_->getId() << " " << father_->id_ << " \"" << name_ << "\"" << std::endl;
    fprintf(tracing_file, "%s", stream.str().c_str());
    XBT_DEBUG("Dump %s", stream.str().c_str());
    stream.str("");
    stream.clear();
  } else if (instr_fmt_type == instr_fmt_TI) {
    // if we are in the mode with only one file
    static FILE* ti_unique_file = nullptr;

    if (tracing_files.empty()) {
      // generate unique run id with time
      prefix = xbt_os_time();
    }

    if (not xbt_cfg_get_boolean("tracing/smpi/format/ti-one-file") || ti_unique_file == nullptr) {
      char* folder_name = bprintf("%s_files", TRACE_get_filename());
      char* filename    = bprintf("%s/%f_%s.txt", folder_name, prefix, name_.c_str());
#ifdef WIN32
      _mkdir(folder_name);
#else
      mkdir(folder_name, S_IRWXU | S_IRWXG | S_IRWXO);
#endif
      ti_unique_file = fopen(filename, "w");
      xbt_assert(ti_unique_file, "Tracefile %s could not be opened for writing: %s", filename, strerror(errno));
      fprintf(tracing_file, "%s\n", filename);

      xbt_free(folder_name);
      xbt_free(filename);
    }

    tracing_files.insert({this, ti_unique_file});
  } else {
    THROW_IMPOSSIBLE;
  }
}

void Container::logDestruction()
{
  std::stringstream stream;
  double timestamp = SIMIX_get_clock();

  XBT_DEBUG("%s: event_type=%d, timestamp=%f", __FUNCTION__, simgrid::instr::PAJE_DestroyContainer, timestamp);

  if (instr_fmt_type == instr_fmt_paje) {
    stream << std::fixed << std::setprecision(TRACE_precision());
    stream << simgrid::instr::PAJE_DestroyContainer;
    stream << " ";
    /* prevent 0.0000 in the trace - this was the behavior before the transition to c++ */
    if (timestamp < 1e-12)
      stream << 0;
    else
      stream << timestamp;
    stream << " " << type_->getId() << " " << id_ << std::endl;
    fprintf(tracing_file, "%s", stream.str().c_str());
    XBT_DEBUG("Dump %s", stream.str().c_str());
    stream.str("");
    stream.clear();
  } else if (instr_fmt_type == instr_fmt_TI) {
    if (not xbt_cfg_get_boolean("tracing/smpi/format/ti-one-file") || tracing_files.size() == 1) {
      FILE* f = tracing_files.at(this);
      fclose(f);
    }
    tracing_files.erase(this);
  } else {
    THROW_IMPOSSIBLE;
  }
}
}
}
