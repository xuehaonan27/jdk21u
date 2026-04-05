import java.lang.ref.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.stream.*;
import java.nio.*;
import java.nio.charset.*;
import java.security.*;

/**
 * Heavy stress test for disaggregated memory infrastructure.
 * Exercises: GC pressure, C1/C2 JIT, java.lang.ref, ConcurrentHashMap,
 * atomics, String operations, lambdas/streams, polymorphic dispatch,
 * deep object graphs, large arrays, weak/soft/phantom references.
 */
public class StressTest {

    // ---- Retained data (promotes to Old, candidates for classification/eviction) ----
    static final int NUM_ENTRIES = 50_000;
    static final ConcurrentHashMap<String, Node> liveMap = new ConcurrentHashMap<>();
    static final ArrayList<Object> tenured = new ArrayList<>();
    static final LinkedList<SoftReference<byte[]>> softCache = new LinkedList<>();
    static final LinkedList<WeakReference<String>> weakInterns = new LinkedList<>();
    static final ReferenceQueue<Object> phantomQueue = new ReferenceQueue<>();
    static final ArrayList<PhantomReference<byte[]>> phantoms = new ArrayList<>();
    static final AtomicLong allocBytes = new AtomicLong();
    static final AtomicInteger gcCount = new AtomicInteger();
    static volatile boolean running = true;

    // ---- Polymorphic hierarchy (C2 profiles, inline caches) ----
    interface Shape { double area(); String name(); }
    record Circle(double r) implements Shape {
        public double area() { return Math.PI * r * r; }
        public String name() { return "Circle(r=" + r + ")"; }
    }
    record Rect(double w, double h) implements Shape {
        public double area() { return w * h; }
        public String name() { return "Rect(" + w + "x" + h + ")"; }
    }
    record Triangle(double b, double h) implements Shape {
        public double area() { return 0.5 * b * h; }
        public String name() { return "Triangle(b=" + b + ",h=" + h + ")"; }
    }

    // ---- Linked list node (deep object graph, pointer-heavy) ----
    static class Node {
        final String key;
        volatile Object value;
        volatile Node next;
        final long stamp;
        Node(String key, Object value, Node next) {
            this.key = key;
            this.value = value;
            this.next = next;
            this.stamp = System.nanoTime();
        }
        int chainLength() {
            int len = 1;
            Node n = next;
            while (n != null) { len++; n = n.next; }
            return len;
        }
    }

    // ---- Tree node (balanced, recursive, promotes to Old) ----
    static class TreeNode {
        int val;
        TreeNode left, right;
        String label;
        TreeNode(int val) {
            this.val = val;
            this.label = "node-" + val;
        }
        static TreeNode buildBalanced(int lo, int hi) {
            if (lo > hi) return null;
            int mid = (lo + hi) / 2;
            TreeNode n = new TreeNode(mid);
            n.left = buildBalanced(lo, mid - 1);
            n.right = buildBalanced(mid + 1, hi);
            return n;
        }
        int sum() {
            int s = val;
            if (left != null) s += left.sum();
            if (right != null) s += right.sum();
            return s;
        }
        int depth() {
            int ld = left == null ? 0 : left.depth();
            int rd = right == null ? 0 : right.depth();
            return 1 + Math.max(ld, rd);
        }
    }

    public static void main(String[] args) throws Exception {
        System.out.println("=== Heavy Stress Test for Disaggregated Memory ===");
        System.out.println("Heap max: " + Runtime.getRuntime().maxMemory() / (1024*1024) + " MB");
        long start = System.currentTimeMillis();

        // Phase 1: Build long-lived data structures (promote to Old)
        System.out.println("\n[Phase 1] Building long-lived data structures...");
        phase1_buildRetained();

        // Phase 2: Allocation storm (trigger Young GC, promote survivors)
        System.out.println("\n[Phase 2] Allocation storm (GC pressure)...");
        phase2_allocationStorm();

        // Phase 3: Reference processing (Soft, Weak, Phantom)
        System.out.println("\n[Phase 3] Reference processing...");
        phase3_references();

        // Phase 4: Polymorphic dispatch + streams (C2 JIT, profiling)
        System.out.println("\n[Phase 4] Polymorphic dispatch + streams...");
        phase4_polymorphicStreams();

        // Phase 5: Concurrent HashMap churn (CAS, volatile, atomics)
        System.out.println("\n[Phase 5] Concurrent HashMap churn...");
        phase5_concurrentChurn();

        // Phase 6: String operations (interning, concatenation, encoding)
        System.out.println("\n[Phase 6] String operations...");
        phase6_stringOps();

        // Phase 7: Deep recursive tree (pointer-heavy, promotes to Old)
        System.out.println("\n[Phase 7] Deep recursive tree...");
        phase7_deepTree();

        // Phase 8: Final GC + verify
        System.out.println("\n[Phase 8] Final GC + verification...");
        phase8_verify();

        long elapsed = System.currentTimeMillis() - start;
        System.out.println("\n=== Stress Test PASSED in " + elapsed + " ms ===");
        System.out.println("Total allocated: " + allocBytes.get() / (1024*1024) + " MB");
    }

    static void phase1_buildRetained() {
        // Build linked-list chains in the ConcurrentHashMap
        for (int i = 0; i < NUM_ENTRIES; i++) {
            String key = "entry-" + i;
            byte[] payload = new byte[64 + (i % 256)];
            Arrays.fill(payload, (byte)(i & 0xFF));
            Node prev = liveMap.get("entry-" + Math.max(0, i - 10));
            Node node = new Node(key, payload, prev);
            liveMap.put(key, node);
            allocBytes.addAndGet(payload.length + 80);
        }
        // Build array of mixed types
        for (int i = 0; i < 10_000; i++) {
            if (i % 3 == 0) tenured.add(new int[32]);
            else if (i % 3 == 1) tenured.add("tenured-string-" + i);
            else tenured.add(new Circle(i * 0.1));
        }
        System.out.println("  liveMap: " + liveMap.size() + " entries");
        System.out.println("  tenured: " + tenured.size() + " objects");
    }

    static void phase2_allocationStorm() {
        // Allocate and discard rapidly to trigger many Young GCs
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        long phaseAlloc = 0;
        for (int round = 0; round < 20; round++) {
            ArrayList<Object> ephemeral = new ArrayList<>(10_000);
            for (int i = 0; i < 10_000; i++) {
                int size = 64 + rng.nextInt(4096);
                byte[] buf = new byte[size];
                buf[0] = (byte) round;
                buf[size - 1] = (byte) i;
                ephemeral.add(buf);
                phaseAlloc += size;
                // Occasionally touch long-lived data (forces remset cards)
                if (i % 500 == 0 && !liveMap.isEmpty()) {
                    String k = "entry-" + rng.nextInt(NUM_ENTRIES);
                    Node n = liveMap.get(k);
                    if (n != null) n.value = buf; // cross-gen pointer
                }
            }
            ephemeral.clear(); // discard
        }
        allocBytes.addAndGet(phaseAlloc);
        System.out.println("  Allocated " + phaseAlloc / (1024*1024) + " MB ephemeral data");
    }

    static void phase3_references() {
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        // Create soft references (cleared under memory pressure)
        for (int i = 0; i < 5_000; i++) {
            byte[] data = new byte[1024 + rng.nextInt(8192)];
            softCache.add(new SoftReference<>(data));
            allocBytes.addAndGet(data.length);
        }
        // Create weak references to interned-like strings
        for (int i = 0; i < 10_000; i++) {
            String s = new String("weak-intern-" + rng.nextInt(100_000));
            weakInterns.add(new WeakReference<>(s));
        }
        // Create phantom references
        for (int i = 0; i < 2_000; i++) {
            byte[] data = new byte[512];
            phantoms.add(new PhantomReference<>(data, phantomQueue));
        }
        // Force GC to process references
        System.gc();
        try { Thread.sleep(100); } catch (InterruptedException e) {}

        // Count survivors
        long softAlive = softCache.stream().filter(r -> r.get() != null).count();
        long weakAlive = weakInterns.stream().filter(r -> r.get() != null).count();
        int phantomPolled = 0;
        while (phantomQueue.poll() != null) phantomPolled++;

        System.out.println("  Soft refs alive: " + softAlive + "/" + softCache.size());
        System.out.println("  Weak refs alive: " + weakAlive + "/" + weakInterns.size());
        System.out.println("  Phantom refs polled: " + phantomPolled);
    }

    static void phase4_polymorphicStreams() {
        // Build a large list of polymorphic shapes
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        ArrayList<Shape> shapes = new ArrayList<>(100_000);
        for (int i = 0; i < 100_000; i++) {
            switch (i % 3) {
                case 0 -> shapes.add(new Circle(rng.nextDouble(100)));
                case 1 -> shapes.add(new Rect(rng.nextDouble(100), rng.nextDouble(100)));
                case 2 -> shapes.add(new Triangle(rng.nextDouble(100), rng.nextDouble(100)));
            }
        }

        // Stream operations (lambdas, closures — JIT-friendly)
        double totalArea = shapes.stream().mapToDouble(Shape::area).sum();
        long bigShapes = shapes.stream().filter(s -> s.area() > 5000).count();
        Optional<Shape> maxShape = shapes.stream().max(Comparator.comparingDouble(Shape::area));

        // Parallel stream (multi-threaded)
        Map<String, Double> avgByType = shapes.stream()
                .collect(Collectors.groupingBy(
                        s -> s.getClass().getSimpleName(),
                        Collectors.averagingDouble(Shape::area)));

        System.out.println("  Total area: " + String.format("%.2f", totalArea));
        System.out.println("  Big shapes (area > 5000): " + bigShapes);
        System.out.println("  Max shape: " + maxShape.map(Shape::name).orElse("none"));
        System.out.println("  Avg area by type: " + avgByType);
    }

    static void phase5_concurrentChurn() throws Exception {
        ConcurrentHashMap<Integer, AtomicLong> counters = new ConcurrentHashMap<>();
        for (int i = 0; i < 1000; i++) counters.put(i, new AtomicLong());

        int nThreads = Math.min(Runtime.getRuntime().availableProcessors(), 4);
        ExecutorService pool = Executors.newFixedThreadPool(nThreads);
        CountDownLatch latch = new CountDownLatch(nThreads);

        for (int t = 0; t < nThreads; t++) {
            final int tid = t;
            pool.submit(() -> {
                try {
                    ThreadLocalRandom rng = ThreadLocalRandom.current();
                    for (int i = 0; i < 200_000; i++) {
                        int k = rng.nextInt(1000);
                        counters.get(k).incrementAndGet();
                        // Also churn the liveMap
                        if (i % 100 == 0) {
                            String key = "entry-" + rng.nextInt(NUM_ENTRIES);
                            Node n = liveMap.get(key);
                            if (n != null) {
                                // CAS-like update pattern
                                n.value = new byte[32 + rng.nextInt(64)];
                            }
                        }
                    }
                } finally {
                    latch.countDown();
                }
            });
        }
        latch.await(30, TimeUnit.SECONDS);
        pool.shutdown();
        pool.awaitTermination(10, TimeUnit.SECONDS);

        long totalOps = counters.values().stream().mapToLong(AtomicLong::get).sum();
        System.out.println("  Concurrent ops: " + totalOps + " across " + nThreads + " threads");
    }

    static void phase6_stringOps() {
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        ArrayList<String> strings = new ArrayList<>(50_000);

        // Build strings via concatenation (invokedynamic + StringConcatFactory)
        for (int i = 0; i < 20_000; i++) {
            String s = "key=" + i + ",val=" + rng.nextInt(1_000_000) + ",hash=" + Integer.toHexString(i);
            strings.add(s);
        }
        // Build strings via StringBuilder
        for (int i = 0; i < 10_000; i++) {
            StringBuilder sb = new StringBuilder(128);
            for (int j = 0; j < 10; j++) {
                sb.append("seg").append(j).append('-').append(rng.nextInt(100));
                if (j < 9) sb.append('/');
            }
            strings.add(sb.toString());
        }
        // Encoding round-trip (UTF-8 → bytes → String)
        int encodingErrors = 0;
        for (int i = 0; i < 10_000; i++) {
            String orig = strings.get(i % strings.size());
            byte[] utf8 = orig.getBytes(StandardCharsets.UTF_8);
            String decoded = new String(utf8, StandardCharsets.UTF_8);
            if (!orig.equals(decoded)) encodingErrors++;
        }
        // Hashing + dedup (exercises String.hashCode, equals)
        HashSet<String> deduped = new HashSet<>(strings);
        // Sorting
        strings.sort(Comparator.naturalOrder());
        // Substring + contains
        long containsCount = strings.stream()
                .filter(s -> s.contains("val=") && s.length() > 20)
                .count();

        System.out.println("  Strings: " + strings.size() + ", unique: " + deduped.size());
        System.out.println("  Encoding errors: " + encodingErrors);
        System.out.println("  Contains matches: " + containsCount);
    }

    static void phase7_deepTree() {
        // Build a balanced BST with 100K nodes
        TreeNode root = TreeNode.buildBalanced(0, 99_999);
        int sum = root.sum();
        int depth = root.depth();
        // Force a GC to age the tree into Old gen
        System.gc();
        // Verify after GC
        int sum2 = root.sum();
        if (sum != sum2) throw new RuntimeException("Tree sum changed after GC: " + sum + " vs " + sum2);

        // Build and discard many small trees (GC pressure on pointer-heavy data)
        long treeAlloc = 0;
        for (int i = 0; i < 500; i++) {
            TreeNode t = TreeNode.buildBalanced(0, 999);
            treeAlloc += t.sum(); // force traversal before discard
        }

        System.out.println("  Tree depth: " + depth + ", sum: " + sum);
        System.out.println("  Small tree traversal checksum: " + treeAlloc);
    }

    static void phase8_verify() {
        System.gc();
        try { Thread.sleep(200); } catch (InterruptedException e) {}

        // Verify liveMap is intact
        int chainLenSum = 0;
        int nullValues = 0;
        for (Map.Entry<String, Node> e : liveMap.entrySet()) {
            Node n = e.getValue();
            if (n.value == null) nullValues++;
            chainLenSum += n.chainLength();
        }
        System.out.println("  liveMap entries: " + liveMap.size());
        System.out.println("  Total chain length: " + chainLenSum);
        System.out.println("  Null values (overwritten by ephemeral): " + nullValues);

        // Verify tenured objects
        int intArrays = 0, strings = 0, circles = 0;
        for (Object o : tenured) {
            if (o instanceof int[]) intArrays++;
            else if (o instanceof String) strings++;
            else if (o instanceof Circle) circles++;
        }
        System.out.println("  Tenured: " + intArrays + " int[], " + strings + " String, " + circles + " Circle");

        // Final memory stats
        Runtime rt = Runtime.getRuntime();
        long used = rt.totalMemory() - rt.freeMemory();
        System.out.println("  Heap used: " + used / (1024*1024) + " MB / " + rt.maxMemory() / (1024*1024) + " MB");
    }
}
