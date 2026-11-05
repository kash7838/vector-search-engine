### Cosine Similarity (The "Angle" Between Vectors)
- --

- Imagine you are trying to figure out how similar two documents or sentences are. If you convert them into numbers (vectors), you get a point in multi-dimensional space (think of an arrow pointing from the origin (0, 0) to coordinates like (3, 4)).
    - **Euclidean Distance** measures the physical length between the tips of two arrows.
    - **Cosine Similarity** measures the angle ($\theta$) between two arrows, regardless of their length.

#### Why Cosine?
- --

- In text and data analysis, length can be misleading (e.g., a long document uses the same words more often, making its vector numbers larger, even if it's about the exact same topic as a short document). Cosine similarity focuses purely on direction:
    - **Angle = 0° (Cosine = 1)**: The vectors point in the exact same direction. They are identical in meaning or content.
    - **Angle = 90° (Cosine = 0):** The vectors are completely unrelated.
    - **Angle = 180° (Cosine = -1):** They are exact opposites.

### Top-$K$ Retrieval (Finding the Best Matches)
- --

- When a user runs a search query, your database might contain millions of vectors. Calculating the cosine similarity against every single one gives you millions of scores.
- You don't want to show the user millions of results—you only want the best $K$ results (e.g., Top-5 or Top-10 most similar vectors).

#### How It Works Efficiently:
- --

- **The Scan**: As your program loops through the database vectors and computes their cosine similarity score against the query vector, it needs a way to keep track of the winners.
- **The Podium/Heap Pattern**: Instead of sorting all millions of scores (which takes $O(N \log N)$ time and is slow), you maintain a small tracking structure (like a priority queue or a mini podium of size $K$).
- **The Result**: By the time the loop finishes scanning the file, your podium holds the absolute highest $K$ scores in $O(N \log K)$ time.

- --