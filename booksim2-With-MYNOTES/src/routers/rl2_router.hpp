// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _RL2_ROUTER_HPP_
#define _RL2_ROUTER_HPP_

#include <string>
#include <deque>
#include <queue>
#include <set>
#include <map>

#include "router.hpp"
#include "routefunc.hpp"

using namespace std;

class VC;
class Flit;
class Credit;
class Buffer;
class BufferState;
class Allocator;
class SwitchMonitor;
class BufferMonitor;

class RL2Router : public Router {

  int _vcs;

  int ejectFlitPid;
  int ejectFlitInput;
  int ejectFlitRing;
  bool eject;
  bool ejectFlag;

  int ejectFlitPid_2;
  int ejectFlitInput_2;
  int ejectFlitRing_2;
  bool eject_2;
  bool ejectFlag_2 ;


  static const int infinite = 2147483647;
  vector<pair<int, int> > input_to_output;
  map<int, int> _ringOutput; // a mapping from outputPort to RingID
  map<int, int> _ringInput;
  vector<bool> _isRingBusy;



  int injectFlitPID;
  int injectFlitRing;
  int injectFlitOutput;
  bool inject ;



  bool _active;


  map<int, Flit *> _in_queue_flits;




  deque<pair<int, pair<Credit *, int> > > _proc_credits;


  map<int, Credit *> _out_queue_credits;

  vector<Buffer *> _buf;
  vector<BufferState *> _next_buf;


  tRoutingFunction   _rf;

  int _output_buffer_size;
  vector<queue<Flit *> > _output_buffer;

  vector<queue<Credit *> > _credit_buffer;



#ifdef TRACK_FLOWS
  vector<vector<queue<int> > > _outstanding_classes;
#endif

  bool _ReceiveFlits( );
  bool _ReceiveCredits( );
  void printBuffers();
  virtual void _InternalStep( );

  bool _SWAllocAddReq(int input, int vc, int output);

  void _InputQueuing( );

  void _ClearFlags();
  void _InjectionModule();
  void _InputModule();
  void _EjectionModule() ;
  void _EjectionModule_2() ;

  void _RouteTraffic();
  void _tempRoute();
 
  void _OutputQueuing( );

  void _SendFlits( );
  void _SendCredits( );

  void _UpdateNOQ(int input, int vc, Flit const * f);

  // ----------------------------------------
  //
  //   Router Power Modellingyes
  //
  // ----------------------------------------

  SwitchMonitor * _switchMonitor ;
  BufferMonitor * _bufferMonitor ;

public:

  RL2Router( Configuration const & config,
	    Module *parent, string const & name, int id,
	    int inputs, int outputs );

  virtual ~RL2Router( );

  virtual void AddOutputChannel(FlitChannel * channel, CreditChannel * backchannel);
  virtual void AddInputChannel(FlitChannel * channel, CreditChannel * backchannel);

  virtual void ReadInputs( );
  virtual void WriteOutputs( );

  void Display( ostream & os = cout ) const;

  virtual int GetUsedCredit(int o) const;
  virtual int GetBufferOccupancy(int i) const;

#ifdef TRACK_BUFFERS
  virtual int GetUsedCreditForClass(int output, int cl) const;
  virtual int GetBufferOccupancyForClass(int input, int cl) const;
#endif

  virtual vector<int> UsedCredits() const;
  virtual vector<int> FreeCredits() const;
  virtual vector<int> MaxCredits() const;

  SwitchMonitor const * const GetSwitchMonitor() const {return _switchMonitor;}
  BufferMonitor const * const GetBufferMonitor() const {return _bufferMonitor;}

};

#endif
