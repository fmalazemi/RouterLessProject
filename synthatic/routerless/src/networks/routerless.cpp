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

/*RouterLess
 *
 *Network setup file format
 *example 1:
 *router 0 router 1 15 router 2
 *
 *Router 0 is connect to router 1 with a 15-cycle channel, and router 0 is connected to
 * router 2 with a 1-cycle channel, the channels latency are unidirectional, so channel
 * from router 1 back to router 0 is only single-cycle because it was not specified
 *
 *example 2:
 *router 0 node 0 node 1 5 node 2 5
 *
 *Router 0 is directly connected to node 0-2. Channel latency is 5cycles for 1 and 2. In
 * this case the latency specification is bidirectional, the injeciton and ejection lat
 * for node 1 and 2 are 5-cycle
 *
 *other notes:
 *
 *Router and node numbers must be sequential starting with 0
 *Credit channel latency follows the channel latency, even though it travels in revse
 * direction this might not be desired
 *
 */

#include "routerless.hpp"
#include <fstream>
#include <sstream>
#include <limits>
#include <algorithm>

RouterLess::RouterLess( const Configuration &config, const string & name )
  :  Network( config, name ){

  _ComputeSizeRL( config );
  _Alloc();
  _BuildNetRL(config);

}

RouterLess::~RouterLess(){
  for(int i = 0; i < ring.size(); i++){
    ring[i].clear();
  }
  ring.clear();

  for(int i = 0; i < shortestRing.size(); i++){
    for(int j = 0; j < shortestRing[i].size(); j++){
      shortestRing[i][j].clear();
    }
    shortestRing[i].clear();
  }
  shortestRing.clear();

}
void RouterLess::_ComputeSize( const Configuration &config ){
  cout<<"ERROR: _compateSize() in RouterLess.cpp!"<<endl;
  exit(0);

};
void RouterLess::_BuildNet( const Configuration &config ){
  cout<<"ERROR: _BuildNetRL() in RouterLess.cpp!"<<endl;
  exit(0);

}

void RouterLess::_ComputeSizeRL( const Configuration &config ){
  file_name = config.GetStr("network_file");
  if(file_name==""){
    cout<<"No network file name provided"<<endl;
    exit(-1);
  }
  //parse the network description file
  readFileRL();

}

void RouterLess::readRingsRL(ifstream & in /*input file*/){
  string line ;
  getline(in,line);
  int n = atoi(line.c_str());
  ring.resize(n+1);
  for(int i = 0; i < n; i++){
    getline(in,line);
    replace(line.begin(), line.end(), ':', ' ');
    replace(line.begin(), line.end(), '[', ' ');
    replace(line.begin(), line.end(), ']', ' ');
    replace(line.begin(), line.end(), ',', ' ');
    stringstream str;
    str<<line;


    int id, node_id;
    char ringType; 
    str>>ringType; 

    str>>id ; //ring id
    if(ringType == 'v') {  
     vRing.push_back(id);
     cout<<"v --> "<<id<<endl;} 
    else if(ringType == 'h') {  
        hRing.push_back(id);
        cout<<"h---> "<<id<<endl;}



    while(str>>node_id){
      ring[id].push_back(node_id);
    }
    
  }
}

void RouterLess::readShortestRingsRL(ifstream & in /*input file*/){
	string line ;
	getline(in,line);
	int n = atoi(line.c_str());
	shortestRing.resize(n);
	for(int i = 0; i < n; i++){
		shortestRing[i].resize(n);

		getline(in,line);
		int src = atoi(line.c_str());
		bool x[ring.size()];
		for(int i = 0; i < ring.size(); i++){
			x[i] = false;
		}

		for(int j = 0; j < n; j++){

			getline(in,line);
			replace(line.begin(), line.end(), ':', ' ');
			replace(line.begin(), line.end(), '[', ' ');
			replace(line.begin(), line.end(), ']', ' ');
			replace(line.begin(), line.end(), ',', ' ');
			stringstream str;
			str<<line;

			int dest, ring_id;
			str>>dest ; //ring id

			while(str>>ring_id){
				shortestRing[src][dest].push_back(ring_id);
				x[ring_id] = true;
			}

			//random_shuffle(shortestRing[src][dest].begin(), shortestRing[src][dest].end());

		}
		for(int i = 0; i < ring.size(); i ++){
			if(x[i] == false){
				continue;
			}
			shortestRing[src][src].push_back(i);
		}

	}
}
int RouterLess::countChannelsRL(){
  int count = 0;
  for(int i = 0; i < ring.size(); i++){
    count += ring[i].size();

  }
  cout<<"The number of channels = "<<count<<endl;

  return count;
}


void RouterLess::readFileRL(){

  ifstream in;//input file
  string line;



  in.open(file_name.c_str());
  if(!in.is_open()){
    cout<<"RouterLess:can't open network file "<<file_name<<endl;
    exit(-1);
  }

  readRingsRL(in);
  readShortestRingsRL(in);

  _size = shortestRing.size(); //number of routers
  _nodes = shortestRing.size(); // number of cores
  _channels = countChannelsRL();


}

int RouterLess::_countPassingRingsRL(int node){

  set<int> s;

  for(int dest = 0; dest < shortestRing[node].size(); dest++){
    for(int j = 0; j < shortestRing[node][dest].size(); j++){
      s.insert(shortestRing[node][dest][j]);
    }

  }
  return s.size();

}


void RouterLess::_BuildNetRL(const Configuration &config){

  cout<<"=========================================================="<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"============  {---- Ring Configuration ----} ============"<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"==========================================================\n\n\n"<<endl;

  cout<<"------------------ [ Router-to-Node ] -------------------"<<endl;

  for(int node = 0; node < shortestRing.size(); node++){

    int radix = _countPassingRingsRL(node) + 1; //+1 to core
    //every ring need two channels to pass through a node.

    ostringstream router_name;
    router_name << "router";
    router_name << "_" <<  node ;


    _routers[node] = Router::NewRouter( config, this, router_name.str(), node, radix, radix);
    _routers[node]->setAllRings(vRing, hRing, ring); 
    _routers[node]->setRL(shortestRing[node]);
    
    _timed_modules.push_back(_routers[node]);


    _inject[node]->SetLatency(1);
    _inject_cred[node]->SetLatency(1);

    _inject[node]->SetRingID(-1);
    _inject_cred[node]->SetRingID(-1);

    _routers[node]->AddInputChannel(_inject[node], _inject_cred[node]);


    _eject[node]->SetLatency(1);
    _eject_cred[node]->SetLatency(1);

    _eject[node]->SetRingID(-1);
    _eject_cred[node]->SetRingID(-1);

    _routers[node]->AddOutputChannel(_eject[node], _eject_cred[node]);

    cout<<_routers[node]->GetID()<<" -to- "<<node<<endl;


  }

  int curChannel = 0;
  cout<<"\n\n------------------- [ Rings List ] --------------------\n\n"<<endl;

  for(int rID = 1; rID < ring.size(); rID++){//ring zero is empty. Ignore.
      cout<<"Ring iD = "<<rID<<endl;
      assert(ring[rID].size() > 1);
      cout<<"Ring ID = "<<rID <<endl;
      cout<<"Ring List = [";

      for(int i = 0; i < ring[rID].size()-1; i++){

        int node = ring[rID][i];
        int nextNode = ring[rID][i+1];

        _chan[curChannel]->SetLatency(1);
        _chan_cred[curChannel]->SetLatency(1);


        _chan[curChannel]->SetRingID(rID);
        _chan_cred[curChannel]->SetRingID(rID);
        _routers[node]->AddOutputChannel( _chan[curChannel], _chan_cred[curChannel] );
        _routers[nextNode]->AddInputChannel( _chan[curChannel], _chan_cred[curChannel]);

        curChannel++;
        cout<<_routers[node]->GetID()<<", ";

      }


      int node = ring[rID][ring[rID].size()-1];
      cout<<_routers[node]->GetID()<<" ]"<<endl;

      int nextNode = ring[rID][0];
      _chan[curChannel]->SetLatency(1);
      _chan_cred[curChannel]->SetLatency(1);

      _chan[curChannel]->SetRingID(rID);
      _chan_cred[curChannel]->SetRingID(rID);

      _routers[node]->AddOutputChannel( _chan[curChannel], _chan_cred[curChannel] );
      _routers[nextNode]->AddInputChannel( _chan[curChannel], _chan_cred[curChannel]);
      curChannel++;

  }

  for(int i = 0; i < _routers.size(); i++){
    cout<<"At router "<<_routers[i]->GetID()<<" = "<<i<<endl;
    for(int j = 1; j < _routers[i]->NumInputs(); j++){
        FlitChannel * x = _routers[i]->GetInputChannel(j);
        Router const * source = x->GetSource();
        Router const * dest = x->GetSink();
        cout<<"input   "<<j<<" is a channel to "<<dest->GetID()<<" to "<<source->GetID()<<" (Ring "<<x->GetRingID()<<")"<<endl;
    }
    for(int j = 1; j < _routers[i]->NumOutputs(); j++){
        FlitChannel * x = _routers[i]->GetOutputChannel(j);
        Router const * source = x->GetSource();
        Router const * dest = x->GetSink();
        cout<<"Output  "<<j<<" is a channel from "<<source->GetID()<<" to "<<dest->GetID()<<" (Ring "<<x->GetRingID()<<")"<<endl;

    }
  }



  cout<<"\n\n====================  {---- END ----} ====================\n\n"<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"============  {---- Starting Simulation ----} ============"<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"==========================================================\n\n\n"<<endl;



}





void RouterLess::RegisterRoutingFunctions() {

  gRoutingFunctionMap["min_routerless"] = &min_RouterLess;

}

void min_RouterLess( const Router *r, const Flit *f, int in_channel,
	OutputSet *outputs, bool inject ){

  //This is a dummy router function. The output port for any case is 0.
  //Routing is calculated inside the router.

  int out_port=-1;
  if(!inject){
    out_port = 0;
  //  cout<<"pid = "<<f->pid<<", Src = "<<f->src<<", dest = "<<f->dest<<endl;
    if(f->dest != r->GetID()){
      out_port = (f->id%2)+1;
    }
    out_port = 0;
  }


  int vcBegin = 0, vcEnd = gNumVCs-1;
  if ( f->type == Flit::READ_REQUEST ) {
    vcBegin = gReadReqBeginVC;
    vcEnd   = gReadReqEndVC;
  } else if ( f->type == Flit::WRITE_REQUEST ) {
    vcBegin = gWriteReqBeginVC;
    vcEnd   = gWriteReqEndVC;
  } else if ( f->type ==  Flit::READ_REPLY ) {
    vcBegin = gReadReplyBeginVC;
    vcEnd   = gReadReplyEndVC;
  } else if ( f->type ==  Flit::WRITE_REPLY ) {
    vcBegin = gWriteReplyBeginVC;
    vcEnd   = gWriteReplyEndVC;
  }

  outputs->Clear( );

  outputs->AddRange( out_port , vcBegin, vcEnd );
}
