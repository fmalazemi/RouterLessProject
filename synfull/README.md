In mesh and routerless, trafficmanager.cpp is customized to connect to synfull only. No other trafficmanager is allowed. 
To run mesh or routerless for synthatic traffic, check synthatic folder at the root.


## Running multiple synfull instances
To run multiple instances of synfull, first define number of instance in booksim configuration file (synfull_instances), then define mapping fes2_mapping_x where x is the instance number and x starts from 0. 
Note that the instances always starts from zero  and end in < synfull_instances. 
Moreover, You must start booksim first and then instance_0, instance_1, ... , and instance_x in order where x = synfull_instances-1. 
Booksim connects to synfull instance y through socket ./sockety. 
I added addtional parameter to synfull to let it self determine instance number. For Example ./tgen ModelFile 50000 0 x where x is the instance number. 

    
