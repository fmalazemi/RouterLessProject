// $Id: trafficmanager.hpp 5188 2012-08-30 00:31:31Z dub $

/*
 Copyright (c) 2007-2012, Trustees of The Leland Stanford Junior University
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

#ifndef _FES2_TRAFFICMANAGER_HPP_
#define _FES2_TRAFFICMANAGER_HPP_


#include "module.hpp"
#include "config_utils.hpp"
#include "network.hpp"
#include "flit.hpp"
#include "buffer_state.hpp"
#include "stats.hpp"
#include "traffic.hpp"
#include "routefunc.hpp"
#include "outputset.hpp"
#include "injection.hpp"
#include "trafficmanager.hpp"
#include "fes2_interface.hpp"



class FeS2TrafficManager : public TrafficManager {

private:
	  struct FeS2PayLoad {
		  int fes2_id;
		  int fes2_subnetwork;
	  };

	  int  _flit_width;
	  int _ideal_interconnect;

	  TraceGenerator *_time_trace;
protected:

  FeS2Interface *_fes2_interface;


  virtual void _RetireFlit( Flit *f, int dest );

  virtual void _Inject();
  virtual void _Step( );
  
  void _GeneratePacket( int source, int size, int cl, int time, int fes2_id, int subnetwork, int destination  );


  virtual bool _SingleSim( );



public:
  FeS2TrafficManager( const Configuration &config, const vector<Network *> & net );
  virtual ~FeS2TrafficManager( );

};

#endif
