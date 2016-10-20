/*
 * FeS2Interface1.cpp
 *
 *  Created on: Apr 10, 2010
 *      Author: robert
 */

#include <sstream>

#include "fes2_interface.hpp"

FeS2Interface::FeS2Interface( const Configuration &config,
		const vector<Network *> & net) {

	_trace_mode = config.GetInt("trace_mode");

	if (_trace_mode == 1) {
		_trace = new TraceGenerator();

		string trace_file_name;
		trace_file_name = config.GetStr("trace_file");

		if (!_trace->openTraceFile( trace_file_name )) {
			cerr << "Trace file cannot be opened: " << trace_file_name << endl;
		}
	}
	numOfSyn = config.GetInt("synfull_instances");
	_original_destinations.resize(numOfSyn);
	_original_source.resize(numOfSyn);
	_listenSocket.resize(numOfSyn);	
	_channel.resize(numOfSyn); 
	for(int i = 0; i < numOfSyn; i++){
		_channel[i] = NULL;
	}
	_sources = net[0]->NumNodes( );
	_dests   = net[0]->NumNodes( );
	
	_duplicate_networks.resize(numOfSyn); 
	for(int i = 0; i < numOfSyn; i++){
		_duplicate_networks[i] = config.GetInt("subnets");
	}

	_concentrate.resize(numOfSyn); 
	_concentrate[0] = config.GetInt("fes2_concentrate") ? true : false;

	_host = config.GetStr( "fes2_host");
	_port = config.GetInt("fes2_port");
	

        for(int i = 0; i < _sources; i++){
               _synfull_map[i] = -1 ; 
         }
	_node_map.resize(numOfSyn); 
	for(int i = 0; i < numOfSyn; i++){
		string s = "fes2_mapping_"; 
		s += (char)('0'+i);
		vector<int> mapping = config.GetIntArray(s);
		for(int j = 0; j < mapping.size(); j++) {
			int m = mapping[j]; 
			_node_map[i][j] = m ;
			assert(_synfull_map[m] == -1 || _synfull_map[m] == i);
			_synfull_map[m] = i ; 
			 
		}
	}

	_request_buffer.resize(numOfSyn); 
        _reply_buffer.resize(numOfSyn); 
	for(int x = 0; x < numOfSyn; x++){

		_request_buffer[x] = new queue<FeS2RequestPacket *> * [_duplicate_networks[x]];
		for (int i=0; i < _duplicate_networks[x]; i++ ) {
			_request_buffer[x][i] = new queue<FeS2RequestPacket *> [_sources];
		}
       	}

	for(int i = 0; i < numOfSyn; i++){
		synfullDone[i] = false ; 
	}

}
FeS2Interface::~FeS2Interface() {
	if (_trace_mode == 1) {
		_trace->closeTraceFile();
	}
/*
	for(int m = 0; m < _duplicate_networks.size(); m++){
		delete [] (_request_buffer[0][m]);
	}
	delete []  (_request_buffer);
*/
}


int FeS2Interface::Init() {
	for(int i = 0; i < numOfSyn; i++){
		Init(i); 
	}
	return 0 ; 
}




int FeS2Interface::Init(int sid/*synfull id*/) {
	// Start listening for incoming connections

	// TODO Configurable host/port
	// if (_listenSocket.listen(_host.c_str(), _port) < 0)

	// constants available in both BookSim and FeS2
	if (_listenSocket[sid].listen(NS_HOST, NS_PORT, sid) < 0) {
		return -1;
	}

	// Wait for FeS2 to connect
	_channel[sid] = _listenSocket[sid].accept();
	cout << "FeS2 instance "<<sid<<" is connected" << endl;
	// Initialize client
	InitializeReqMsg req;
	InitializeResMsg res;
	SocketStream * _ch;
	_ch = _channel[sid] ;
	*_ch >> req << res;

	return 0;
}

int FeS2Interface::Step() {
	bool notDone = false; 
        for(int i = 0; i < numOfSyn; i++){
		
                if( ! synfullDone[i] ){
			bool f = (Step(i) == 1);
			synfullDone[i] = f; 
			notDone = true; 
		} 
        }
        return (notDone)? 0 : 1; 

}


int FeS2Interface::Step(int sid) {
	bool process_more = true;
	StreamMessage *msg = NULL;
	SocketStream * _ch; 
        _ch = _channel[sid] ;
	while (process_more && _channel[sid] && _channel[sid]->isAlive())
	{
		// read message
		*_ch >> (StreamMessage*&) msg;


		switch(msg->type)
		{
		case STEP_REQ:
		{
			// acknowledge the receipt of step request
			// we're actually doing a little bit of work in parallel
			StepResMsg res;
			*_ch << res;

			// fall-through and perform one step of BookSim loop
			process_more = false;

			break;
		}
		case INJECT_REQ:
		{
			InjectReqMsg* req = (InjectReqMsg*) msg;

			// create packet to store in local queue
			FeS2RequestPacket* rp = new FeS2RequestPacket();
			rp->cl = req->cl;
			rp->dest = req->dest;
			rp->id = req->id;
			rp->network = req->network;
			rp->size = req->packetSize;
			rp->source = req->source;
			rp->miss_pred = req->miss_pred;


			if (_trace_mode == 1) {
				//_trace->writeTraceItem(GetSimTime(), rp->source, rp->dest,
				//		rp->size, req->address, rp->network);
				stringstream str;

				str << rp->id << " " << GetSimTime() << " " << rp->source << " "
						<< rp->dest << " " << rp->size << " " << req->msgType
						<< " " << req->coType << " " << req->address;

				_trace->writeTrace(str.str());
			}

			EnqueueFeS2RequestPacket(rp, sid);

			// acknowledge receipt of packet to FeS2
			InjectResMsg res;
			*_ch << res;

			break;
		}
		case EJECT_REQ:
		{
			EjectReqMsg* req = (EjectReqMsg*) msg;
			EjectResMsg res;
			// create packet to store in local queue
			FeS2ReplyPacket* rp = DequeueFeS2ReplyPacket(sid);

			if (rp != NULL)
			{
				res.source = rp->source;
				res.dest = rp->dest;
				res.network = rp->network;
				res.id = rp->id;
				res.cl = rp->cl;
				res.miss_pred = rp->miss_pred;

				// free up reply packet
				free(rp);
				rp = NULL;
			}
			else
			{
				res.id = -1;
			}

			res.remainingRequests = getFeS2ReplyQueueSize(sid);

			*_ch << res;

			break;
		}
		case QUIT_REQ:
		{
			// acknowledge quit
			QuitResMsg res;
			*_ch << res;

			return 1; // signal that we're done

			break;
		}
		default:
		{
			cout << "<FeS2Interface::Step> Unknown message type: "
					<< msg->type << endl;
			break;
		}
		} // end switch

		// done processing message, destroy it
		StreamMessage::destroy(msg);
	}

	return 0;
}


int FeS2Interface::EnqueueFeS2RequestPacket(FeS2RequestPacket *packet, int sid) {
	//The mapping of FeS2 devices to BookSim nodes is done here

	// We always need to store the original destination send it to the correct

	// FeS2 queue
	_original_destinations[sid][packet->id] = packet->dest;
	_original_source[sid][packet->id] = packet->source;
	//_node_map is based off of the configuration file. See "fes2_mapping"
	packet->source 	= _node_map[sid][packet->source];
	packet->dest 	= _node_map[sid][packet->dest];

	// special case: single network
	if (_duplicate_networks[sid] == 1) {
		_request_buffer[sid][0][packet->source].push(packet);
	} else {
		assert (packet->network < _duplicate_networks[sid]);
		_request_buffer[sid][packet->network][packet->source].push(packet);
	}
	return 0;
}

FeS2RequestPacket *FeS2Interface::DequeueFeS2RequestPacket(int source, int network, int cl) {
	return DequeueFeS2RequestPacket(source, network, cl, 0);
}

FeS2RequestPacket *FeS2Interface::DequeueFeS2RequestPacket(int source,
		int network, int cl, int sid) {
	FeS2RequestPacket *packet = NULL;

	if (!_request_buffer[sid][network][source].empty()) {
		packet = _request_buffer[sid][network][source].front();
		if (packet->cl == cl) {
			_request_buffer[sid][network][source].pop();
		} else {
			packet = 0;
		}
	}

	return packet;
}

int FeS2Interface::EnqueueFeS2ReplyPacket(FeS2ReplyPacket *packet) {
	return EnqueueFeS2ReplyPacket(packet, 0); 

}

int FeS2Interface::EnqueueFeS2ReplyPacket(FeS2ReplyPacket *packet, int sid) {
	assert(_original_destinations[sid].find(packet->id) !=
			_original_destinations[sid].end());
        assert(_original_source[sid].find(packet->id) !=
                        _original_source[sid].end());
	if (_concentrate[sid]) {
		assert(_original_destinations[sid].find(packet->id) !=
				_original_destinations[sid].end());
		//cout<<_original_destinations[sid][packet->id]/2<<", "<<_original_destinations[sid][packet->id]<<", "<<packet->dest<<endl;
		//assert(_original_destinations[sid][packet->id]/2 == packet->dest);
		//packet->source *= 2;
	}

	packet->dest = _original_destinations[sid][packet->id];
        packet->source = _original_source[sid][packet->id] ; 
	_original_destinations[sid].erase(packet->id);
        _original_source[sid].erase(packet->id);


	_reply_buffer[sid].push(packet);

	return 0;
}

FeS2ReplyPacket *FeS2Interface::DequeueFeS2ReplyPacket(int sid) {
	FeS2ReplyPacket *packet = NULL;

	if (!_reply_buffer[sid].empty()) {
		packet = _reply_buffer[sid].front();
		_reply_buffer[sid].pop();
	}

	return packet;
}

int FeS2Interface::GenerateTestPackets() {
	return 0;
}


int FeS2Interface::GetSynfullID(int s){
	//s may be source or desitnation of a packet. 
	if(_synfull_map[s] == -1){
		assert("Synfull ID Mapping Error!" && false);
		return 0; 
	}
	return _synfull_map[s]; 
}
