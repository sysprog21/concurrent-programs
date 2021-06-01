# Quiescent-State-Based Reclamation

Designing high throughput server is hard. One of the problems we often
encounter is how to update internal data-structure that sustain mostly
read traffic. 

Tom Hard and PE McKenney conducted a thorough comparison of most
performant implementations within this scheme. One of them is a
cooperative algorithm called "Quiescent-State-Based Reclamation".
