## Notes
- --

In modern AI applications (like the research agents or RAG pipelines), text, images, and data are converted into arrays of numbers called vectors (or embeddings). A vector search engine takes a query vector and searches a database to find the vectors that are mathematically closest or most similar to it.

### Mini-project breaks down under the hood
- --

1. **The Vector Database (Binary Storage)**: Instead of storing data in a messy CSV or JSON file, the program reads and writes a dense, raw binary file containing thousands of high-dimensional vectors (e.g., arrays of floats like [0.12, -0.43, 0.89, -0.23]).

2. **Memory Mapping (mmap)**: Normally, loading a huge file means reading it entirely into RAM using heavy IO operations. With memory mapping, the operating system treats the file on the disk as if it were already in memory. You can access vectors using direct memory pointers, and the OS handles loading and caching parts of the file behind the scenes. This is how high-performance databases and search engines handle massive datasets efficiently.

3. **Similarity Scoring**: For every vector in the database, the engine computes the **Cosine Similarity** (measuring the angle between two vectors to see how closely aligned they are) using fast loops.

4. **Top-$K$ Retrieval**: It tracks and returns the top matching results sorted by score.

