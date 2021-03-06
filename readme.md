In the last years the time of easy performance gaining is ended due to a physical constraint called Power Wall. In this scenario, the large diffusion of shared-memory multi-core machines offers a new opportunities to face the increasing demand for improved performance. Anyhow, common lock-based synchronization techniques could be deleterious for performances and frustrate the presence of the increasing number of cores.
In order to optimize parallel execution, the non-blocking synchronization paradigm, based on the exploitation of Read-Modify-write (RMW) instructions, was born. This new technique requires a deep knowledge of the underlying hardware capabilities and on the assumptions that can be made on it.
On the other hand, the memory allocation problem is still relevant to support fast execution of both system and user applications.
An efficient allocator is required to avoid memory requests to become a bottleneck in high-performance scenarios characterized by large amount of processes. Moreover, requests could be very different depending on the target application; a good memory allocator has to works well in as many as possible scenarios.
In this work I present the design and the implementation of a lock-free buddy system memory allocator based on a binary tree structure. The proposed algorithm offers great performances in highly parallel machines and it is very memory efficient, producing low data overhead to work.
The results obtained in the experiments confirm the actual scalability of this proposal and the effectiveness of the lock-free synchronization.


Thesis could be found here: https://drive.google.com/file/d/1ejyvYIf5IWRBcRD3X2Oj7NFAU-sZNv9K/view?usp=sharing
