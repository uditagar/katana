// simple galois context and contention manager -*- C++ -*-

#ifndef _GALOIS_RUNTIME_CONTEXT_H
#define _GALOIS_RUNTIME_CONTEXT_H

#include <boost/intrusive/slist.hpp>

#include "Galois/Runtime/SimpleLock.h"

namespace GaloisRuntime {

//All objects that may be locked (nodes primarily) must inherit from Lockable
//Use an intrusive list to track objects in a context without allocation overhead
class LockableListTag;
typedef boost::intrusive::slist_base_hook<boost::intrusive::tag<LockableListTag>,boost::intrusive::link_mode<boost::intrusive::normal_link> > LockableBaseHook;

class Lockable : public LockableBaseHook, public PtrLock<void*, true> {
};

class SimpleRuntimeContext {
  
  typedef boost::intrusive::slist<Lockable, boost::intrusive::base_hook<LockableBaseHook>, boost::intrusive::constant_time_size<false>, boost::intrusive::linear<true> > locksTy;

  //The locks we hold
  locksTy locks;

  //Sanity check for failsafe point
  bool failsafe;

  void acquire_i(Lockable* C) {
    assert(!failsafe && "Acquiring a new lock after failsafe");
    bool suc = C->try_lock();
    if (suc) {
      C->setValue(this);
      locks.push_front(*C);
    } else {
      if (C->getValue() != this)
	throw -1; //CONFLICT
    }
  }

public:
  void start_iteration() {
    assert(locks.empty());
    failsafe = false;
  }
  
  void cancel_iteration() {
    //FIXME: not handled yet
    //abort();
    commit_iteration();
  }
  
  void commit_iteration() {
    //Although the destructor for the list would do the unlink,
    //we do it here since we already are iterating
    while (!locks.empty()) {
      //ORDER MATTERS!
      Lockable& L = locks.front();
      locks.pop_front();
      L.unlock_and_clear();
    }
    failsafe = false;
  }

  void enter_failsafe() {
    failsafe = true;
  }
  
  static void acquire(SimpleRuntimeContext* C, Lockable* L) {
    if (C)
      C->acquire_i(L);
  }

};

extern __thread SimpleRuntimeContext* thread_cnx;

static SimpleRuntimeContext* getThreadContext() {
  //    assert(0);
  return thread_cnx;
}

static __attribute__((unused)) void setThreadContext(SimpleRuntimeContext* n) {
  thread_cnx = n;
}


static __attribute__((unused)) void acquire(Lockable* C) {
  SimpleRuntimeContext* cnx = getThreadContext();
  if (cnx)
    SimpleRuntimeContext::acquire(cnx, C);
}

}

#endif
