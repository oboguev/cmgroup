# cmgroup
IO completion port for Linux (toy project)

This was a toy project to try re-implementing Windows IO completion port
facility under Linux and to assess if there is any real performance benefit
still left to the completion ports idea. 

Benchmarking indicated any benefit at all is achievable only in a very narrow
class of use cases, and is minor even then, and would require a somewhat
invasive changes to the scheduler guts even in that case, so overall not worth
bothering.

