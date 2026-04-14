/**
 * MicroBench — small GC-heavy workload for LIBAPTH overhead benchmarking.
 *
 * Target: ~30s on stock JDK at localrate=100.
 * Heap: ~2GB working set, triggers frequent Young + occasional Mixed GC.
 * Workload: HashMap churn + object graph traversal + allocation storms.
 *
 * Usage: java -Xmx4g -XX:+UseG1GC MicroBench
 */
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class MicroBench {
    static final int MAP_SIZE = 1_000_000;      // Long-lived map entries (~1.5GB)
    static final int CHURN_ROUNDS = 100;       // Churn iterations
    static final int CHURN_OPS = 200_000;      // Ops per churn round
    static final int ALLOC_ROUNDS = 50;        // Allocation storm rounds
    static final int ALLOC_PER_ROUND = 100_000;// Allocations per round
    static final int TREE_SIZE = 2_000_000;    // Tree nodes
    static final int THREADS = 8;              // Concurrent threads

    // Long-lived data (promotes to Old)
    static ConcurrentHashMap<Integer, Object[]> liveMap = new ConcurrentHashMap<>();
    static volatile Object[] treeRoot;

    public static void main(String[] args) throws Exception {
        long start = System.currentTimeMillis();
        System.out.println("=== MicroBench: GC-heavy workload for LIBAPTH benchmarking ===");
        System.out.printf("Heap max: %d MB%n", Runtime.getRuntime().maxMemory() / (1024*1024));

        // Phase 1: Build long-lived data (~1GB)
        phase1_build();

        // Phase 2: Allocation storms (GC pressure)
        phase2_alloc_storm();

        // Phase 3: Concurrent HashMap churn
        phase3_concurrent_churn();

        // Phase 4: Deep tree + traversal
        phase4_tree();

        // Phase 5: Mixed workload (churn + allocate + traverse)
        phase5_mixed();

        // Verification
        long elapsed = System.currentTimeMillis() - start;
        System.out.printf("liveMap: %d entries, tree depth check: OK%n", liveMap.size());
        System.out.printf("=== MicroBench PASSED in %d ms ===%n", elapsed);
    }

    static void phase1_build() {
        long t = System.currentTimeMillis();
        for (int i = 0; i < MAP_SIZE; i++) {
            Object[] chain = new Object[5];
            chain[0] = new byte[32];
            chain[1] = String.valueOf(i);
            chain[2] = new int[]{i, i+1, i+2};
            chain[3] = new double[]{i * 0.1};
            chain[4] = new Object[]{chain[0], chain[1]};
            liveMap.put(i, chain);
        }
        System.out.printf("[Phase 1] Built %d map entries in %d ms%n",
                          MAP_SIZE, System.currentTimeMillis() - t);
    }

    static void phase2_alloc_storm() {
        long t = System.currentTimeMillis();
        long totalBytes = 0;
        for (int r = 0; r < ALLOC_ROUNDS; r++) {
            Object[] temp = new Object[ALLOC_PER_ROUND];
            for (int i = 0; i < ALLOC_PER_ROUND; i++) {
                temp[i] = new byte[64 + (i % 256)];
                totalBytes += 64 + (i % 256);
            }
            // Keep some alive briefly to promote
            if (r % 5 == 0) {
                for (int i = 0; i < 1000; i++) {
                    liveMap.put(MAP_SIZE + r * 1000 + i, new Object[]{temp[i]});
                }
            }
        }
        System.out.printf("[Phase 2] Allocated %d MB in %d ms%n",
                          totalBytes / (1024*1024), System.currentTimeMillis() - t);
    }

    static void phase3_concurrent_churn() throws Exception {
        long t = System.currentTimeMillis();
        ExecutorService pool = Executors.newFixedThreadPool(THREADS);
        AtomicLong ops = new AtomicLong();
        List<Future<?>> futures = new ArrayList<>();

        for (int thread = 0; thread < THREADS; thread++) {
            final int tid = thread;
            futures.add(pool.submit(() -> {
                Random rng = new Random(tid);
                for (int r = 0; r < CHURN_ROUNDS; r++) {
                    for (int i = 0; i < CHURN_OPS; i++) {
                        int key = rng.nextInt(MAP_SIZE);
                        Object[] val = liveMap.get(key);
                        if (val != null && rng.nextInt(10) == 0) {
                            // Replace with new allocation
                            Object[] newVal = new Object[5];
                            newVal[0] = new byte[32];
                            newVal[1] = String.valueOf(key);
                            newVal[2] = val[2];
                            newVal[3] = val[3];
                            newVal[4] = new Object[]{newVal[0], newVal[1]};
                            liveMap.put(key, newVal);
                        }
                        ops.incrementAndGet();
                    }
                }
            }));
        }
        for (Future<?> f : futures) f.get();
        pool.shutdown();
        System.out.printf("[Phase 3] %d concurrent ops in %d ms%n",
                          ops.get(), System.currentTimeMillis() - t);
    }

    static void phase4_tree() {
        long t = System.currentTimeMillis();
        Object[][] tree = new Object[TREE_SIZE][];
        for (int i = 0; i < TREE_SIZE; i++) {
            tree[i] = new Object[3]; // value, left, right
            tree[i][0] = new byte[16];
            if (i > 0) {
                int parent = (i - 1) / 2;
                if (i % 2 == 1) tree[parent][1] = tree[i];
                else tree[parent][2] = tree[i];
            }
        }
        treeRoot = tree[0];

        // Traverse
        long sum = 0;
        ArrayDeque<Object[]> stack = new ArrayDeque<>();
        stack.push(tree[0]);
        while (!stack.isEmpty()) {
            Object[] node = stack.pop();
            sum += ((byte[])node[0]).length;
            if (node[1] != null) stack.push((Object[])node[1]);
            if (node[2] != null) stack.push((Object[])node[2]);
        }
        System.out.printf("[Phase 4] Tree: %d nodes, traversal sum=%d in %d ms%n",
                          TREE_SIZE, sum, System.currentTimeMillis() - t);
    }

    static void phase5_mixed() throws Exception {
        long t = System.currentTimeMillis();
        ExecutorService pool = Executors.newFixedThreadPool(THREADS);
        AtomicInteger gcCount = new AtomicInteger();
        List<Future<?>> futures = new ArrayList<>();

        for (int thread = 0; thread < THREADS; thread++) {
            final int tid = thread;
            futures.add(pool.submit(() -> {
                Random rng = new Random(tid + 100);
                for (int r = 0; r < 10; r++) {
                    // Allocate + churn + traverse
                    Object[] temp = new Object[10000];
                    for (int i = 0; i < 10000; i++) {
                        temp[i] = new byte[128];
                        int key = rng.nextInt(MAP_SIZE);
                        liveMap.get(key);
                    }
                    // Trigger GC
                    if (tid == 0 && r % 3 == 0) {
                        System.gc();
                        gcCount.incrementAndGet();
                    }
                }
            }));
        }
        for (Future<?> f : futures) f.get();
        pool.shutdown();
        System.out.printf("[Phase 5] Mixed workload, %d explicit GCs in %d ms%n",
                          gcCount.get(), System.currentTimeMillis() - t);
    }
}
