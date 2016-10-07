We allowed packets of src == dest (by reverting traffic.cpp of its original) to be injected in the network. 
Such packets are pushed through any ring attached to the src router. 
They will loop through the ring and eventually get back to the source=dest. 


