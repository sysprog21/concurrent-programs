# Quiescent-State-Based Reclamation

Designing high throughput server is hard. One of the problems we often
encounter is how to update internal data structures that sustain mostly
read traffic. 

Paul E McKenney conducted a thorough comparison of most performant
implementations within this scheme. One of them is a cooperative
algorithm called "Quiescent-State-Based Reclamation".
